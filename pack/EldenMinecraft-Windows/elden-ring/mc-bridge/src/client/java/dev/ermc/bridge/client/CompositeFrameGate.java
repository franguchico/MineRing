package dev.ermc.bridge.client;

import dev.ermc.bridge.CoordMap;

/** A PBO may publish only in the exact session/view mode in which it was captured. */
final class CompositeFrameGate {
    record Context(Object connection, Object world, Object player, CoordMap.Mapping map, int life, boolean passive) {}
    record Ticket(long generation, long capturedAt) {}
    private Context context;
    private long generation;
    static final long MAX_FRAME_AGE_NS = 150_000_000L;

    boolean update(Context next) {
        if (same(context, next)) return false;
        context = next;
        generation++;
        return true;
    }
    Ticket capture(long now) { return context == null ? null : new Ticket(generation, now); }
    boolean accepts(Ticket ticket, long now) {
        return context != null && ticket != null && ticket.generation() == generation
            && now >= ticket.capturedAt() && now - ticket.capturedAt() <= MAX_FRAME_AGE_NS;
    }
    boolean accepts(Ticket ticket, Context current, long now) {
        return same(context, current) && accepts(ticket, now);
    }
    void invalidate() { context = null; generation++; }
    static boolean same(Context a, Context b) {
        return a == b || (a != null && b != null && a.connection() == b.connection()
            && a.world() == b.world() && a.player() == b.player() && a.life() == b.life()
            && a.passive() == b.passive() && java.util.Objects.equals(a.map(), b.map()));
    }
}
