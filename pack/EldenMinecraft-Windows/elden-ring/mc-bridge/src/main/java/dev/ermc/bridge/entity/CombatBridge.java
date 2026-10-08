package dev.ermc.bridge.entity;

import dev.ermc.bridge.CoordMap;
import dev.ermc.bridge.TerrainManager;
import dev.ermc.bridge.coop.CoopSession;
import dev.ermc.bridge.coop.PeerState;
import dev.ermc.bridge.entity.combat.PlayerEventLedger;
import dev.ermc.bridge.link.ErLink;
import dev.ermc.bridge.link.Protocol;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.ResourceKey;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageType;
import net.minecraft.world.phys.Vec3;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import java.util.HashSet;
import java.util.Set;
import java.util.UUID;

/**
 * Host game -> Minecraft damage. While the Tarnished stands in for the Minecraft player, the
 * DLL reports the hits it takes (HP lost, and the nearest hostile character as the likely
 * attacker) and the HP it loses to status effects.
 * <ul>
 *   <li>Each hit is a Minecraft attack by that enemy's proxy (the live enemy proxy nearest the
 *   reported attacker, never an NPC's): a shield raised towards it blocks it, armor reduces
 *   it, it knocks back, and tamed wolves go for the attacker. Its size is a base by attacker
 *   size plus the share of the Tarnished's max HP lost, so it doesn't depend on the host game's
 *   HP scale.</li>
 *   <li>Status damage drains health once a second, proportional to the share of HP lost;
 *   shields and armor don't help (damage type {@code erbridge:status}).</li>
 * </ul>
 * Creative mode is invulnerable as usual.
 */
public final class CombatBridge {
	private CombatBridge() {
	}

	private static final Logger LOG = LoggerFactory.getLogger("erbridge");
	/** Attack by an enemy with a Minecraft proxy ("slain by c4300"). */
	public static final ResourceKey<DamageType> MONSTER = key("monster");
	/** Attack by an enemy without a proxy nearby ("slain by a monster"). */
	public static final ResourceKey<DamageType> MONSTER_HIT = key("monster_hit");
	/** Poison, fire and other status damage: bypasses shields and armor, no knockback. */
	public static final ResourceKey<DamageType> STATUS = key("status");

	private static final float[] EV = new float[9];
	/** Minecraft damage per host-game hit, before Minecraft armor: a base by attacker size plus a share of the hit. */
	private static final float SMALL_BASE = 3.0F;
	private static final float LARGE_BASE = 6.0F;
	private static final float PROPORTIONAL = 30.0F; // x (host-game damage / the Tarnished's max HP)
	/** The DLL's attacker position is the enemy's feet; its proxy's box is searched this far around it. */
	private static final double BLAME_RADIUS = 32.0;
	private static int lastHits = -1;
	private static float lastTotal;
	private static float lastStatus;
	private static float pendingStatus;
	private static int statusTicks;
	private record LifeEpoch(long connection, int life) { }
	private static final PlayerEventLedger<LifeEpoch> COOP_DAMAGE = new PlayerEventLedger<>();

	private static ResourceKey<DamageType> key(String name) {
		return ResourceKey.create(Registries.DAMAGE_TYPE, ResourceLocation.fromNamespaceAndPath("erbridge", name));
	}

	private static Holder<DamageType> type(ServerLevel level, ResourceKey<DamageType> key) {
		return level.registryAccess().registryOrThrow(Registries.DAMAGE_TYPE).getHolderOrThrow(key);
	}

	public static void reset() {
		lastHits = -1;
		pendingStatus = 0.0F;
		statusTicks = 0;
		COOP_DAMAGE.clear();
	}

	/** Called at only this player's death/respawn; queued poison never crosses lives. */
	public static void resetPlayer(UUID player) { COOP_DAMAGE.forget(player); }

	public static void onServerTick(MinecraftServer server) {
		if (CoopSession.enabled()) {
			onCoopTick(server);
			return;
		}
		if (!TerrainManager.isBridgeWorld() || !ErLink.get().readHunterEvents(EV)) {
			return;
		}
		int hits = (int) EV[0];
		float total = EV[1];
		float status = EV[8];
		if (lastHits < 0) {  // first read: only count damage from now on
			lastHits = hits;
			lastTotal = total;
			lastStatus = status;
			return;
		}
		float maxHp = EV[6];  // the Tarnished's max HP; 0 until the DLL reported a hit
		ServerLevel level = server.overworld();
		if (hits != lastHits) {
			float hostDamage = total - lastTotal;
			lastHits = hits;
			lastTotal = total;
			CoordMap.Mapping map = CoordMap.get();
			Vec3 from = map != null ? map.toMc(EV[3], EV[4], EV[5]) : null;
			ErEntity attacker = from != null ? EntityBridge.nearestEnemy(from, BLAME_RADIUS) : null;
			// The host game's damage after the Tarnished's armor says little in Minecraft terms, so
			// scale: small enemies hit like a zombie, bosses much harder.
			int kind = attacker != null ? attacker.kind() : (int) EV[7];
			float share = maxHp > 0 ? hostDamage / maxHp : 0.0F;
			float amount = Math.min(20.0F, (kind == Protocol.ENT_SMALL_MONSTER ? SMALL_BASE : LARGE_BASE) + share * PROPORTIONAL);
			for (ServerPlayer player : server.getPlayerList().getPlayers()) {
				hit(level, player, from, attacker, amount, hostDamage, share);
			}
		}

		pendingStatus += Math.max(0.0F, status - lastStatus);
		lastStatus = status;
		if (++statusTicks >= 20) {
			statusTicks = 0;
			float amount = maxHp > 0 ? pendingStatus / maxHp * PROPORTIONAL : 0.0F;
			if (amount >= 0.05F) {
				for (ServerPlayer player : server.getPlayerList().getPlayers()) {
					player.hurt(new DamageSource(type(level, STATUS)), amount);
				}
				LOG.info("Elden Ring status damage {} HP -> {} Minecraft damage", pendingStatus, amount);
			}
			pendingStatus = 0.0F;
		}
	}

	private static void onCoopTick(MinecraftServer server) {
		if (!TerrainManager.isBridgeWorld() || !CoopSession.isHostAuthority(server)) {
			COOP_DAMAGE.clear();
			return;
		}
		Set<UUID> active = new HashSet<>();
		CoordMap.Mapping map = CoordMap.get();
		for (ServerPlayer player : server.getPlayerList().getPlayers()) {
			UUID id = player.getUUID();
			PeerState peer = CoopSession.state(player);
			long connection = CoopSession.connectionEpoch(player);
			if (peer == null || connection == 0 || peer.connectionEpoch() != connection || !player.isAlive() ||
				dev.ermc.bridge.coop.CoopBodyGuard.isSuspended(player) ||
				!CoopSession.eligible(player) || CoopSession.recallPending(player) ||
				map == null || map.provisional() || peer.stageId() != map.zone()) {
				COOP_DAMAGE.forget(id);
				continue;
			}
			float[] events = peer.hunterEvents();
			boolean valid = events.length == 9;
			for (float value : events) valid &= Float.isFinite(value);
			if (!valid || events[0] < 0 || events[0] > 0xffff_ffffL || events[6] < 0) {
				COOP_DAMAGE.forget(id);
				continue;
			}
			active.add(id);
			LifeEpoch epoch = new LifeEpoch(connection, peer.hostLife());
			COOP_DAMAGE.bind(id, epoch);
			var delta = COOP_DAMAGE.observe(id, epoch, peer.sequence(), peer.hostDeaths(),
				(long) events[0], events[1], events[8], server.getTickCount());
			if (!(events[6] > 0)) continue;
			ServerLevel level = player.serverLevel();
			if (delta.hit()) {
				Vec3 from = map.toMc(events[3], events[4], events[5]);
				ErEntity attacker = EntityBridge.nearestEnemy(from, BLAME_RADIUS);
				int kind = attacker != null ? attacker.kind() : (int) events[7];
				float share = delta.damage() / events[6];
				float amount = Math.min(20.0F, (kind == Protocol.ENT_SMALL_MONSTER ? SMALL_BASE : LARGE_BASE) + share * PROPORTIONAL);
				hit(level, player, from, attacker, amount, delta.damage(), share);
			}
			float statusAmount = (float) Math.min(Float.MAX_VALUE, (double) delta.status() / events[6] * PROPORTIONAL);
			if (statusAmount >= 0.05F && player.isAlive()) {
				player.hurt(new DamageSource(type(level, STATUS)), statusAmount);
			}
		}
		COOP_DAMAGE.retain(active);
	}

	private static void hit(ServerLevel level, ServerPlayer player, Vec3 from, ErEntity attacker, float amount, float hostDamage,
							float share) {
		DamageSource source;
		if (attacker != null) {
			source = new DamageSource(type(level, MONSTER), attacker);
		} else if (from != null) {
			source = new DamageSource(type(level, MONSTER_HIT), from);
		} else {
			source = new DamageSource(type(level, MONSTER_HIT));
		}
		// Blocking needs to know where the blow came from: the attacker's position.
		boolean blocked = player.isDamageSourceBlocked(source);
		boolean hurt = player.hurt(source, amount);
		// Minecraft already knocks back by 0.4 away from the attacker; big host-game blows push
		// harder, unless the shield took them.
		if (hurt && !blocked && from != null) {
			double dx = from.x - player.getX();
			double dz = from.z - player.getZ();
			double extra = Math.min(1.1, share * 3.0);
			if (extra > 0.05 && dx * dx + dz * dz > 1e-4) {
				player.knockback(extra, dx, dz);
				player.hurtMarked = true;
			}
		}
		LOG.info("Elden Ring hit {} for {} HP ({}% of max) -> {} Minecraft damage from {}{}", player.getName().getString(), hostDamage,
			Math.round(share * 100.0F), amount, attacker != null ? attacker.getName().getString() : "?",
			blocked ? " (blocked by shield)" : hurt ? "" : " (ignored: creative/invulnerable)");
	}
}
