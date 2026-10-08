package dev.ermc.bridge;

import dev.ermc.bridge.link.ErLink;
import dev.ermc.bridge.coop.CoopSession;
import dev.ermc.bridge.coop.PeerState;
import dev.ermc.bridge.entity.CombatBridge;
import dev.ermc.bridge.entity.combat.PlayerEventLedger;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.ResourceKey;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageType;
import net.minecraft.world.damagesource.DamageTypes;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.level.GameRules;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import java.util.HashSet;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

/**
 * One life shared by both games.
 * In opt-in co-op, each authenticated Minecraft player shares only their own ER life.
 *
 * <ul>
 *   <li>Minecraft's player dies (void, lava, mobs, an Elden Ring hit that took the last hearts...):
 *   the DLL kills the standing-in Tarnished with the game's own death ({@code H_MC_DEATHS}).</li>
 *   <li>The standing-in Tarnished dies in Elden Ring (it never dies of damage, but falling below
 *   the map does it): Minecraft's player dies too ({@code H_HOST_DEATHS}, damage type
 *   {@code erbridge:host_death}, which even creative mode doesn't survive).</li>
 *   <li>Respawn: Elden Ring's "YOU DIED" is the death screen, so Minecraft respawns at once
 *   ({@code doImmediateRespawn}) and draws nothing until Elden Ring has put the Tarnished at the
 *   last Site of Grace; then the player is moved there ({@link TerrainManager#requestRecall()}).</li>
 * </ul>
 */
public final class LifeBridge {
	private LifeBridge() {
	}

	private static final Logger LOG = LoggerFactory.getLogger("erbridge");
	/** "Steve died in the Lands Between": the Tarnished died, so Minecraft's player does too. */
	public static final ResourceKey<DamageType> HOST_DEATH = ResourceKey.create(Registries.DAMAGE_TYPE,
		ResourceLocation.fromNamespaceAndPath("erbridge", "host_death"));

	private static int lastHostDeaths = Integer.MIN_VALUE;
	private record LifeEpoch(long connection, int life) { }
	private static final PlayerEventLedger<LifeEpoch> COOP_DEATHS = new PlayerEventLedger<>();
	private static final Map<UUID, LifeEpoch> COOP_MC_DEATHS = new HashMap<>();

	public static void onServerStarted(MinecraftServer server) {
		lastHostDeaths = Integer.MIN_VALUE;
		COOP_DEATHS.clear();
		COOP_MC_DEATHS.clear();
		if (!TerrainManager.isBridgeWorld()) {
			return;
		}
		server.getGameRules().getRule(GameRules.RULE_DO_IMMEDIATE_RESPAWN).set(true, server);
	}

	public static void onServerTick(MinecraftServer server) {
		if (!TerrainManager.isBridgeWorld()) {
			return;
		}
		if (CoopSession.enabled()) {
			onCoopTick(server);
			return;
		}
		ErLink link = ErLink.get();
		if (!link.alive()) {
			return;
		}
		int deaths = link.hostDeaths();
		if (lastHostDeaths == Integer.MIN_VALUE) {
			lastHostDeaths = deaths;  // count from now on
			return;
		}
		if (deaths == lastHostDeaths) {
			return;
		}
		lastHostDeaths = deaths;
		DamageSource source = new DamageSource(server.overworld().registryAccess()
			.registryOrThrow(Registries.DAMAGE_TYPE).getHolderOrThrow(HOST_DEATH));
		for (ServerPlayer player : server.getPlayerList().getPlayers()) {
			if (player.isAlive()) {
				LOG.info("The Tarnished died in Elden Ring -> {} dies too", player.getName().getString());
				player.hurt(source, Float.MAX_VALUE);
			}
		}
	}

	private static void onCoopTick(MinecraftServer server) {
		if (!CoopSession.isHostAuthority(server)) {
			COOP_DEATHS.clear();
			COOP_MC_DEATHS.clear();
			return;
		}
		Set<UUID> connected = new HashSet<>();
		for (ServerPlayer player : server.getPlayerList().getPlayers()) {
			UUID id = player.getUUID();
			long connection = CoopSession.connectionEpoch(player);
			PeerState peer = CoopSession.state(player);
			if (!CoopSession.connected(player) || peer == null || connection == 0 || peer.connectionEpoch() != connection) {
				COOP_DEATHS.forget(id);
				COOP_MC_DEATHS.remove(id);
				continue;
			}
			connected.add(id);
			LifeEpoch epoch = new LifeEpoch(connection, peer.hostLife());
			if (!epoch.equals(COOP_MC_DEATHS.get(id))) COOP_MC_DEATHS.remove(id);
			COOP_DEATHS.bind(id, epoch);
			// A dead/busy ER is deliberately allowed here: requiring combat eligibility
			// would discard the very death notification that must reach its player.
			var delta = COOP_DEATHS.observe(id, epoch, peer.sequence(), peer.hostDeaths(), 0, 0, 0, server.getTickCount());
			if (delta.hostDeath() && player.isAlive()) {
				DamageSource source = new DamageSource(player.serverLevel().registryAccess()
					.registryOrThrow(Registries.DAMAGE_TYPE).getHolderOrThrow(HOST_DEATH));
				LOG.info("Paired Elden Ring died -> only {} dies", player.getName().getString());
				player.hurt(source, Float.MAX_VALUE);
			}
		}
		COOP_DEATHS.retain(connected);
		COOP_MC_DEATHS.keySet().retainAll(connected);
	}

	/** Any Minecraft player death except the one we caused above. */
	public static void onDeath(LivingEntity entity, DamageSource source) {
		if (!(entity instanceof ServerPlayer player) || !TerrainManager.isBridgeWorld() || source.is(HOST_DEATH)) {
			return;
		}
		if (CoopSession.enabled()) {
			MinecraftServer server = player.serverLevel().getServer();
			if (!CoopSession.isHostAuthority(server) || server.getPlayerList().getPlayer(player.getUUID()) != player) return;
			PeerState peer = CoopSession.state(player);
			long connection = CoopSession.connectionEpoch(player);
			if (peer == null || connection == 0 || peer.connectionEpoch() != connection) return;
			LifeEpoch epoch = new LifeEpoch(connection, peer.hostLife());
			if (epoch.equals(COOP_MC_DEATHS.put(player.getUUID(), epoch))) return;
			COOP_DEATHS.forget(player.getUUID());
			CombatBridge.resetPlayer(player.getUUID());
			// Vanilla /kill still controls permissions and target selection. Only its
			// GENERIC_KILL source bypasses the inactive-body mode restriction.
			boolean queued = CoopSession.routeMcDeath(player, source.is(DamageTypes.GENERIC_KILL));
			LOG.info("{} died ({}) -> paired client death {}", player.getName().getString(),
				source.getMsgId(), queued ? "queued" : "unavailable; not sent to another ER");
			return;
		}
		ErLink.get().bumpMcDeaths();
		LOG.info("{} died ({}) -> the Tarnished dies too if it was standing in", player.getName().getString(),
			source.getMsgId());
	}

	public static void onRespawn(ServerPlayer oldPlayer, ServerPlayer newPlayer, boolean alive) {
		if (!alive && TerrainManager.isBridgeWorld()) {
			if (CoopSession.enabled()) {
				COOP_DEATHS.forget(newPlayer.getUUID());
				COOP_MC_DEATHS.remove(newPlayer.getUUID());
				CombatBridge.resetPlayer(newPlayer.getUUID());
				TerrainManager.onRespawn(newPlayer);  // co-op: floor + hold + only this player's recall
				return;
			}
			TerrainManager.onRespawn(newPlayer);
		}
	}
}
