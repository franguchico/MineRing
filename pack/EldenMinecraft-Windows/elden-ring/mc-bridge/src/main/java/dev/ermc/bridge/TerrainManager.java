package dev.ermc.bridge;

import dev.ermc.bridge.link.GameState;
import dev.ermc.bridge.link.ErLink;
import dev.ermc.bridge.link.Protocol;
import dev.ermc.bridge.coop.CoopSession;
import dev.ermc.bridge.coop.CoopBodyGuard;
import dev.ermc.bridge.coop.CoopProtocol;
import dev.ermc.bridge.coop.PeerState;
import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.Mth;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.storage.LevelResource;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.io.Reader;
import java.io.Writer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;

/**
 * Server side of the bridge (runs on the integrated server thread).
 *
 * <p>Owns the host game (Elden Ring)&lt;-&gt;Minecraft anchor and keeps invisible terrain blocks
 * under and around the players. Terrain comes from the host game itself: batches of downward
 * rays are cast against its collision geometry (by the DLL, on the host game's thread) and
 * every hit becomes a column of {@link TerrainBlock}s whose top matches the host game's ground
 * to 1/16 of a block. Unanswered ground queries hold the player at its last safe position;
 * only a completed query can establish that a column has no ground.
 */
public final class TerrainManager {
	private TerrainManager() {
	}

	private static final Logger LOG = LoggerFactory.getLogger("erbridge");
	/** Only worlds created by the bridge are touched, never the player's normal worlds. */
	public static final String BRIDGE_LEVEL_NAME = "ER Bridge";
	private static final String ANCHOR_FILE = "erbridge-anchor.properties";

	/** Columns within this many blocks of a player are kept sampled. */
	private static final int SAMPLE_RADIUS = 24;
	private static final int COLUMN_CACHE_LIMIT = 16384;
	private static final int[][] PLAYER_SAMPLE_OFFSETS = TerrainScan.offsets(SAMPLE_RADIUS);
	/**
	 * Rays start this far above the player's feet and reach this far below them. Just over head
	 * height: from 4 m, the ray under a doorway landed on top of its arch, which became a pillar at
	 * head height with no floor under it (the chapel door). Ground rising higher than
	 * this is found by the high ray.
	 */
	private static final double RAY_ABOVE = TerrainSampling.RAY_ABOVE;
	private static final double RAY_BELOW = TerrainSampling.RAY_BELOW;
	/** Solid terrain thickness below the host game's surface. */
	private static final int THICKNESS = 2;
	/** Obstacle probes: horizontal rays across each column at this height above the player's feet. */
	private static final double PROBE_HEIGHT = TerrainSampling.PROBE_HEIGHT;
	/** Obstacles fill their column up to this far above the player's feet. */
	private static final double WALL_HEIGHT = 3.0;
	/** Two ground rays and four horizontal obstacle probes, including the column's diagonals. */
	private static final int RAYS_PER_COLUMN = TerrainSampling.RAYS_PER_COLUMN;
	private static final int BATCH = TerrainSampling.BATCH_RAYS;
	/** High ray: finds ground that rises above the player's head (hills, cliffs ahead). */
	private static final double RAY_HIGH = TerrainSampling.RAY_HIGH;
	/** Hills/cliffs above the player are filled at most this far above the player's feet. */
	private static final double CLIFF_FILL = 6.0;

	/**
	 * Collision filter for terrain rays (Param::setAttr a/b/c), null = the host game's default
	 * (everything). Elden Ring's equivalent of the MHW hunter-movement-mesh filter (which
	 * excluded the invisible walls that only stop monsters, the Palico or the camera) is TBD,
	 * so this starts unfiltered; use the "terrain filter ..." dev command to set one.
	 */
	private static volatile int[] rayFilter = null;
	/** Surfaces with any of these attribute bits never count as walls. */
	private static volatile int wallIgnoreMask;
	private static volatile boolean resetRequested;
	private static final java.util.Map<Integer, Integer> GROUND_ATTRS = new java.util.HashMap<>();
	private static final java.util.Map<Integer, Integer> WALL_ATTRS = new java.util.HashMap<>();
	private static long lastAttrLog;

	/** What was placed in a column: terrain blocks from bottom..top (inclusive). */
	private record Column(double sampleY, int top, int bottom, double surfaceY, double supportY, boolean flight,
			int cleanupBottom, int cleanupTop, List<Column> retained) {
		Column(double sampleY, int top, int bottom, double surfaceY, double supportY, boolean flight) {
			this(sampleY,top,bottom,surfaceY,supportY,flight,bottom,top,List.of());
		}
		Column(double sampleY, int top, int bottom, double surfaceY, double supportY, boolean flight, List<Column> retained) {
			this(sampleY,top,bottom,surfaceY,supportY,flight,lowest(bottom,retained),highest(top,retained),List.copyOf(retained));
		}
		private static int lowest(int bottom,List<Column> retained) {
			int result=bottom;
			for (Column c:retained) result=result==Integer.MIN_VALUE ? c.bottom() : Math.min(result,c.bottom());
			return result;
		}
		private static int highest(int top,List<Column> retained) {
			int result=top;
			for (Column c:retained) result=Math.max(result,c.top());
			return result;
		}
	}

	private static final GameState STATE = new GameState();
	/** Anchors per host-game zone id; each zone gets its own Minecraft region. */
	private static final java.util.Map<Integer, CoordMap.Mapping> ANCHORS = new java.util.HashMap<>();
	/** Anchor from before per-zone regions existed; adopted by the first zone seen. */
	private static CoordMap.Mapping legacyAnchor;
	/**
	 * Recalls: requests to put every player next to the Tarnished (world opened, control back
	 * from Elden Ring, Minecraft respawn, Elden Ring placed the Tarnished anew). A generation
	 * number rather than a flag, so the client can tell whether the latest request was served
	 * without racing the server thread.
	 */
	private static final java.util.concurrent.atomic.AtomicInteger RECALL_GEN = new java.util.concurrent.atomic.AtomicInteger();
	private static volatile int recallDone = -1;
	private static volatile long recallDoneAtMs;
	/** Last Elden Ring "life" (Protocol.H_HOST_LIFE) seen; a change means the Tarnished was placed anew. */
	private static int lastHostLife = Integer.MIN_VALUE;
	private static final TerrainCache<Column> COLUMNS = new TerrainCache<>(COLUMN_CACHE_LIMIT);
	private static long skippedUnloadedColumns;
	private static long applyNanos, applyWorstNanos;
	private static int applyBatches;
	private static final java.util.Map<java.util.UUID, TerrainSampling.GroundGuard> GROUND_GUARDS = new java.util.HashMap<>();
	private static final java.util.Map<java.util.UUID, TerrainSampling.Motion> MOTION = new java.util.HashMap<>();
	private static final java.util.Map<java.util.UUID, Boolean> FLIGHT_MODE = new java.util.HashMap<>();
	private static final java.util.Map<java.util.UUID, Vec3> CRITICAL_GROUND = new java.util.HashMap<>();
	/** Ingress gate is separate from ordinary local movement protection. Queries never mutate it. */
	private static final TerrainAdmission<java.util.UUID,Vec3> ADMISSION = new TerrainAdmission<>();
	private static final java.util.Map<java.util.UUID, TerrainScan.Cursor> SCAN_CURSORS = new java.util.HashMap<>();
	private static int samplePlayer;
	private static TerrainWorkBudget tickBudget;
	private static ServerLevel samplingLevel;
	private static Vec3 lastMovementCorrection;
	private static final java.util.Map<java.util.UUID, Vec3> COOP_HOLD = new java.util.HashMap<>();
	private static final java.util.Map<java.util.UUID, Vec3> BOOTSTRAP_HOLD = new java.util.HashMap<>();
	private static final java.util.Map<java.util.UUID, Vec3> COOP_CORRECTIONS = new java.util.HashMap<>();
	private static boolean bridgeWorld;

	// Ray batch in flight
	private static int pendingSeq = -1;
	private static long pendingSince;
	private static TerrainSampling.BatchContext<CoordMap.Mapping> pendingContext;
	private record PendingColumn(long key, double sampleY, double below, boolean flight, TerrainCache.Revision revision) {
	}
	private static final List<PendingColumn> PENDING_COLUMNS = new ArrayList<>();
	private static final float[] RAYS = new float[BATCH * 6];
	private static final float[] HITS = new float[BATCH * 6];
	private static final int[] HIT_FLAGS = new int[BATCH];
	private static final int[] HIT_ATTRS = new int[BATCH];
	private static boolean raysAnswered;
	private static boolean pendingWarned;
	private static boolean hitsRead;
	private static int applyingColumn;
	private static ColumnApplication application;
	private static long tickNanos, tickWorstNanos;
	private static int terrainTicks;

	public static boolean isBridgeWorld() {
		return bridgeWorld;
	}

	public static void reset() {
		bridgeWorld = false;
		COLUMNS.clear();
		skippedUnloadedColumns = 0;
		applyNanos = applyWorstNanos = 0;
		applyBatches = 0;
		discardPending();
		BATCH_KEYS.clear();
		RESAMPLE.clear();
		passages = new double[0];
		passageIds.clear();
		lastPassageReadMs = 0;
		raysAnswered = false;
		GROUND_GUARDS.clear();
		MOTION.clear();
		FLIGHT_MODE.clear();
		CRITICAL_GROUND.clear();
		ADMISSION.clear();
		SCAN_CURSORS.clear();
		samplePlayer = 0; samplingLevel=null;
		tickNanos = tickWorstNanos = 0; terrainTicks = 0;
		lastMovementCorrection = null;
		COOP_HOLD.clear();
		BOOTSTRAP_HOLD.clear();
		COOP_CORRECTIONS.clear();
		ANCHORS.clear();
		legacyAnchor = null;
		CoordMap.set(null);
	}

	/** Bounded delayed refresh areas (a struck prop may have broken, a door opened). */
	private static final TerrainRefreshQueue RESAMPLE = new TerrainRefreshQueue();

	/**
	 * The player punched Elden Ring terrain at {@code pos}: strike the looked-at point there (the DLL
	 * fires the player's attack at it, which breaks breakable objects), then resample around it.
	 */
	public static void strike(net.minecraft.world.entity.player.Player player, BlockPos pos) {
		CoordMap.Mapping map = CoordMap.get();
		if (map == null || map.provisional()) {
			return;
		}
		Vec3 hit = Vec3.atCenterOf(pos);
		net.minecraft.world.phys.HitResult hr = player.pick(6.0, 1.0F, false);
		if (hr instanceof net.minecraft.world.phys.BlockHitResult bh && bh.getBlockPos().equals(pos)) {
			hit = bh.getLocation();
		}
		double[] h = map.toHost(hit.x, hit.y, hit.z);
		ErLink.get().pushDamage(0L, 1.0F, (float) h[0], (float) h[1], (float) h[2], 0);
		if (CoopSession.enabled()) {
			// Guest clients have no local terrain server; invalidate only near this authenticated peer.
			CoopSession.clientTerrainChanged();
			return;
		}
		RESAMPLE.add(pos.getX(), pos.getZ(), 2, System.currentTimeMillis() + 900);
	}

	/**
	 * Elden Ring performed an action for the player (a door swung open, a lever moved a gate):
	 * sample the terrain around them again while things move and once they have stopped, or the
	 * old collision stays behind as an invisible wall. Any thread.
	 */
	public static void resampleAround(double x, double z, int radius) {
		long now = System.currentTimeMillis();
		for (long delay : new long[] {1500, 3500, 7000}) {
			RESAMPLE.add(Mth.floor(x), Mth.floor(z), radius, now + delay);
		}
	}

	private static void resampleStruck() {
		RESAMPLE.drain(System.currentTimeMillis(), tickBudget, key -> {
			COLUMNS.invalidate(key);
		});
	}

	/** Put the Minecraft players next to the Tarnished as soon as Elden Ring reports it usable. */
	public static void requestRecall() {
		RECALL_GEN.incrementAndGet();
	}

	/** True when no recall is pending and the last one was served at least {@code settleMs} ago (so the teleport reached the client). */
	public static boolean recallSettled(long settleMs) {
		return recallDone == RECALL_GEN.get() && System.currentTimeMillis() - recallDoneAtMs >= settleMs;
	}

	/** Dev: collision filter for terrain rays ({a, b, c} for Param::setAttr), or null for none. */
	public static void setRayFilter(int[] filter) {
		rayFilter = filter;
	}

	public static void setWallIgnoreMask(int mask) {
		wallIgnoreMask = mask;
	}

	/** Dev: remove all sampled terrain and sample again. */
	public static void requestReset() {
		resetRequested = true;
	}

	public static String describeSettings() {
		int[] f = rayFilter;
		return "filter " + (f == null ? "none" : String.format("(%d, %#x, %d)", f[0], f[1], f[2]))
			+ String.format(" wallIgnore %#x columns %d/%d evicted %d unloadedAnswers %d refreshRegions %d droppedRefresh %d applying %d/%d", wallIgnoreMask,
				COLUMNS.size(), COLUMN_CACHE_LIMIT, COLUMNS.evictions(), skippedUnloadedColumns, RESAMPLE.size(), RESAMPLE.dropped(), applyingColumn, PENDING_COLUMNS.size());
	}

	public static void onServerStarted(MinecraftServer server) {
		reset();
		bridgeWorld = BRIDGE_LEVEL_NAME.equals(server.getWorldData().getLevelName());
		if (!bridgeWorld) {
			return;
		}
		ServerLevel overworld = server.overworld();
		// Midnight, for good: undead mobs don't burn, so they can fight Elden Ring's enemies. Minecraft
		// still renders full daylight (ClientLevelMixin); Elden Ring's light is applied on top.
		overworld.setDayTime(18000);
		server.getGameRules().getRule(net.minecraft.world.level.GameRules.RULE_DAYLIGHT).set(false, server);
		server.getGameRules().getRule(net.minecraft.world.level.GameRules.RULE_DOINSOMNIA).set(false, server);
		// One life for both games: dying costs the runes in Elden Ring, not the Minecraft inventory,
		// which would be left behind in an area the story may never return to.
		server.getGameRules().getRule(net.minecraft.world.level.GameRules.RULE_KEEPINVENTORY).set(true, server);
		overworld.setDefaultSpawnPos(BlockPos.containing(CoordMap.MC_X, CoordMap.MC_Y, CoordMap.MC_Z), 0.0F);
		loadAnchors(server);
		lastHostLife = Integer.MIN_VALUE;
		// The hunter is where the player really is when the world opens.
		requestRecall();
	}

	/** Minecraft respawned a player (at the world spawn, over the void): keep them standing until the recall moves them. */
	public static void onRespawn(ServerPlayer player) {
		if (CoopSession.enabled() && player.getY() < player.serverLevel().getMinBuildHeight() + 2) {
			CoordMap.Mapping map = CoordMap.get();
			player.teleportTo(player.serverLevel(), map == null ? CoordMap.MC_X : map.originX(), CoordMap.MC_Y,
				CoordMap.MC_Z, player.getYRot(), player.getXRot());
		}
		if (CoopSession.enabled()) {
			COOP_HOLD.put(player.getUUID(), player.position());
			CoopSession.requestRecall(player);
		} else requestRecall();
	}

	public static void onJoin(MinecraftServer server, ServerPlayer player) {
		if (!bridgeWorld) {
			return;
		}
		if (CoopSession.enabled()) {
			java.util.UUID id = player.getUUID();
			COOP_HOLD.remove(id); COOP_CORRECTIONS.remove(id); GROUND_GUARDS.remove(id);
			CRITICAL_GROUND.remove(id); MOTION.remove(id); FLIGHT_MODE.remove(id);
			ADMISSION.remove(id); SCAN_CURSORS.remove(id);
		}
		// Brand-new worlds spawn players in the void; put them next to the hunter instead.
		CoordMap.Mapping map = CoordMap.get();
		if (player.getY() < 0) {
			if (map != null && (!CoopSession.enabled() || CoopSession.localPlayer(server, player))) {
				teleportToHunter(server.overworld(), player, map);
			} else {
				// The host can still be loading, or this can be a diagnostic world open.
				// Void-world spawn search reaches minY; hold the player above our fallback
				// floor until the first real host position supplies the recall anchor.
				ServerLevel level = server.overworld();
				Vec3 target = new Vec3(CoordMap.MC_X, CoordMap.MC_Y, CoordMap.MC_Z);
				player.teleportTo(level, target.x, target.y + 0.01, target.z, player.getYRot(), player.getXRot());
					LOG.info("Host anchor pending: player waits for measured support");
			}
		}
	}

	/** Guard before player ticking too: an unanswered floor must not accrue fatal fall distance. */
	public static void onServerTickStart(MinecraftServer server) {
		if (!bridgeWorld) return;
		if (CoopSession.enabled()) holdUnsupportedPeers(server);
		CoordMap.Mapping map = CoordMap.get();
		if (map == null || map.provisional()) {
			holdWithoutAnchor(server.getPlayerList().getPlayers());
			return;
		}
		BOOTSTRAP_HOLD.clear();
		ErLink link = ErLink.get();
		boolean ready = link.poll() && link.snapshot(STATE) && STATE.has(Protocol.STATE_PLAYER_VALID)
			&& !STATE.has(Protocol.STATE_HOST_BUSY) && !STATE.has(Protocol.STATE_PLAYER_DEAD)
			&& map.zone() == STATE.stageId && link.hostLife() == lastHostLife && recallDone == RECALL_GEN.get();
		protectUnansweredGround(server.overworld(), map, server.getPlayerList().getPlayers(), ready, server.getTickCount());
	}

	public static void onServerTick(MinecraftServer server) {
		if (!bridgeWorld) {
			return;
		}
		long started = System.nanoTime();
		tickBudget = new TerrainWorkBudget();
		try { tickTerrain(server); }
		finally {
			long elapsed = System.nanoTime() - started;
			tickNanos += elapsed; tickWorstNanos = Math.max(tickWorstNanos, elapsed); terrainTicks++;
		}
	}

	private static void tickTerrain(MinecraftServer server) {
		if (CoopSession.enabled()) holdUnsupportedPeers(server);
		ErLink link = ErLink.get();
		boolean alive = link.poll();
		boolean haveState = alive && link.snapshot(STATE);
		boolean hostReady = haveState && STATE.has(Protocol.STATE_PLAYER_VALID)
			&& !STATE.has(Protocol.STATE_HOST_BUSY) && !STATE.has(Protocol.STATE_PLAYER_DEAD);
		if (haveState) {
			int life = link.hostLife();
			if (life != lastHostLife) {
				if (lastHostLife != Integer.MIN_VALUE) {
					LOG.info("Elden Ring placed the Tarnished anew (life {}): players go to it", life);
				}
				lastHostLife = life;
				discardPending();
				clearPassages();
				raysAnswered = false;
				GROUND_GUARDS.clear();
				MOTION.clear();
				FLIGHT_MODE.clear();
				CRITICAL_GROUND.clear();
				requestRecall();
			}
			CoordMap.Mapping previous = CoordMap.get();
			if (hostReady) {
				updateAnchor(server);  // its zone-change path can teleport players too
			}
			if (!java.util.Objects.equals(previous, CoordMap.get())) {
				discardPending();
				clearPassages();
				raysAnswered = false;
				GROUND_GUARDS.clear();
				MOTION.clear();
				FLIGHT_MODE.clear();
				CRITICAL_GROUND.clear();
			}
			int gen = RECALL_GEN.get();
			if (gen != recallDone && hostReady && CoordMap.get() != null
				&& !server.getPlayerList().getPlayers().isEmpty()) {
				for (ServerPlayer p : server.getPlayerList().getPlayers()) {
					if (!CoopSession.enabled() || CoopSession.localPlayer(server, p)) teleportToHunter(server.overworld(), p, CoordMap.get());
				}
				recallDoneAtMs = System.currentTimeMillis();
				recallDone = gen;
			}
			CoordMap.Mapping current = CoordMap.get();
			if (hostReady && STATE.has(Protocol.STATE_MOVEMENT_CORRECTED) && current != null && current.zone() == STATE.stageId) {
				Vec3 corrected = current.toMc(STATE.playerPos[0], STATE.playerPos[1], STATE.playerPos[2]);
				boolean prime = correctionNeedsPrime(server.overworld(),lastMovementCorrection,corrected,0.6);
				if (prime) discardPending();
				for (ServerPlayer p : server.getPlayerList().getPlayers()) {
					if (!CoopSession.enabled() || CoopSession.localPlayer(server, p)) teleportToHunter(server.overworld(), p, current, prime, true);
				}
				lastMovementCorrection = corrected;
			} else {
				lastMovementCorrection = null;
			}
		}
		CoordMap.Mapping map = CoordMap.get();
		ServerLevel level = server.overworld();
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (map == null || map.provisional()) {
			discardPending();
			clearPassages();
			holdWithoutAnchor(players);
			return;
		}
		if (!haveState || !hostReady || map.zone() != STATE.stageId || recallDone != RECALL_GEN.get()) {
			discardPending();
			clearPassages();
			protectUnansweredGround(level, map, players, false, server.getTickCount());
			return;  // death/loading/recall is not a valid coordinate frame for new terrain
		}
		if (CoopSession.enabled()) recallPeers(server, map);
		updatePassages(map);
		if (resetRequested) {
			resetRequested = false;
			// No global block sweep: each loaded replacement cleans its own column incrementally.
			COLUMNS.invalidateAll();
			discardPending();
			LOG.info("Terrain reset ({})", describeSettings());
		}
		resampleStruck();
		collectResults(level, map);
		protectUnansweredGround(level, map, players, true, server.getTickCount());
		if (pendingSeq < 0 && !players.isEmpty()) {
			List<ServerPlayer> sampled = CoopSession.enabled() ? players.stream().filter(CoopSession::eligible).toList() : players;
			if (!sampled.isEmpty()) submitRays(map, sampled);
		}
		logAttrStats();
	}

	/** Ingress is latched only after finite measured support or an active-flight survey; ordinary gaps do not churn ACKs. */
	public static boolean criticalGroundReady(ServerPlayer player) {
		return ADMISSION.ready(player.getUUID());
	}

	/** Cached ingress/recall evidence, latched after certification; not a grounded-contact flag. */
	public static TerrainReadiness supportReadiness(ServerPlayer player) {
		return ADMISSION.result(player.getUUID());
	}
	public static boolean actualSupportReady(ServerPlayer player) {
		return ADMISSION.current(player.getUUID()) == TerrainReadiness.SUPPORTED;
	}

	/** Waiting for an anchor does not create a world-spanning temporary collision plane. */
	private static void holdWithoutAnchor(List<ServerPlayer> players) {
		java.util.Set<java.util.UUID> present=new java.util.HashSet<>();
		for (ServerPlayer player:players) {
			present.add(player.getUUID());
			if (player.isDeadOrDying() || player.isSpectator()) continue;
			Vec3 held=BOOTSTRAP_HOLD.computeIfAbsent(player.getUUID(),ignored -> player.position());
			if (player.position().distanceToSqr(held)>1e-8)
				player.teleportTo(player.serverLevel(),held.x,held.y,held.z,player.getYRot(),player.getXRot());
			player.setDeltaMovement(Vec3.ZERO); player.fallDistance=0;
		}
		BOOTSTRAP_HOLD.keySet().retainAll(present);
	}

	private static void holdUnsupportedPeers(MinecraftServer server) {
		java.util.Set<java.util.UUID> present = new java.util.HashSet<>();
		for (ServerPlayer player : server.getPlayerList().getPlayers()) {
			java.util.UUID id = player.getUUID(); present.add(id);
			if (CoopSession.eligible(player) && !CoopSession.recallPending(player)) {
				COOP_HOLD.put(id, player.position()); continue;
			}
			// Even flying guests cannot explore collision which the host cannot measure.
			Vec3 held = COOP_HOLD.computeIfAbsent(id, ignored -> player.position());
			if (!player.isDeadOrDying()) {
				if (player.position().distanceToSqr(held) > 1e-8)
					player.teleportTo(player.serverLevel(), held.x, held.y, held.z, player.getYRot(), player.getXRot());
				player.setDeltaMovement(Vec3.ZERO); player.fallDistance = 0;
			}
		}
		COOP_HOLD.keySet().retainAll(present);
		COOP_CORRECTIONS.keySet().retainAll(present);
	}

	private static void recallPeers(MinecraftServer server, CoordMap.Mapping map) {
		for (ServerPlayer player : server.getPlayerList().getPlayers()) {
			if (CoopSession.spatialStatus(player) != CoopProtocol.READY || player.isDeadOrDying()) continue;
			PeerState peer = CoopSession.state(player);
			if (peer == null) continue;
			Vec3 target = map.toMc(peer.hostX(), peer.hostY(), peer.hostZ());
			if (target.y < server.overworld().getMinBuildHeight() + 2 || target.y > server.overworld().getMaxBuildHeight() - 3) continue;
			boolean recall = CoopSession.recallPending(player);
			boolean correction = !CoopSession.localPlayer(server, player) && (peer.flags() & Protocol.STATE_MOVEMENT_CORRECTED) != 0;
			if (!correction) COOP_CORRECTIONS.remove(player.getUUID());
			if (!recall && !correction) continue;
			Vec3 previous = COOP_CORRECTIONS.get(player.getUUID());
			boolean prime = recall || correctionNeedsPrime(server.overworld(),previous,target,player.getBbWidth());
			teleportToFeet(server.overworld(), player, target, player.getYRot(), player.getXRot(), prime);
			COOP_HOLD.put(player.getUUID(), player.position());
			if (correction) COOP_CORRECTIONS.put(player.getUUID(), target);
			if (recall) CoopSession.recalled(player);
		}
	}

	// -- ray-sampled terrain ----------------------------------------------------------------------

	/** Resume the distant horizon; immediate feet and the 3x3 are submitted separately first. */
	private static int sampleAround(CoordMap.Mapping map, double ex, double ey, double ez, int radius, int n,
			boolean flight, TerrainScan.Cursor cursor) {
		int px=Mth.floor(ex), pz=Mth.floor(ez);
		int[][] offsets=radius==SAMPLE_RADIUS ? PLAYER_SAMPLE_OFFSETS : MOB_SAMPLE_OFFSETS;
		cursor.centre(px,pz);
		for (int visited=0;visited<64 && visited<offsets.length && n+RAYS_PER_COLUMN<=BATCH;visited++) {
			int[] offset=offsets[cursor.next(offsets.length)];
			n=sampleColumn(map,px+offset[0],ey,pz+offset[1],n,flight);
			if (tickBudget.expired()) break;
		}
		return n;
	}

	private static int sampleColumn(CoordMap.Mapping map, int x, double py, int z, int n) {
		return sampleColumn(map, x, py, z, n, false);
	}

	private static int sampleColumn(CoordMap.Mapping map, int x, double py, int z, int n, boolean flight) {
		long key = ChunkPos.asLong(x, z);
		Column c = COLUMNS.peek(key);
		if (n + RAYS_PER_COLUMN > BATCH || !tickBudget.scan() || samplingLevel.getChunkSource().getChunkNow(x >> 4,z >> 4)==null
			|| (c != null && COLUMNS.current(key) && matchingColumn(c,py,flight)!=null)
			|| !BATCH_KEYS.add(key)) {
			return n;
		}
		double below = flight ? TerrainSampling.FLIGHT_RAY_BELOW : RAY_BELOW;
		n = TerrainSampling.writeColumn(RAYS, n, x, py, z, flight ? TerrainSampling.FLIGHT_RAY_ABOVE : RAY_ABOVE, below, map::toHost);
		PENDING_COLUMNS.add(new PendingColumn(key, py, below, flight, COLUMNS.capture(key)));
		return n;
	}

	private static int sampleFeet(CoordMap.Mapping map, Vec3 feet, double width, int n) {
		return sampleFeet(map, feet, width, n, false);
	}

	private static int sampleFeet(CoordMap.Mapping map, Vec3 feet, double width, int n, boolean flight) {
		int[] b = TerrainSampling.footprint(feet.x, feet.z, width);
		for (int x = b[0]; x <= b[1]; x++) {
			for (int z = b[2]; z <= b[3]; z++) {
				n = sampleColumn(map, x, feet.y, z, n, flight);
			}
		}
		return n;
	}

	private static TerrainSampling.Ground groundAt(Vec3 feet, double width) {
		return groundAt(feet, width, false);
	}

	private static TerrainSampling.Ground groundAt(Vec3 feet, double width, boolean flight) {
		int[] b=TerrainSampling.footprint(feet.x,feet.z,width);
		boolean surface=false, answered=true;
		for (int x=b[0];x<=b[1];x++) for (int z=b[2];z<=b[3];z++) {
			long key=ChunkPos.asLong(x,z);
			Column c=COLUMNS.get(key); // retain feet, not the distant horizon
			if (c==null || !COLUMNS.current(key) || (c=matchingColumn(c,feet.y,flight))==null) { answered=false; continue; }
			// Dirty physical collision is retained, but cannot certify a fresh recall or movement query.
			if (Double.isFinite(c.surfaceY())) {
				if (installedSurface(samplingLevel,c,x,z)) surface=true; else answered=false;
			}
			else if (!COLUMNS.current(key)) answered=false;
		}
		return TerrainSampling.footprintGround(surface,answered);
	}

	private static Column matchingColumn(Column column,double feetY,boolean flight) {
		if (TerrainSampling.sampleCurrent(column.sampleY(),column.surfaceY(),column.supportY(),feetY,column.flight(),flight)) return column;
		for (Column retained:column.retained()) {
			// Retained entries describe collision only, never a different altitude's air answer.
			if (Double.isFinite(retained.surfaceY()) && TerrainSampling.sampleCurrent(retained.sampleY(),retained.surfaceY(),retained.supportY(),feetY,retained.flight(),flight))
				return retained;
		}
		return null;
	}

	private static boolean installedSurface(ServerLevel level, Column column, int x, int z) {
		if (level==null) return false;
		int y=Mth.floor(column.surfaceY()-1e-4);
		if (y<level.getMinBuildHeight() || y>=level.getMaxBuildHeight()) return false;
		LevelChunk chunk=level.getChunkSource().getChunkNow(x>>4,z>>4);
		if (chunk==null) return false;
		BlockState block=chunk.getBlockState(new BlockPos(x,y,z));
		return block.is(ErBridgeMod.TERRAIN) && Math.abs(y+block.getValue(TerrainBlock.HEIGHT)/16.0-column.surfaceY())<0.01;
	}

	private static TerrainReadiness readinessAt(ServerLevel level, Vec3 feet, double width, boolean flight) {
		int[] b=TerrainSampling.footprint(feet.x,feet.z,width);
		boolean answered=true;
		for (int x=b[0];x<=b[1];x++) for (int z=b[2];z<=b[3];z++) {
			long key=ChunkPos.asLong(x,z);
			Column c=COLUMNS.get(key);
			LevelChunk chunk=level.getChunkSource().getChunkNow(x>>4,z>>4);
			if (chunk==null || c==null || !COLUMNS.current(key)
				|| (c=matchingColumn(c,feet.y,flight))==null) { answered=false; continue; }
			if (!flight && TerrainSampling.supportsFeet(c.supportY(),feet.y)) {
				int y=Mth.floor(c.supportY()-1e-4);
				if (y>=level.getMinBuildHeight() && y<level.getMaxBuildHeight()) {
					BlockState block=chunk.getBlockState(new BlockPos(x,y,z));
					if (block.is(ErBridgeMod.TERRAIN) && Math.abs(y+block.getValue(TerrainBlock.HEIGHT)/16.0-c.supportY())<0.01)
						return TerrainReadiness.SUPPORTED;
				}
			}
		}
		return !answered ? TerrainReadiness.PENDING : flight ? TerrainReadiness.AIRBORNE : TerrainReadiness.UNSUPPORTED;
	}

	/** Player-built collision is independently authoritative; stale bridge blocks are not. */
	private static boolean ownSupport(ServerLevel level, ServerPlayer player) {
		AABB feet = player.getBoundingBox();
		AABB support = new AABB(feet.minX + 1e-7, player.getY() - 0.08, feet.minZ + 1e-7,
			feet.maxX - 1e-7, player.getY() + 0.001, feet.maxZ - 1e-7);
		int[] b = TerrainSampling.footprint(player.getX(), player.getZ(), player.getBbWidth());
		for (int x = b[0]; x <= b[1]; x++) {
			for (int z = b[2]; z <= b[3]; z++) {
				LevelChunk chunk=level.getChunkSource().getChunkNow(x>>4,z>>4);
				if (chunk==null) continue;
				for (int y = Mth.floor(support.minY); y <= Mth.floor(support.maxY); y++) {
					BlockPos pos = new BlockPos(x, y, z);
					BlockState state = chunk.getBlockState(pos);
					if (state.isAir() || state.is(ErBridgeMod.TERRAIN)) continue;
					for (AABB box : state.getCollisionShape(level, pos, net.minecraft.world.phys.shapes.CollisionContext.of(player)).toAabbs()) {
						if (box.move(x, y, z).intersects(support)) return true;
					}
				}
			}
		}
		return false;
	}

	private static boolean flying(ServerPlayer player) {
		return !player.onGround() && !player.isSpectator() && (player.isFallFlying() || player.getAbilities().flying);
	}

	private static boolean canLaunch(ServerPlayer player) {
		net.minecraft.world.item.ItemStack chest = player.getItemBySlot(net.minecraft.world.entity.EquipmentSlot.CHEST);
		return chest.is(net.minecraft.world.item.Items.ELYTRA) && net.minecraft.world.item.ElytraItem.isFlyEnabled(chest)
			&& !player.isInWater() && !player.hasEffect(net.minecraft.world.effect.MobEffects.LEVITATION);
	}

	/** Certify the recalled body's loaded movement medium, never a cached entity flag alone. */
	private static TerrainReadiness movementReadiness(ServerLevel level, ServerPlayer player) {
		AABB body=player.getBoundingBox().deflate(1e-7);
		int minX=Mth.floor(body.minX), maxX=Mth.floor(body.maxX), minZ=Mth.floor(body.minZ), maxZ=Mth.floor(body.maxZ);
		// Fluid height and ladder checks below must not cause a neighbouring chunk to load.
		for (int x=minX>>4;x<=maxX>>4;x++) for (int z=minZ>>4;z<=maxZ>>4;z++)
			if (level.getChunkSource().getChunkNow(x,z)==null) return TerrainReadiness.PENDING;
		if (player.isInWater()) {
			for (int x=minX;x<=maxX;x++) for (int z=minZ;z<=maxZ;z++) {
				LevelChunk chunk=level.getChunkSource().getChunkNow(x>>4,z>>4);
				for (int y=Mth.floor(body.minY);y<=Mth.floor(body.maxY);y++) {
					if (y<level.getMinBuildHeight() || y>=level.getMaxBuildHeight()) continue;
					BlockPos pos=new BlockPos(x,y,z);
					var fluid=chunk.getFluidState(pos);
					if (fluid.is(net.minecraft.tags.FluidTags.WATER) && y+fluid.getHeight(level,pos)>body.minY)
						return TerrainReadiness.SWIMMING;
				}
			}
		}
		if (player.onClimbable()) {
			BlockPos pos=BlockPos.containing(player.position());
			if (pos.getY()<level.getMinBuildHeight() || pos.getY()>=level.getMaxBuildHeight()) return TerrainReadiness.PENDING;
			LevelChunk chunk=level.getChunkSource().getChunkNow(pos.getX()>>4,pos.getZ()>>4);
			BlockState state=chunk.getBlockState(pos);
			if (state.is(net.minecraft.tags.BlockTags.CLIMBABLE)) return TerrainReadiness.CLIMBING;
			// Vanilla also permits an open trapdoor continuing a ladder with the same facing.
			if (state.getBlock() instanceof net.minecraft.world.level.block.TrapDoorBlock && state.getValue(net.minecraft.world.level.block.TrapDoorBlock.OPEN)
				&& pos.getY()>level.getMinBuildHeight()) {
				BlockState below=chunk.getBlockState(pos.below());
				if (below.is(net.minecraft.world.level.block.Blocks.LADDER)
					&& below.getValue(net.minecraft.world.level.block.LadderBlock.FACING)==state.getValue(net.minecraft.world.level.block.TrapDoorBlock.FACING))
					return TerrainReadiness.CLIMBING;
			}
		}
		return TerrainReadiness.PENDING;
	}

	private static void protectUnansweredGround(ServerLevel level, CoordMap.Mapping map, List<ServerPlayer> players, boolean hostReady, long tick) {
		java.util.Set<java.util.UUID> present = new java.util.HashSet<>();
		for (ServerPlayer player : players) {
			java.util.UUID id = player.getUUID();
			present.add(id);
			if (CoopSession.enabled() && (!CoopSession.eligible(player) || CoopSession.recallPending(player))) continue;
			if (player.isDeadOrDying() || player.isSpectator() || player.isPassenger()
				|| player.isInWater() || player.onClimbable()) {
				GROUND_GUARDS.remove(id);
				MOTION.remove(id);
				FLIGHT_MODE.remove(id);
				CRITICAL_GROUND.remove(id);
				Vec3 target=ADMISSION.waiting(id);
				boolean valid=hostReady && !player.isDeadOrDying() && !player.isSpectator() && !player.isPassenger()
					&& (target==null || target.distanceToSqr(player.position())<0.25);
				ADMISSION.observe(id,valid ? movementReadiness(level,player) : TerrainReadiness.PENDING);
				continue;
			}
			Vec3 at = player.position();
			TerrainSampling.Point point = new TerrainSampling.Point(at.x, at.y, at.z);
			TerrainSampling.Motion motion = MOTION.computeIfAbsent(id, ignored -> new TerrainSampling.Motion());
			motion.observe(point, tick);
			TerrainSampling.GroundGuard guard = GROUND_GUARDS.get(id);
			if (guard == null) {
				PeerState peer = CoopSession.enabled() ? CoopSession.state(player) : null;
				Vec3 seed = peer != null ? map.toMc(peer.hostX(), peer.hostY(), peer.hostZ()).add(0, .01, 0)
					: hostReady ? map.toMc(STATE.playerPos[0], STATE.playerPos[1], STATE.playerPos[2]).add(0, 0.01, 0) : at;
				guard = new TerrainSampling.GroundGuard(new TerrainSampling.Point(seed.x, seed.y, seed.z));
				GROUND_GUARDS.put(id, guard);
			}
			Vec3 waiting = CRITICAL_GROUND.get(id);
			if (waiting != null && groundAt(waiting, player.getBbWidth()) != TerrainSampling.Ground.UNANSWERED) {
				CRITICAL_GROUND.remove(id);
			}
			boolean flight = hostReady && flying(player);
			FLIGHT_MODE.put(id, flight); // Keep the bounded mailbox; mode changes resample on the next submission.
			TerrainSampling.Ground ground = hostReady ? groundAt(at, player.getBbWidth(), flight) : TerrainSampling.Ground.UNANSWERED;
			boolean support = ownSupport(level, player);
			Vec3 admission=ADMISSION.waiting(id);
			Vec3 survey=admission==null || flight ? at : admission;
			TerrainReadiness readiness=hostReady ? readinessAt(level,survey,player.getBbWidth(),flight) : TerrainReadiness.PENDING;
			if (support && !flight && at.distanceToSqr(survey)<0.25) readiness=TerrainReadiness.SUPPORTED;
			ADMISSION.observe(id,readiness);
			// The main guard owns passive/unready physics. Do not roll a ready native-mode body back
			// to its last Minecraft checkpoint; keep the checkpoint local while it is suspended.
			if (CoopSession.enabled() && criticalGroundReady(player) && CoopBodyGuard.isSuspended(player)) {
				GROUND_GUARDS.put(id,new TerrainSampling.GroundGuard(point));
				motion.reset(point,tick);
				continue;
			}
			TerrainSampling.Point held = guard.update(point, ground, player.onGround(), support, flight, hostReady && canLaunch(player), tick);
			if (flight) CRITICAL_GROUND.remove(id); // an old walking destination must not consume the flight budget
			if (ground == TerrainSampling.Ground.UNANSWERED && !support && held != null) {
				// Keep the attempted destination queued after rolling the player back to known footing.
				CRITICAL_GROUND.putIfAbsent(id, at);
			}
			if (held != null) {
				if (at.distanceToSqr(new Vec3(held.x(), held.y(), held.z())) > 1e-8) {
					player.teleportTo(level, held.x(), held.y(), held.z(), player.getYRot(), player.getXRot());
				}
				player.setDeltaMovement(Vec3.ZERO);
				player.fallDistance = 0.0F;
				motion.reset(held, tick);
			}
		}
		GROUND_GUARDS.keySet().retainAll(present);
		MOTION.keySet().retainAll(present);
		FLIGHT_MODE.keySet().retainAll(present);
		CRITICAL_GROUND.keySet().retainAll(present);
		ADMISSION.retain(present);
		SCAN_CURSORS.keySet().retainAll(present);
	}

	/** Ground is also kept under Minecraft mobs this close to a player (e.g. a zombie walking to an Elden Ring enemy). */
	private static final double MOB_SAMPLE_RANGE = 64.0;
	private static final int MOB_SAMPLE_RADIUS = 6;
	private static final int[][] MOB_SAMPLE_OFFSETS = TerrainScan.offsets(MOB_SAMPLE_RADIUS);
	private static final int MAX_SAMPLED_MOBS = 24;
	private static final LongOpenHashSet BATCH_KEYS = new LongOpenHashSet();

	private static void submitRays(CoordMap.Mapping map, List<ServerPlayer> players) {
		samplingLevel=players.get(0).serverLevel();
		List<ServerPlayer> ordered=new ArrayList<>();
		int start=Math.floorMod(samplePlayer++,players.size());
		for (int i=0;i<players.size() && i<8;i++) ordered.add(players.get((start+i)%players.size()));
		players=ordered;
		TerrainSampling.BatchContext<CoordMap.Mapping> context = new TerrainSampling.BatchContext<>(map, lastHostLife, RECALL_GEN.get());
		clearPendingColumns();
		BATCH_KEYS.clear();
		int n = 0;
		// A critical destination held back last tick must finish before distant terrain or mobs.
		for (ServerPlayer player : players) {
			Vec3 waiting = ADMISSION.waiting(player.getUUID())!=null ? ADMISSION.waiting(player.getUUID()) : CRITICAL_GROUND.get(player.getUUID());
			if (waiting != null && !flying(player)) n = sampleFeet(map, waiting, player.getBbWidth(), n);
			n = sampleFeet(map, player.position(), player.getBbWidth(), n, flying(player));
		}
		// A 12-15 FPS host may answer half a second later. Probe observed travel before the
		// stationary radius, including lower predicted feet during an elytra approach.
		for (ServerPlayer player : players) {
			TerrainSampling.Motion motion = MOTION.get(player.getUUID());
			if (!flying(player) || motion == null) continue;
			for (TerrainSampling.Point ahead : motion.ahead(new TerrainSampling.Point(player.getX(), player.getY(), player.getZ()))) {
				n = sampleFeet(map, new Vec3(ahead.x(), ahead.y(), ahead.z()), player.getBbWidth(), n, true);
			}
		}
		for (ServerPlayer player : players) {
			Vec3 waiting = ADMISSION.waiting(player.getUUID())!=null ? ADMISSION.waiting(player.getUUID()) : CRITICAL_GROUND.get(player.getUUID());
			Vec3 priority = waiting != null && !flying(player) ? waiting : player.position();
			// Complete the immediate 3x3 before filling the far 24-block horizon.
			for (int dx = -1; dx <= 1; dx++) {
				for (int dz = -1; dz <= 1; dz++) {
					n = sampleColumn(map, Mth.floor(priority.x) + dx, priority.y, Mth.floor(priority.z) + dz, n, flying(player));
				}
			}
		}
		for (ServerPlayer player : players) {
			n = sampleAround(map, player.getX(), player.getY(), player.getZ(), SAMPLE_RADIUS, n, flying(player),
				SCAN_CURSORS.computeIfAbsent(player.getUUID(),ignored -> new TerrainScan.Cursor()));
		}
		if (n+RAYS_PER_COLUMN<=BATCH && !tickBudget.expired() && samplingLevel.getServer().getTickCount()%10==0) {
			ServerPlayer first=players.get(0);
			List<net.minecraft.world.entity.Mob> mobs=new ArrayList<>(MAX_SAMPLED_MOBS);
			// Native entity query stops at the cap; never allocate/sort every mob in a 128m box.
			samplingLevel.getEntities(net.minecraft.world.level.entity.EntityTypeTest.forClass(net.minecraft.world.entity.Mob.class),
				first.getBoundingBox().inflate(MOB_SAMPLE_RANGE), m -> true, mobs, MAX_SAMPLED_MOBS);
			for (net.minecraft.world.entity.Mob mob:mobs) {
				if (n+RAYS_PER_COLUMN>BATCH || tickBudget.expired()) break;
				if (!mob.isAlive()) continue;
				n=sampleFeet(map,mob.position(),mob.getBbWidth(),n);
			}
		}
		if (n == 0 || !context.matches(CoordMap.get(), lastHostLife, RECALL_GEN.get()) || recallDone != context.recallGeneration()) {
			clearPendingColumns();
			return;
		}
		int[] filter = rayFilter;
		int seq = ErLink.get().submitRays(RAYS, n, filter != null ? dev.ermc.bridge.link.Protocol.RAYS_CUSTOM_FILTER : 0, filter);
		if (seq >= 0) {
			pendingSeq = seq;
			pendingSince = System.currentTimeMillis();
			pendingContext = context;
			pendingWarned = false;
		} else {
			clearPendingColumns();
		}
	}

	private static void discardPending() {
		if (application!=null) COLUMNS.invalidate(application.sample.key());
		pendingSeq = -1;
		pendingContext = null;
		pendingWarned = false;
		clearPendingColumns();
		hitsRead=false; applyingColumn=0; application=null;
	}

	private static void clearPendingColumns() {
		for (PendingColumn sample:PENDING_COLUMNS) COLUMNS.release(sample.key());
		PENDING_COLUMNS.clear();
	}

	/** A horizontal probe hit counts as an obstacle if the surface is steep and stands above the ground. */
	private static boolean isObstacle(int ray, double groundY, double probeY, CoordMap.Mapping map) {
		if (HIT_FLAGS[ray] == 0) {
			return false;
		}
		countAttribute(WALL_ATTRS,HIT_ATTRS[ray]);
		if ((HIT_ATTRS[ray] & wallIgnoreMask) != 0) {
			return false;
		}
		double ny = HITS[ray * 6 + 4];
		return Math.abs(ny) < 0.7 && groundY < probeY - 0.25;
	}

	private static void logAttrStats() {
		long now = System.currentTimeMillis();
		if (now - lastAttrLog < 30000) {
			return;
		}
		lastAttrLog = now;
		if (terrainTicks>0) {
			LOG.info("Terrain server tick: count {}, average {} ms, worst {} ms; limit {} block steps, {} scans, {} apply columns, {} ms cooperative budget",
				terrainTicks,tickNanos/(terrainTicks*1_000_000.0),tickWorstNanos/1_000_000.0,
				TerrainWorkBudget.BLOCK_STEPS,TerrainWorkBudget.SCAN_STEPS,TerrainWorkBudget.APPLY_COLUMNS,TerrainWorkBudget.TICK_NANOS/1_000_000.0);
			terrainTicks=0; tickNanos=tickWorstNanos=0;
		}
		LOG.info("Surface attributes - ground: {} walls: {}", fmtAttrs(GROUND_ATTRS), fmtAttrs(WALL_ATTRS));
		LOG.info("Terrain working set: {}", describeSettings());
		if (applyBatches > 0) {
			LOG.info("Terrain apply slices: count {}, average {} ms, worst {} ms", applyBatches,
				applyNanos / (applyBatches * 1_000_000.0), applyWorstNanos / 1_000_000.0);
			applyBatches = 0;
			applyNanos = applyWorstNanos = 0;
		}
	}

	private static void countAttribute(java.util.Map<Integer,Integer> values, int key) {
		if (values.containsKey(key) || values.size()<128) values.merge(key,1,Integer::sum);
	}

	private static String fmtAttrs(java.util.Map<Integer, Integer> m) {
		StringBuilder b = new StringBuilder();
		m.entrySet().stream().sorted((x, y) -> Integer.compare(y.getValue(),x.getValue())).limit(8)
			.forEach(e -> b.append(String.format("%#x=%d ", e.getKey(), e.getValue())));
		return b.toString();
	}

	private static final class ColumnApplication {
		final PendingColumn sample;
		final Column result;
		final int groundBlock, height;
		final boolean obstacle;
		final TerrainColumnWork work;
		ColumnApplication(PendingColumn sample, Column result, int groundBlock, int height, boolean obstacle, TerrainColumnWork work) {
			this.sample=sample; this.result=result; this.groundBlock=groundBlock; this.height=height; this.obstacle=obstacle; this.work=work;
		}
	}

	private static ColumnApplication prepareColumn(ServerLevel level, CoordMap.Mapping sampleMap, int i) {
		PendingColumn sample = PENDING_COLUMNS.get(i);
		long key = sample.key();
		double sampleY = sample.sampleY();
		int x=ChunkPos.getX(key), z=ChunkPos.getZ(key);
		Column old = COLUMNS.peek(key);
		// Also cover untracked collision from earlier sessions or an interrupted application.
		int clearTop = Mth.floor(sampleY + RAY_HIGH);
		int clearBottom = Mth.floor(sampleY - sample.below()) - THICKNESS;
		if (old != null && old.top() != Integer.MIN_VALUE) {
			clearTop=Math.max(clearTop,old.top()); clearBottom=Math.min(clearBottom,old.bottom());
		}
		if (old!=null && old.cleanupTop()!=Integer.MIN_VALUE) {
			clearTop=Math.max(clearTop,old.cleanupTop()); clearBottom=Math.min(clearBottom,old.cleanupBottom());
		}
		int top=Integer.MIN_VALUE, bottom=Integer.MIN_VALUE, topBlock=Integer.MIN_VALUE, height=16;
		double surfaceY=Double.NaN, supportY=Double.NaN;
		boolean obstacle=false;
		int down=i*RAYS_PER_COLUMN, high=down+1;
		int groundRay=TerrainSampling.groundRay(HIT_FLAGS[down]!=0,HIT_FLAGS[high]!=0,HITS[high*6+4],sample.flight());
		if (groundRay >= 0) {
			int g=down+groundRay;
			countAttribute(GROUND_ATTRS,HIT_ATTRS[g]);
			Vec3 hit=sampleMap.toMc(HITS[g*6],HITS[g*6+1],HITS[g*6+2]);
			double groundY=groundRay==1 ? Math.min(hit.y,sampleY+CLIFF_FILL) : hit.y;
			if (!Double.isFinite(groundY)) groundRay=-1;
			else {
				topBlock=Mth.floor(groundY-1e-4);
				height=Mth.clamp((int)Math.ceil((groundY-topBlock)*16.0-1e-3),1,16);
				double probeY=sampleY+PROBE_HEIGHT;
				// A chest-height wall probe cannot justify extruding a distant ground hit into a floor.
				if (sampleY-groundY <= CLIFF_FILL && !inPassage(x+0.5,z+0.5,groundY)) {
					for (int probe=TerrainSampling.FIRST_OBSTACLE_RAY;probe<RAYS_PER_COLUMN;probe++)
						obstacle |= isObstacle(down+probe,groundY,probeY,sampleMap);
				}
				top=obstacle ? Math.max(topBlock,Mth.floor(sampleY+WALL_HEIGHT)) : topBlock;
				bottom=groundRay==1 ? Math.min(topBlock,Mth.floor(sampleY)-THICKNESS) : topBlock-THICKNESS;
				surfaceY=obstacle ? top+1.0 : topBlock+height/16.0;
				// Only an actual upward-facing hit at the placed surface establishes grounded admission.
				if (!obstacle && Math.abs(hit.y-groundY)<0.1 && HITS[g*6+4]>0.3) supportY=surfaceY;
			}
		}
		List<Column> keep=new ArrayList<>();
		if (old!=null) {
			List<Column> previous=new ArrayList<>(old.retained());
			if (old.top()!=Integer.MIN_VALUE) previous.add(new Column(old.sampleY(),old.top(),old.bottom(),old.surfaceY(),old.supportY(),old.flight()));
			for (Column collision:previous) {
				boolean outside=TerrainSampling.outsideSurvey(collision.top(),collision.bottom(),sampleY-sample.below(),sampleY+(sample.flight()?TerrainSampling.FLIGHT_RAY_ABOVE:RAY_HIGH));
				// A first hit on a different storey does not prove the other floor disappeared.
				boolean separate=groundRay>=0 && (collision.top()<bottom || collision.bottom()>top);
				if (!outside && !separate) continue;
				// Nonoverlapping, build-clipped installed ranges bound the list by world height.
				if (collision.bottom()<level.getMinBuildHeight() || collision.top()>=level.getMaxBuildHeight()) continue;
				boolean certified=COLUMNS.current(key);
				keep.add(new Column(collision.sampleY(),collision.top(),collision.bottom(),certified?collision.surfaceY():Double.NaN,certified?collision.supportY():Double.NaN,collision.flight()));
			}
		}
		int first=Math.max(clearBottom,level.getMinBuildHeight()), last=Math.min(clearTop,level.getMaxBuildHeight()-1);
		int placeBottom=topBlock==Integer.MIN_VALUE ? Integer.MIN_VALUE : Math.max(bottom,level.getMinBuildHeight());
		int placeTop=topBlock==Integer.MIN_VALUE ? Integer.MIN_VALUE : Math.min(top,level.getMaxBuildHeight()-1);
		TerrainColumnWork work=topBlock==Integer.MIN_VALUE
			? new TerrainColumnWork(Integer.MIN_VALUE,Integer.MIN_VALUE,first,last)
			: new TerrainColumnWork(placeBottom,placeTop,first,last);
		// Record possibly touched heights before any edit. A discarded or invalidated
		// transaction must remain cleanable by a replacement sampled at another altitude.
		Column retained=old==null ? new Column(sampleY,Integer.MIN_VALUE,Integer.MIN_VALUE,Double.NaN,Double.NaN,sample.flight(),first,last,List.of())
			: new Column(old.sampleY(),old.top(),old.bottom(),old.surfaceY(),old.supportY(),old.flight(),first,last,old.retained());
		COLUMNS.retain(key,retained);
		return new ColumnApplication(sample,new Column(sampleY,top,bottom,surfaceY,supportY,sample.flight(),keep),topBlock,height,obstacle,work);
	}

	private static void collectResults(ServerLevel level, CoordMap.Mapping map) {
		if (pendingSeq<0) return;
		if (pendingContext==null || !pendingContext.matches(map,lastHostLife,RECALL_GEN.get())) { discardPending(); return; }
		ErLink link=ErLink.get();
		if (!hitsRead) {
			TerrainSampling.Query query=TerrainSampling.queryState(link.raysDone(pendingSeq),System.currentTimeMillis()-pendingSince);
			if (query!=TerrainSampling.Query.READY) {
				if (!pendingWarned && query==TerrainSampling.Query.DELAYED) {
					pendingWarned=true;
					LOG.warn("Terrain batch {} is delayed; retaining its native queue and holding unanswered ground",pendingSeq);
				}
				return;
			}
			link.readHits(PENDING_COLUMNS.size()*RAYS_PER_COLUMN,HITS,HIT_FLAGS,HIT_ATTRS);
			hitsRead=true;
			if (pendingWarned) LOG.info("Terrain batch {} answered after {} ms",pendingSeq,System.currentTimeMillis()-pendingSince);
			if (!raysAnswered) { raysAnswered=true; LOG.info("Terrain ray queries are working"); }
		}
		long started=System.nanoTime();
		int completed=0;
		while (applyingColumn<PENDING_COLUMNS.size() && completed<TerrainWorkBudget.APPLY_COLUMNS && !tickBudget.expired()) {
			PendingColumn sample=PENDING_COLUMNS.get(applyingColumn);
			if (!COLUMNS.matches(sample.key(),sample.revision())) {
				application=null; applyingColumn++; completed++; continue;
			}
			int x=ChunkPos.getX(sample.key()),z=ChunkPos.getZ(sample.key());
			LevelChunk chunk=level.getChunkSource().getChunkNow(x>>4,z>>4);
			if (chunk==null) {
				COLUMNS.invalidate(sample.key()); skippedUnloadedColumns++; applyingColumn++; application=null; completed++; continue;
			}
			if (application==null) application=prepareColumn(level,pendingContext.mapping(),applyingColumn);
			ColumnApplication job=application;
			BlockState terrain=ErBridgeMod.TERRAIN.defaultBlockState();
			boolean done=job.work.drain(tickBudget,
				y -> setTerrain(level,chunk,x,y,z,!job.obstacle && y==job.groundBlock ? terrain.setValue(TerrainBlock.HEIGHT,job.height) : terrain),
				y -> {
					for (Column collision:job.result.retained())
						if (y>=collision.bottom() && y<=collision.top()) return collision.top()+1;
					return TerrainCleanup.step(level,chunk,x,y,z);
				});
			if (!done) break;
			COLUMNS.putIfCurrent(sample.key(),job.result,sample.revision()); // only complete, still-current transactions certify readiness
			application=null; applyingColumn++; completed++;
		}
		long elapsed=System.nanoTime()-started;
		applyNanos+=elapsed; applyWorstNanos=Math.max(applyWorstNanos,elapsed); applyBatches++;
		if (applyingColumn==PENDING_COLUMNS.size()) discardPending();
	}

	// -- open doorways (the DLL's ErmcPassageTable) ------------------------------------------------

	/** Doubles per passage, in Minecraft coordinates: x, z, floor y, along x, along z, half width, half depth. */
	private static final int PF = TerrainSampling.PASSAGE_FIELDS;
	private static final float[] PASS_RAW = new float[Protocol.MAX_PASSAGES * Protocol.PASSAGE_FLOATS];
	private static final int[] PASS_ZONE = new int[1];
	private static double[] passages = new double[0];
	private static it.unimi.dsi.fastutil.ints.IntOpenHashSet passageIds = new it.unimi.dsi.fastutil.ints.IntOpenHashSet();
	private static long lastPassageReadMs;
	private static final long PASSAGE_TIMEOUT_MS = 2000;

	private static void clearPassages() {
		if (passages.length != 0) {
			resamplePassages(passages);
			discardPending();
		}
		passages = new double[0];
		passageIds.clear();
		lastPassageReadMs = 0;
	}

	/**
	 * Elden Ring's open doorways near the player. A 1 m opening at an angle to the block grid can't
	 * be carved out of 1-block wall columns, so inside these corridors no wall blocks are placed
	 * (floors are): open doors can be walked through. A door that opens or closes changes the
	 * set, and its surroundings are sampled again.
	 */
	private static void updatePassages(CoordMap.Mapping map) {
		int n = ErLink.get().readPassages(PASS_RAW, PASS_ZONE);
		if (n < 0) {
			if (System.currentTimeMillis() - lastPassageReadMs >= PASSAGE_TIMEOUT_MS) {
				clearPassages();
			}
			return;
		}
		lastPassageReadMs = System.currentTimeMillis();
		if (PASS_ZONE[0] != map.zone()) {
			n = 0;
		}
		double s = 1.0 / map.unitsPerMeter();
		double[] p = new double[n * PF];
		it.unimi.dsi.fastutil.ints.IntOpenHashSet ids = new it.unimi.dsi.fastutil.ints.IntOpenHashSet();
		for (int i = 0; i < n; i++) {
			int o = i * Protocol.PASSAGE_FLOATS;
			Vec3 c = map.toMc(PASS_RAW[o], PASS_RAW[o + 1], PASS_RAW[o + 2]);
			double yaw = PASS_RAW[o + 3];
			p[i * PF] = c.x;
			p[i * PF + 1] = c.z;
			p[i * PF + 2] = c.y;
			p[i * PF + 3] = Math.sin(yaw);
			p[i * PF + 4] = map.flipZ() ? -Math.cos(yaw) : Math.cos(yaw);
			p[i * PF + 5] = PASS_RAW[o + 4] * s;
			p[i * PF + 6] = PASS_RAW[o + 5] * s;
			ids.add(Float.floatToRawIntBits(PASS_RAW[o + 7]));
		}
		if (!ids.equals(passageIds) || !TerrainSampling.samePassages(p, passages)) {
			// A moved/reoriented corridor with the same IDs must invalidate its old carve too.
			// Pending rays were sampled before this door state; wait for a fresh batch.
			discardPending();
			resamplePassages(passages);
			resamplePassages(p);
			LOG.info("Open doorways: {} (no wall blocks inside them)", n);
			passageIds = ids;
			passages = p;
		}
	}

	private static void resamplePassages(double[] p) {
		for (int i=0;i+PF<=p.length;i+=PF) {
			int radius=(int)Math.ceil(p[i+5]+p[i+6])+1;
			RESAMPLE.add(Mth.floor(p[i]),Mth.floor(p[i+1]),radius,System.currentTimeMillis());
		}
	}

	private static boolean inPassage(double x, double z, double groundY) {
		return TerrainSampling.inPassage(passages, x, z, groundY);
	}

	private static void setTerrain(ServerLevel level, LevelChunk chunk, int x, int y, int z, BlockState state) {
		if (y<level.getMinBuildHeight() || y>=level.getMaxBuildHeight()) return;
		BlockPos pos=new BlockPos(x,y,z);
		BlockState cur=chunk.getBlockState(pos);
		if ((cur.isAir() || cur.is(ErBridgeMod.TERRAIN)) && cur!=state)
			level.setBlock(pos,state,Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
	}

	// -- anchor ------------------------------------------------------------------------------------

	private static void updateAnchor(MinecraftServer server) {
		CoordMap.Mapping m = CoordMap.get();
		double upm = STATE.unitsPerMeter > 0 ? STATE.unitsPerMeter : 100.0;
		int zone = STATE.stageId;
		if (zone == -1 || zone == 0) {
			return;  // loading screen: not a place
		}
		if (STATE.has(Protocol.STATE_PLAYER_VALID)) {
			if (m == null || m.provisional() || m.zone() != zone) {
				CoordMap.Mapping z = ANCHORS.get(zone);
				if (z == null && legacyAnchor != null) {
					z = new CoordMap.Mapping(legacyAnchor.ax(), legacyAnchor.ay(), legacyAnchor.az(), legacyAnchor.unitsPerMeter(),
						legacyAnchor.flipZ(), false, zone, 0);
					legacyAnchor = null;
				}
				if (z == null) {
					int region = ANCHORS.values().stream().mapToInt(CoordMap.Mapping::region).max().orElse(-1) + 1;
					z = new CoordMap.Mapping(STATE.playerPos[0], STATE.playerPos[1], STATE.playerPos[2], upm, CoordMap.HOST_FLIP_Z, false, zone, region);
					LOG.info("New Elden Ring zone {}: anchored to hunter at {}, {}, {} (region {})", zone,
						STATE.playerPos[0], STATE.playerPos[1], STATE.playerPos[2], region);
				} else {
					LOG.info("Elden Ring zone {} (region {})", zone, z.region());
				}
				ANCHORS.put(zone, z);
				CoordMap.set(z);
				saveAnchors(server);
				boolean moved = false;
				for (ServerPlayer p : server.getPlayerList().getPlayers()) {
					// Only move players who aren't already in this zone's region.
					if ((!CoopSession.enabled() || CoopSession.localPlayer(server, p)) && !z.contains(p.getX())) {
						teleportToHunter(server.overworld(), p, z);
						moved = true;
					}
				}
			}
		} else if (m == null) {
			// The host game is running but the hunter isn't known yet: map its origin so rendering
			// can already be tested. Replaced as soon as the hunter's position is available.
			CoordMap.set(new CoordMap.Mapping(0, 0, 0, upm, CoordMap.HOST_FLIP_Z, true, 0, 0));
		}
	}

	private static void teleportToHunter(ServerLevel level, ServerPlayer player, CoordMap.Mapping map) {
		teleportToHunter(level, player, map, true);
	}

	private static void teleportToHunter(ServerLevel level, ServerPlayer player, CoordMap.Mapping map, boolean prime) {
		teleportToHunter(level, player, map, prime, false);
	}

	private static void teleportToHunter(ServerLevel level, ServerPlayer player, CoordMap.Mapping map, boolean prime, boolean preserveView) {
		boolean known = STATE.has(Protocol.STATE_PLAYER_VALID);
		Vec3 target = known
			? map.toMc(STATE.playerPos[0], STATE.playerPos[1], STATE.playerPos[2])
			: new Vec3(map.originX(), CoordMap.MC_Y, CoordMap.MC_Z);
		// Take the Tarnished's place, looking where it looks (it stands in for this player
		// from now on). Its rotation is about +Y: forward = (sin a, 0, cos a) in Elden Ring.
		float yaw = player.getYRot();
		if (known && !preserveView) {
			double a = 2.0 * Math.atan2(STATE.playerQuat[1], STATE.playerQuat[3]);
			Vec3 f = map.dirToMc(Math.sin(a), 0.0, Math.cos(a));
			yaw = (float) Math.toDegrees(Math.atan2(-f.x, f.z));
		}
		teleportToFeet(level, player, target, yaw, preserveView ? player.getXRot() : 0.0F, prime);
	}

	private static void teleportToFeet(ServerLevel level, ServerPlayer player, Vec3 target, float yaw, float pitch, boolean prime) {
		// Repeating an already-applied correction need not resend a teleport or erase velocity.
		if (!prime && player.position().distanceToSqr(target.add(0,0.01,0))<0.0625*0.0625) return;
		player.teleportTo(level, target.x, target.y + 0.01, target.z, yaw, pitch);
		player.setDeltaMovement(Vec3.ZERO);
		player.fallDistance = 0.0F;
		if (prime) {
			Vec3 feet = target.add(0, 0.01, 0);
			GROUND_GUARDS.put(player.getUUID(), new TerrainSampling.GroundGuard(new TerrainSampling.Point(feet.x, feet.y, feet.z)));
			MOTION.remove(player.getUUID());
			FLIGHT_MODE.remove(player.getUUID());
			CRITICAL_GROUND.put(player.getUUID(), feet);
			ADMISSION.prime(player.getUUID(),feet);
			// F8, elevator recall and native floor correction must not trust old same-zone columns.
			for (int dx = -1; dx <= 1; dx++) {
				for (int dz = -1; dz <= 1; dz++) {
					COLUMNS.invalidate(ChunkPos.asLong(Mth.floor(feet.x) + dx, Mth.floor(feet.z) + dz));
				}
			}
			LOG.info("Moved {} to the Tarnished at {} (yaw {}); critical ground queued", player.getName().getString(), target, Math.round(yaw));
		}
	}

	private static boolean correctionNeedsPrime(ServerLevel level, Vec3 previous, Vec3 current, double width) {
		TerrainSampling.Point before=previous==null ? null : new TerrainSampling.Point(previous.x,previous.y,previous.z);
		TerrainSampling.Point after=new TerrainSampling.Point(current.x,current.y,current.z);
		return TerrainSampling.correctionNeedsPrime(before,after,readinessAt(level,current.add(0,0.01,0),width,false)==TerrainReadiness.SUPPORTED);
	}

	// -- persistence -------------------------------------------------------------------------------

	private static Path anchorPath(MinecraftServer server) {
		return server.getWorldPath(LevelResource.ROOT).resolve(ANCHOR_FILE);
	}

	private static void loadAnchors(MinecraftServer server) {
		Path p = anchorPath(server);
		if (!Files.exists(p)) {
			return;
		}
		Properties props = new Properties();
		try (Reader r = Files.newBufferedReader(p)) {
			props.load(r);
			String zones = props.getProperty("zones");
			if (zones == null && props.getProperty("ax") != null) {
				legacyAnchor = new CoordMap.Mapping(
					Double.parseDouble(props.getProperty("ax")), Double.parseDouble(props.getProperty("ay")),
					Double.parseDouble(props.getProperty("az")), Double.parseDouble(props.getProperty("unitsPerMeter", "100")),
					Boolean.parseBoolean(props.getProperty("flipZ", Boolean.toString(CoordMap.HOST_FLIP_Z))), false, 0, 0);
				LOG.info("Loaded legacy anchor; it becomes region 0 of the first zone seen");
				return;
			}
			if (zones == null || zones.isBlank()) {
				return;
			}
			for (String zs : zones.split(",")) {
				int zone = Integer.parseInt(zs.trim());
				if (zone == -1 || zone == 0) {
					continue;  // a loading screen once got anchored as a place; forget it
				}
				String k = "zone." + zone + ".";
				ANCHORS.put(zone, new CoordMap.Mapping(
					Double.parseDouble(props.getProperty(k + "ax")), Double.parseDouble(props.getProperty(k + "ay")),
					Double.parseDouble(props.getProperty(k + "az")), Double.parseDouble(props.getProperty(k + "unitsPerMeter", "100")),
					Boolean.parseBoolean(props.getProperty(k + "flipZ", Boolean.toString(CoordMap.HOST_FLIP_Z))), false, zone,
					Integer.parseInt(props.getProperty(k + "region", "0"))));
			}
			LOG.info("Loaded anchors for zones {}", ANCHORS.keySet());
		} catch (IOException | RuntimeException e) {
			LOG.warn("Ignoring unreadable anchor file {}: {}", p, e.toString());
		}
	}

	private static void saveAnchors(MinecraftServer server) {
		Properties props = new Properties();
		StringBuilder zones = new StringBuilder();
		for (CoordMap.Mapping m : ANCHORS.values()) {
			if (zones.length() > 0) {
				zones.append(',');
			}
			zones.append(m.zone());
			String k = "zone." + m.zone() + ".";
			props.setProperty(k + "ax", Double.toString(m.ax()));
			props.setProperty(k + "ay", Double.toString(m.ay()));
			props.setProperty(k + "az", Double.toString(m.az()));
			props.setProperty(k + "unitsPerMeter", Double.toString(m.unitsPerMeter()));
			props.setProperty(k + "flipZ", Boolean.toString(m.flipZ()));
			props.setProperty(k + "region", Integer.toString(m.region()));
		}
		props.setProperty("zones", zones.toString());
		try (Writer w = Files.newBufferedWriter(anchorPath(server))) {
			props.store(w, "Per zone: the host game position pinned to Minecraft (0.5 + region * 8192, 100, 0.5)");
		} catch (IOException e) {
			LOG.warn("Could not save anchors: {}", e.toString());
		}
	}
}
