package dev.ermc.bridge.coop;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/** Bounded server-thread ledger. Identity comes from the authenticated connection, never C2S data. */
public final class CoopModeLedger {
    public static final int VERSION = 1, MAX_PLAYERS = 2;
    public static final int UNKNOWN = 0, MINECRAFT = 1, ELDEN_RING = 2;
    public static final long FRESH_NANOS = 1_500_000_000L;
    public static final long MAX_SEQUENCE = 1L << 52;
    // Public-universe, individual-account, desktop-instance SteamID64; account zero is invalid.
    private static final long FIRST_STEAM_ID = 0x0110000100000001L;
    private static final long LAST_STEAM_ID = 0x01100001ffffffffL;

    public record Entry(UUID playerId, long steamId64, int mode, long sequence, long connectionEpoch) {}
    public record Connection(UUID playerId, long epoch, boolean nativeFresh) {}
    private static final class Channel {
        final long epoch;
        long sequence, receivedAt;
        Entry entry;
        Channel(long epoch) { this.epoch = epoch; }
    }
    private final Map<UUID, Channel> channels = new HashMap<>(MAX_PLAYERS);

    public static boolean steamId(long value) {
        return value == 0 || (value >= FIRST_STEAM_ID && value <= LAST_STEAM_ID);
    }
    public static boolean mode(int value) { return value >= UNKNOWN && value <= ELDEN_RING; }
    public static boolean sequence(long value) { return value > 0 && value < MAX_SEQUENCE; }
    public static boolean valid(int version, long epoch, long sequence, long steamId64, int mode) {
        return version == VERSION && epoch > 0 && sequence(sequence) && steamId(steamId64) && mode(mode);
    }

    /** A verified local Steam identity is supplied by the client's native bridge, or zero until known. */
    public boolean accept(UUID authenticatedId, long currentEpoch, boolean nativeFresh,
            int version, long epoch, long sequence, long steamId64, int mode, long now) {
        if (authenticatedId == null || !nativeFresh || currentEpoch != epoch
                || !valid(version, epoch, sequence, steamId64, mode)) return false;
        Channel channel = channels.get(authenticatedId);
        if (channel == null || channel.epoch != currentEpoch) {
            if (channel == null && channels.size() >= MAX_PLAYERS) return false;
            channel = new Channel(currentEpoch);
            channels.put(authenticatedId, channel);
        }
        if (sequence <= channel.sequence || (channel.sequence != 0 && now < channel.receivedAt)) return false;
        channel.sequence = sequence;
        channel.receivedAt = now;
        channel.entry = new Entry(authenticatedId, steamId64, mode, sequence, currentEpoch);
        return true;
    }

    /** Drop disconnected/replaced channels and unavailable native samples before publishing a roster. */
    public void retain(List<Connection> connections) {
        if (connections.size() > MAX_PLAYERS) throw new IllegalArgumentException("Too many mode connections");
        var iterator = channels.entrySet().iterator();
        while (iterator.hasNext()) {
            var saved = iterator.next();
            Connection current = null;
            for (Connection c : connections) {
                if (saved.getKey().equals(c.playerId()) && saved.getValue().epoch == c.epoch()) {
                    current = c;
                    break;
                }
            }
            if (current == null) iterator.remove();
            else if (!current.nativeFresh()) saved.getValue().entry = null;
        }
    }

    /** Expiration removes visibility data but retains the replay watermark for this connection. */
    public List<Entry> snapshot(long now) {
        var entries = new ArrayList<Entry>(MAX_PLAYERS);
        for (Channel c : channels.values()) {
            long age = now - c.receivedAt;
            if (age < 0 || age > FRESH_NANOS) c.entry = null;
            if (c.entry != null) entries.add(c.entry);
        }
        entries.sort((a, b) -> a.playerId().compareTo(b.playerId()));
        return List.copyOf(entries);
    }

    /** Allocation-free, epoch-bound authority for server physics and damage routing. */
    public int mode(UUID playerId, long epoch, long now) {
        Channel c = channels.get(playerId);
        if (c == null || c.epoch != epoch || c.entry == null) return UNKNOWN;
        long age = now - c.receivedAt;
        return age >= 0 && age <= FRESH_NANOS ? c.entry.mode() : UNKNOWN;
    }

    /** An old disconnect callback must never remove a newer connection for the same UUID. */
    public void disconnect(UUID playerId, long epoch) {
        Channel channel = channels.get(playerId);
        if (channel != null && channel.epoch == epoch) channels.remove(playerId);
    }
    public void reset() { channels.clear(); }
}
