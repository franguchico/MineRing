package dev.ermc.bridge.coop;

/** Per authenticated connection ledger; the transport owns identity, never the payload. */
public final class PeerChannel {
    public static final int PASSIVE_DEATH = 0, EXPLICIT_RESET = 1;
    public final long epoch;
    private PeerState state;
    private long clientRecall, recalledClient, recall = 1, recalled;
    private long deaths, deathAck, deathSample;
    private int deathLife, deathKind, explicitRoutedLife = -1;
    public PeerChannel(long epoch) {
        if (epoch <= 0) throw new IllegalArgumentException("epoch");
        this.epoch = epoch;
    }
    public boolean accept(PeerState next, long request, long ack) {
        if (!CoopProtocol.valid(next) || next.connectionEpoch() != epoch
            || !CoopProtocol.counter(request) || !CoopProtocol.counter(ack) || ack > deaths
            || request < clientRecall || ack < deathAck) return false;
        if (state != null && (next.sequence() <= state.sequence()
            || next.hostLife() < state.hostLife() || next.hostDeaths() < state.hostDeaths())) return false;
        // Counter reset is permitted at a new host life; never apply it to another connection.
        if (state != null && next.hostLife() == state.hostLife()) {
            float[] old = state.hunterEvents(), now = next.hunterEvents();
            if (now[0] < old[0] || now[1] < old[1] || now[8] < old[8]) return false;
        }
        if (request != clientRecall || (state != null && (next.hostLife() != state.hostLife()
            || next.stageId() != state.stageId()))) recall++;
        clientRecall = request;
        deathAck = ack;
        state = next;
        return true;
    }
    public PeerState fresh(long now) {
        return state != null && now >= state.receivedAtNanos() && now - state.receivedAtNanos() <= CoopProtocol.FRESH_NANOS ? state : null;
    }
    public long sequence() { return state == null ? 0 : state.sequence(); }
    public void requestRecall() { recall++; }
    public boolean recallPending() { return recalled != recall; }
    public void recalled() { recalled = recall; recalledClient = clientRecall; }
    public long recalledGeneration() { return recalled; }
    public long recalledClient() { return recalledClient; }
    /** The old passive API retains its MC-mode caller's eligibility checks. */
    public void routeDeath() { queueDeath(PASSIVE_DEATH); }
    /** An explicit command may target the native body while MC is suspended. */
    public boolean routeExplicitReset(long now) {
        PeerState current = fresh(now);
        if (current == null || (current.flags() & 2) == 0 || (current.flags() & 256) != 0) return false;
        return queueDeath(EXPLICIT_RESET);
    }
    private boolean queueDeath(int kind) {
        if (state == null || deaths != deathAck
            || (kind == EXPLICIT_RESET && explicitRoutedLife == state.hostLife())
            || !CoopProtocol.counter(deaths + 1)) return false;
        deaths++; deathLife = state.hostLife(); deathKind = kind; deathSample = state.sequence();
        // A passive ACK means delivery to the client, not a confirmed native death.
        // F8 may discard that event; it must not consume an authorized reset's guard.
        if (kind == EXPLICIT_RESET) explicitRoutedLife = state.hostLife();
        return true;
    }
    public long deathSequence() { return deaths; }
    public int deathLife() { return deathLife; }
    public int deathKind() { return deathKind; }
    public long deathSample() { return deathSample; }
}
