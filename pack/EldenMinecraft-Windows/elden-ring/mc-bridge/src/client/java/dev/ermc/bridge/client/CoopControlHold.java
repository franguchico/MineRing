package dev.ermc.bridge.client;

import java.util.Objects;

/** A short network pause may hold an acquired body, never acquire/move a new one. */
public final class CoopControlHold {
    public static final long MAX_HOLD_NANOS = 750_000_000L;
    private Object connection, player, mapping;
    private int life;
    private long recall;
    private boolean acquired;
    private boolean suspended;
    private long suspendedAt;

    public void reset() {
        acquired = false;
        suspended = false;
        connection = player = mapping = null;
    }

    public void acquire(Object connection, Object player, Object mapping, int life, long recall) {
        this.connection = connection;
        this.player = player;
        this.mapping = mapping;
        this.life = life;
        this.recall = recall;
        suspended = false;
        acquired = connection != null && player != null && mapping != null;
    }

    public boolean mayHold(boolean eligible, Object connection, Object player, Object mapping, int life, long recall, long now) {
        boolean keep = eligible && acquired && this.connection == connection && this.player == player
            && Objects.equals(this.mapping, mapping) && this.life == life && this.recall == recall;
        if (keep && !suspended) { suspended = true; suspendedAt = now; }
        keep = keep && now >= suspendedAt && now - suspendedAt < MAX_HOLD_NANOS;
        if (!keep) reset();
        return keep;
    }
}
