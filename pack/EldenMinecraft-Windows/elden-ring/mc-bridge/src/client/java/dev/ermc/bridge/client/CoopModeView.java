package dev.ermc.bridge.client;

import dev.ermc.bridge.coop.CoopModePayloads;
import dev.ermc.bridge.coop.CoopProtocol;
import dev.ermc.bridge.link.NativePeerInfo;
import java.util.Map;
import java.util.HashMap;
import java.util.UUID;

/** Authoritative, expiring presentation state, independent of who is looking at the peer. */
public final class CoopModeView {
    private long epoch, sequence, receivedAt;
    private Map<UUID, CoopModePayloads.ModePeer> players = Map.of();
    public void reset() { epoch = sequence = receivedAt = 0; players = Map.of(); }
    public long sequence() { return sequence; }
    public boolean accept(CoopModePayloads.ModeRoster roster, long expectedEpoch, long now) {
        if (expectedEpoch <= 0 || roster.epoch() != expectedEpoch || roster.version() != CoopModePayloads.VERSION
                || (epoch == expectedEpoch && roster.sequence() <= sequence)) return false;
        Map<UUID, CoopModePayloads.ModePeer> next = new HashMap<>();
        java.util.Set<Long> identities = new java.util.HashSet<>();
        for (var p : roster.players()) {
            if (next.put(p.playerId(), p) != null || (p.steamId64() != 0 && !identities.add(p.steamId64()))) return false;
        }
        epoch = expectedEpoch; sequence = roster.sequence(); receivedAt = now; players = Map.copyOf(next);
        return true;
    }
    public CoopModePayloads.ModePeer peer(UUID id, long expectedEpoch, long now) {
        return fresh(expectedEpoch, now) ? players.get(id) : null;
    }
    public CoopModePayloads.ModePeer remote(UUID localId, long expectedEpoch, long now) {
        if (!fresh(expectedEpoch, now)) return null;
        CoopModePayloads.ModePeer remote = null;
        for (var p : players.values()) if (!p.playerId().equals(localId)) {
            if (remote != null) return null;
            remote = p;
        }
        return remote;
    }
    private boolean fresh(long expectedEpoch, long now) {
        return epoch != 0 && epoch == expectedEpoch && now >= receivedAt && now - receivedAt <= CoopProtocol.FRESH_NANOS;
    }
    public static boolean hideMinecraft(CoopModePayloads.ModePeer peer, NativePeerInfo nativePeer) {
        // Wait for the native body to be visible before hiding the Minecraft replacement.
        return peer != null && peer.mode() == CoopModePayloads.ELDEN_RING
            && nativePeer.matchesRemote(peer.steamId64()) && !nativePeer.remoteHidden();
    }
}
