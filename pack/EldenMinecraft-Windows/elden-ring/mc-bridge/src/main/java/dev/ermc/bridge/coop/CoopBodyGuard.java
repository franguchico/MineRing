package dev.ermc.bridge.coop;

import dev.ermc.bridge.LifeBridge;
import dev.ermc.bridge.TerrainManager;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.tags.DamageTypeTags;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageTypes;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.phys.Vec3;

/** Server-thread protection of the MC representation during handoff/recall/network holds. */
public final class CoopBodyGuard {
    private CoopBodyGuard() {}
    private record Lease(ServerPlayer player, long epoch, CoopBodyPolicy policy) {}
    private static final Map<UUID, Lease> LEASES = new HashMap<>();

    private static CoopBodyPolicy policy(ServerPlayer player, long now) {
        MinecraftServer server = player.serverLevel().getServer();
        long epoch = CoopSession.connectionEpoch(player);
        if (!CoopSession.enabled() || !TerrainManager.isBridgeWorld()
                || !CoopSession.isHostAuthority(server) || epoch == 0
                || server.getPlayerList().getPlayer(player.getUUID()) != player) return null;
        Lease lease = LEASES.get(player.getUUID());
        if (lease == null || lease.player() != player || lease.epoch() != epoch) {
            lease = new Lease(player, epoch, new CoopBodyPolicy());
            LEASES.put(player.getUUID(), lease);
        }
        boolean minecraft = CoopModeSession.mode(player, now) == CoopModeLedger.MINECRAFT;
        boolean ready = minecraft && CoopSession.eligible(player) && !CoopSession.recallPending(player)
            && TerrainManager.criticalGroundReady(player);
        lease.policy().update(minecraft, ready, now);
        return lease.policy();
    }

    public static boolean isSuspended(ServerPlayer player) {
        long now = System.nanoTime();
        CoopBodyPolicy policy = policy(player, now);
        // A non-fall ordinary hazard is blocked exactly while this body is suspended.
        return policy != null && policy.blocksDamage(false, false, false, now);
    }

    public static void stopMotion(ServerPlayer player) {
        player.setDeltaMovement(Vec3.ZERO);
        player.fallDistance = 0;
        // Preserve elytra state through a short terrain/network hold, but not while using ER.
        if (CoopModeSession.mode(player, System.nanoTime()) == CoopModeLedger.ELDEN_RING && player.isFallFlying())
            player.stopFallFlying();
    }

    public static boolean allowDamage(LivingEntity entity, DamageSource source, float amount) {
        if (!(entity instanceof ServerPlayer player)) return true;
        long now = System.nanoTime();
        CoopBodyPolicy policy = policy(player, now);
        if (policy == null) return true;
        boolean blocked = policy.blocksDamage(source.is(LifeBridge.HOST_DEATH),
            source.is(DamageTypes.GENERIC_KILL), source.is(DamageTypeTags.IS_FALL), now);
        if (blocked && source.is(DamageTypeTags.IS_FALL)) player.fallDistance = 0;
        return !blocked;
    }

    public static void tick(MinecraftServer server) {
        if (!CoopSession.enabled() || !CoopSession.isHostAuthority(server)) { reset(); return; }
        LEASES.entrySet().removeIf(e -> server.getPlayerList().getPlayer(e.getKey()) != e.getValue().player()
            || CoopSession.connectionEpoch(e.getValue().player()) != e.getValue().epoch());
        for (ServerPlayer player : server.getPlayerList().getPlayers()) {
            if (isSuspended(player)) stopMotion(player);
        }
    }

    public static void reset() { LEASES.clear(); }
}
