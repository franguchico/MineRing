package dev.ermc.bridge.client;

/** Placement and focus have separate deadlines. A refused return always stays retryable. */
final class InputHandoffGate {
    static final long PLACEMENT_TIMEOUT_NS = 8_000_000_000L;
    static final long FOCUS_TIMEOUT_NS = 2_000_000_000L;
    enum Action { WAIT, FOCUS, GRAB, MENU, RETURN }
    private boolean pending, requested;
    private long startedAt, focusedAt;
    void begin(long now) { pending = true; requested = false; startedAt = now; }
    void cancel() { pending = requested = false; }
    boolean pending() { return pending; }
    Action update(boolean blocked, boolean ready, boolean screenOpen, boolean windowActive, long now) {
        if (!pending) return Action.WAIT;
        if (blocked || now < startedAt || now - startedAt >= PLACEMENT_TIMEOUT_NS + FOCUS_TIMEOUT_NS) {
            cancel(); return Action.RETURN;
        }
        if (!ready) {
            // A focus callback received during recall cannot consume the readiness deadline.
            requested = false;
            if (now - startedAt >= PLACEMENT_TIMEOUT_NS) { cancel(); return Action.RETURN; }
            return Action.WAIT;
        }
        // A delivered focus callback wins even if a slow frame crossed the focus deadline.
        if (windowActive) return screenOpen ? Action.MENU : Action.GRAB;
        if (!requested) { requested = true; focusedAt = now; return Action.FOCUS; }
        if (now < focusedAt || now - focusedAt >= FOCUS_TIMEOUT_NS
                || now - startedAt >= PLACEMENT_TIMEOUT_NS + FOCUS_TIMEOUT_NS) {
            cancel(); return Action.RETURN;
        }
        return Action.WAIT;
    }
}
