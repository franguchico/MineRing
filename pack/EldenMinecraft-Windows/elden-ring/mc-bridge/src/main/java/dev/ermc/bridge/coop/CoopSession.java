package dev.ermc.bridge.coop;

import com.mojang.authlib.GameProfile;
import dev.ermc.bridge.CoordMap;
import dev.ermc.bridge.TerrainManager;
import dev.ermc.bridge.mixin.CoopPlayerListAccess;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.network.chat.Component;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.players.UserWhiteListEntry;

/** Integrated-server authority. All mutable methods run on the logical server thread. */
public final class CoopSession {
    private CoopSession() {}
    private static final AtomicLong EPOCHS = new AtomicLong(System.currentTimeMillis());
    private static final AtomicLong CLIENT_TERRAIN_CHANGES = new AtomicLong();
    public static void clientTerrainChanged() { CLIENT_TERRAIN_CHANGES.incrementAndGet(); }
    public static long clientTerrainGeneration() { return CLIENT_TERRAIN_CHANGES.get(); }
    private static final Map<UUID, PeerChannel> PEERS = new HashMap<>();
    private static final Map<UUID, net.minecraft.server.network.ServerGamePacketListenerImpl> CONNECTIONS = new HashMap<>();
    private static final Map<UUID, Long> RECEIVED = new HashMap<>();
    private static final Map<UUID, Long> JOINED = new HashMap<>(), RESAMPLES = new HashMap<>();
    private static volatile MinecraftServer authority;
    public static boolean enabled() { return Boolean.getBoolean("erbridge.coop"); }
    public static boolean guest() { return enabled() && "guest".equals(System.getProperty("erbridge.coop.role", "host")); }
    public static boolean isHostAuthority(MinecraftServer server) {
        return !enabled() || (!guest() && server != null && server == authority);
    }
    public static boolean localPlayer(MinecraftServer server, ServerPlayer player) {
        GameProfile owner = server == null ? null : server.getSingleplayerProfile();
        return owner != null && owner.getId().equals(player.getUUID());
    }
    public static void register() {
        CoopPayloads.register();
        ServerPlayNetworking.registerGlobalReceiver(CoopPayloads.Snapshot.TYPE, (p, c) -> receive(c.player(), p));
        CoopModeSession.register();
    }
    public static void start(MinecraftServer server) {
        reset();
        if (!enabled() || guest() || !TerrainManager.isBridgeWorld() || server.getSingleplayerProfile() == null) return;
        authority = server;
        secure(server);
    }
    /** Must complete before publishServer opens its listener (including e4mc's relay hook). */
    public static void secure(MinecraftServer server) {
        if (!isHostAuthority(server) || !enabled()) throw new IllegalStateException("Not the co-op authority");
        server.setUsesAuthentication(true);
        server.setEnforceWhitelist(true);
        var list = server.getPlayerList();
        list.reloadWhiteList();
        list.setUsingWhiteList(true);
        list.getWhiteList().add(new UserWhiteListEntry(server.getSingleplayerProfile()));
        list.setAllowCommandsForAllPlayers(false);
        ((CoopPlayerListAccess) list).erbridge$maxPlayers(CoopProtocol.MAX_PLAYERS);
        // CoopAdmissionMixin also denies isOp for guests, including old ops.json entries.
        if (!server.usesAuthentication() || !list.isUsingWhitelist() || list.getMaxPlayers() != 2)
            throw new IllegalStateException("Co-op authentication/whitelist/limit setup failed");
    }
    public static Component admission(MinecraftServer server, GameProfile profile) {
        if (!enabled() || server != authority) return null;
        if (!server.usesAuthentication() || !server.getPlayerList().isUsingWhitelist())
            return Component.literal("ER Bridge co-op requires authenticated online mode and whitelist");
        if (!ownerProfile(server, profile) && !server.getPlayerList().getWhiteList().isWhiteListed(profile))
            return Component.translatable("multiplayer.disconnect.not_whitelisted");
        if (server.getPlayerList().getPlayerCount() >= 2 && server.getPlayerList().getPlayer(profile.getId()) == null)
            return Component.translatable("multiplayer.disconnect.server_full");
        return null;
    }
    public static boolean ownerProfile(MinecraftServer server, GameProfile profile) {
        return server.getSingleplayerProfile() != null && server.getSingleplayerProfile().getId().equals(profile.getId());
    }
    public static void join(MinecraftServer server, ServerPlayer player) {
        if (!enabled() || !isHostAuthority(server)) return;
        Component rejection = admission(server, player.getGameProfile());
        if (rejection != null || (PEERS.size() >= 2 && !PEERS.containsKey(player.getUUID()))) {
            player.connection.disconnect(rejection != null ? rejection : Component.translatable("multiplayer.disconnect.server_full"));
            return;
        }
        PEERS.put(player.getUUID(), new PeerChannel(EPOCHS.incrementAndGet()));
        CONNECTIONS.put(player.getUUID(), player.connection);
        RECEIVED.remove(player.getUUID()); RESAMPLES.remove(player.getUUID());
        JOINED.put(player.getUUID(), System.nanoTime());
        if (!localPlayer(server, player)) server.getPlayerList().deop(player.getGameProfile());
    }
    public static void disconnect(ServerPlayer player) {
        if (CONNECTIONS.get(player.getUUID()) != player.connection) return;
        CoopModeSession.disconnect(player);
        PEERS.remove(player.getUUID()); JOINED.remove(player.getUUID()); RESAMPLES.remove(player.getUUID());
        CONNECTIONS.remove(player.getUUID()); RECEIVED.remove(player.getUUID());
    }
    private static PeerChannel peer(ServerPlayer player) {
        return CONNECTIONS.get(player.getUUID()) == player.connection ? PEERS.get(player.getUUID()) : null;
    }
    public static void reset() { PEERS.clear(); CONNECTIONS.clear(); RECEIVED.clear(); JOINED.clear(); RESAMPLES.clear(); authority = null; CoopModeSession.reset(); }
    public static boolean connected(ServerPlayer player) { return peer(player) != null; }
    public static long connectionEpoch(ServerPlayer player) {
        PeerChannel p = peer(player); return p == null ? 0 : p.epoch;
    }
    public static PeerState state(ServerPlayer player) {
        PeerChannel p = peer(player); return p == null ? null : p.fresh(System.nanoTime());
    }
    private static PeerState host() {
        if (authority == null || authority.getSingleplayerProfile() == null) return null;
        PeerChannel p = PEERS.get(authority.getSingleplayerProfile().getId());
        return p == null ? null : p.fresh(System.nanoTime());
    }
    /** Spatial eligibility deliberately excludes recall/ground ACK, allowing terrain to prime them. */
    public static int spatialStatus(ServerPlayer player) {
        if (!connected(player)) return CoopProtocol.HANDSHAKE;
        CoordMap.Mapping map = CoordMap.get();
        if (!CoordMap.validShared(map)) return CoopProtocol.HOST_UNAVAILABLE;
        PeerState peer = state(player);
        int status = CoopProtocol.spatialStatus(host(), peer, map.unitsPerMeter());
        if (status != CoopProtocol.READY) return status;
        if (map.zone() != peer.stageId() || player.serverLevel() != authority.overworld()) return CoopProtocol.OTHER_ZONE;
        return CoopProtocol.READY;
    }
    public static boolean eligible(ServerPlayer player) {
        if (!enabled()) return true;
        if (spatialStatus(player) != CoopProtocol.READY) return false;
        var m = CoordMap.get(); var h = host();
        if (h == null) return false;
        // Before the first recall the old Minecraft spawn is allowed only for priming.
        PeerChannel channel = peer(player);
        return channel.recallPending() || player.position().distanceToSqr(m.toMc(h.hostX(), h.hostY(), h.hostZ()))
            <= CoopProtocol.RANGE_METERS * CoopProtocol.RANGE_METERS;
    }
    public static boolean recallPending(ServerPlayer player) {
        PeerChannel p = peer(player); return p != null && p.recallPending();
    }
    public static void recalled(ServerPlayer player) {
        PeerChannel p = peer(player); if (p != null) p.recalled();
    }
    public static void requestRecall(ServerPlayer player) {
        if (!enabled()) { TerrainManager.requestRecall(); return; }
        PeerChannel p = peer(player); if (p != null) p.requestRecall();
    }
    public static boolean routeMcDeath(ServerPlayer player) {
        return routeMcDeath(player, false);
    }
    /** Invoked only by the server death callback; the payload cannot request a reset. */
    public static boolean routeMcDeath(ServerPlayer player, boolean explicitReset) {
        PeerChannel p = peer(player);
        if (!enabled() || p == null || !isHostAuthority(player.serverLevel().getServer()) || state(player) == null) return false;
        if (explicitReset) return p.routeExplicitReset(System.nanoTime());
        if (CoopModeSession.mode(player, System.nanoTime()) != CoopModeLedger.MINECRAFT) return false;
        p.routeDeath(); return true;
    }
    private static void receive(ServerPlayer player, CoopPayloads.Snapshot p) {
        if (!enabled() || authority == null) return;
        PeerChannel channel = peer(player);
        if (channel == null) return;
        if (p.version() != CoopProtocol.VERSION || !p.enabled()) {
            player.connection.disconnect(Component.literal("ER Bridge co-op protocol/mode mismatch")); return;
        }
        long now = System.nanoTime();
        // Payloads are fixed-size; throttle accepted work to 20 Hz, including terrain invalidation.
        if (now - RECEIVED.getOrDefault(player.getUUID(), 0L) < 50_000_000L) return;
        RECEIVED.put(player.getUUID(), now);
        if (!CoopProtocol.counter(p.resample()) || !channel.accept(p.peer(now), p.recall(), p.deathAck())) return;
        long previous = RESAMPLES.getOrDefault(player.getUUID(), 0L);
        if (p.resample() > previous && eligible(player)) {
            RESAMPLES.put(player.getUUID(), p.resample());
            TerrainManager.resampleAround(player.getX(), player.getZ(), 4);
        }
    }
    /** Registered after terrain/life processing so ACK observes completed teleports and routed deaths. */
    public static void tick(MinecraftServer server) {
        CoopModeSession.tick(server);
        if (!enabled() || !isHostAuthority(server) || server.getTickCount() % 2 != 0) return;
        for (ServerPlayer player : server.getPlayerList().getPlayers()) {
            PeerChannel p = peer(player); if (p == null) continue;
            if (p.sequence() == 0 && System.nanoTime() - JOINED.getOrDefault(player.getUUID(), 0L) > 10_000_000_000L) {
                player.connection.disconnect(Component.literal("ER Bridge co-op handshake timed out")); continue;
            }
            if (p.sequence() == 0) {
                // No changed ACK is sent until the peer's original-layout Snapshot proves v4.
                if (ServerPlayNetworking.canSend(player, CoopPayloads.Handshake.TYPE))
                    ServerPlayNetworking.send(player, new CoopPayloads.Handshake(new CoopPayloads.Ack(
                        CoopProtocol.VERSION, p.epoch, 0, CoopProtocol.HANDSHAKE, null, 0, 0, 0, 0)));
                continue;
            }
            int status = spatialStatus(player);
            if (status == CoopProtocol.READY && !eligible(player)) status = CoopProtocol.TOO_FAR;
            if (status == CoopProtocol.READY && p.recallPending()) status = CoopProtocol.RECALL_PENDING;
            if (status == CoopProtocol.READY && !TerrainManager.criticalGroundReady(player)) status = groundStatus(player);
            if (ServerPlayNetworking.canSend(player, CoopPayloads.Ack.TYPE))
                ServerPlayNetworking.send(player, new CoopPayloads.Ack(CoopProtocol.VERSION, p.epoch, p.sequence(), status,
                    CoordMap.validShared(CoordMap.get()) ? CoordMap.get() : null, p.recalledGeneration(), p.recalledClient(), p.deathSequence(), p.deathLife(), p.deathKind(), p.deathSample()));
        }
    }

    /** Server-thread read-only view, also safe before a handshake or while a peer is stale. */
    public static void logStatus(MinecraftServer server) {
        var log = org.slf4j.LoggerFactory.getLogger("erbridge");
        var list = server.getPlayerList();
        log.info("[devcmd] coopstatus server authority={} auth={} whitelist={} enforceWhitelist={} maxPlayers={} playerNames={} whitelistNames={}",
            isHostAuthority(server), server.usesAuthentication(), list.isUsingWhitelist(), server.isEnforceWhitelist(),
            list.getMaxPlayers(), java.util.Arrays.toString(list.getPlayerNamesArray()), java.util.Arrays.toString(list.getWhiteListNames()));
        for (ServerPlayer player : list.getPlayers()) {
            PeerChannel channel = peer(player);
            PeerState sample = state(player);
            int status = spatialStatus(player);
            boolean eligible = eligible(player);
            if (status == CoopProtocol.READY && !eligible) status = CoopProtocol.TOO_FAR;
            if (status == CoopProtocol.READY && recallPending(player)) status = CoopProtocol.RECALL_PENDING;
            if (status == CoopProtocol.READY && !TerrainManager.criticalGroundReady(player)) status = groundStatus(player);
            log.info("[devcmd] coopstatus peer name={} local={} connected={} epoch={} acceptedSeq={} fresh={} eligible={} status={} reason={} life={} flags={} recallPending={}",
                player.getGameProfile().getName(), localPlayer(server, player), connected(player),
                channel == null ? 0 : channel.epoch, channel == null ? 0 : channel.sequence(), sample != null,
                eligible, status, CoopProtocol.reason(status), sample == null ? -1 : sample.hostLife(),
                sample == null ? 0 : sample.flags(), recallPending(player));
        }
    }

    private static int groundStatus(ServerPlayer player) {
        return TerrainManager.supportReadiness(player) == dev.ermc.bridge.TerrainReadiness.UNSUPPORTED
            ? CoopProtocol.UNSUPPORTED_GROUND : CoopProtocol.GROUND_PENDING;
    }
}
