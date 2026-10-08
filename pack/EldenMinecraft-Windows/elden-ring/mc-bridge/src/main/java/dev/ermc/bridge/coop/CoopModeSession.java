package dev.ermc.bridge.coop;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;

/** Mode authority for visibility and the v3 body lifecycle; all mutation is server-thread bound. */
public final class CoopModeSession {
    private CoopModeSession() {}
    private static final CoopModeLedger LEDGER = new CoopModeLedger();
    private static final Map<UUID, Delivery> DELIVERIES = new HashMap<>(CoopModeLedger.MAX_PLAYERS);
    private static List<CoopModeLedger.Entry> previous = List.of();
    private static final class Delivery {
        final long epoch;
        long sequence;
        Delivery(long epoch) { this.epoch = epoch; }
    }

    public static void register() {
        CoopModePayloads.register();
        ServerPlayNetworking.registerGlobalReceiver(CoopModePayloads.ModeSnapshot.TYPE,
            (payload, context) -> receive(context.player(), payload));
    }
    public static void reset() { LEDGER.reset(); DELIVERIES.clear(); previous = List.of(); }

    public static int mode(ServerPlayer player, long now) {
        long epoch = CoopSession.connectionEpoch(player);
        return epoch > 0 && nativeFresh(player, epoch)
            ? LEDGER.mode(player.getUUID(), epoch, now) : CoopModeLedger.UNKNOWN;
    }

    private static boolean authority(MinecraftServer server) {
        return CoopSession.enabled() && server != null && CoopSession.isHostAuthority(server)
            && server.usesAuthentication();
    }
    private static boolean nativeFresh(ServerPlayer player, long epoch) {
        PeerState peer = CoopSession.state(player);
        // Keep selected mode through death/busy/recall/ground holds. Only native presence is required.
        return peer != null && peer.connectionEpoch() == epoch && (peer.flags() & 2) != 0;
    }

    public static void receive(ServerPlayer sender, CoopModePayloads.ModeSnapshot payload) {
        if (sender == null || payload == null || !authority(sender.serverLevel().getServer())) return;
        long epoch = CoopSession.connectionEpoch(sender);
        LEDGER.accept(sender.getUUID(), epoch, nativeFresh(sender, epoch), payload.version(),
            payload.epoch(), payload.sequence(), payload.steamId64(), payload.mode(), System.nanoTime());
    }

    public static void disconnect(ServerPlayer player) {
        long epoch = CoopSession.connectionEpoch(player);
        if (epoch == 0) return; // CoopSession has already rejected callbacks from an older connection.
        LEDGER.disconnect(player.getUUID(), epoch);
        Delivery delivery = DELIVERIES.get(player.getUUID());
        if (delivery != null && delivery.epoch == epoch) DELIVERIES.remove(player.getUUID());
        // Relay removal in this callback, without waiting for the next periodic tick.
        relay(player.serverLevel().getServer(), player.getUUID(), true);
    }

    public static void tick(MinecraftServer server) { relay(server, null, false); }

    private static void relay(MinecraftServer server, UUID excluded, boolean force) {
        if (!authority(server)) { reset(); return; }
        var players = new ArrayList<ServerPlayer>(CoopModeLedger.MAX_PLAYERS);
        var connections = new ArrayList<CoopModeLedger.Connection>(CoopModeLedger.MAX_PLAYERS);
        for (ServerPlayer player : server.getPlayerList().getPlayers()) {
            if (player.getUUID().equals(excluded)) continue;
            long epoch = CoopSession.connectionEpoch(player);
            if (epoch == 0) continue;
            if (players.size() == CoopModeLedger.MAX_PLAYERS) break;
            players.add(player);
            connections.add(new CoopModeLedger.Connection(player.getUUID(), epoch, nativeFresh(player, epoch)));
        }
        LEDGER.retain(connections);
        DELIVERIES.entrySet().removeIf(saved -> connections.stream().noneMatch(c ->
            saved.getKey().equals(c.playerId()) && saved.getValue().epoch == c.epoch()));
        List<CoopModeLedger.Entry> entries = LEDGER.snapshot(System.nanoTime());
        boolean changed = !previous.equals(entries);
        previous = entries;
        // Heartbeat at 10 Hz, and send expirations/disconnects as soon as observed on any tick.
        if (!force && !changed && server.getTickCount() % 2 != 0) return;
        var peers = new ArrayList<CoopModePayloads.ModePeer>(entries.size());
        for (var e : entries)
            peers.add(new CoopModePayloads.ModePeer(e.playerId(), e.steamId64(), e.mode(), e.sequence(), e.connectionEpoch()));
        for (ServerPlayer player : players) {
            // Missing channels cannot prove active MC ownership; the v3 guard remains suspended.
            if (!ServerPlayNetworking.canSend(player, CoopModePayloads.ModeRoster.TYPE)) continue;
            long epoch = CoopSession.connectionEpoch(player);
            if (!nativeFresh(player, epoch)) continue;
            Delivery delivery = DELIVERIES.get(player.getUUID());
            if (delivery == null || delivery.epoch != epoch) {
                delivery = new Delivery(epoch);
                DELIVERIES.put(player.getUUID(), delivery);
            }
            if (!CoopModeLedger.sequence(delivery.sequence + 1)) continue;
            ServerPlayNetworking.send(player, new CoopModePayloads.ModeRoster(CoopModePayloads.VERSION,
                epoch, ++delivery.sequence, peers));
        }
    }
}
