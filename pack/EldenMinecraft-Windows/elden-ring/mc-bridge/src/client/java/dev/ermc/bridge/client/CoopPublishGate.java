package dev.ermc.bridge.client;

/** Client-thread gate around asynchronous server preparation, then client-thread publication. */
final class CoopPublishGate {
    record Attempt(Object server, Object connection, Object player, Object level) {}
    private Object server;
    private Attempt pending;
    private boolean attempted;

    Attempt begin(Object server, Object connection, Object player, Object level, boolean authority, boolean published) {
        if (server == null || connection == null || player == null || level == null || !authority) return null;
        if (this.server != server) { this.server = server; pending = null; attempted = false; }
        if (published) { pending = null; attempted = true; }
        if (attempted || pending != null) return null;
        return pending = new Attempt(server, connection, player, level);
    }

    /** Claims exactly one invocation, only while the original client/world is still connected. */
    boolean commit(Attempt request, Object server, Object connection, Object player, Object level,
            boolean authority, boolean published) {
        if (request != pending) return false;
        pending = null;
        if (request.server != server || request.connection != connection || request.player != player
            || request.level != level || !authority) return false;
        attempted = true;
        return !published;
    }

    boolean fail(Attempt request) {
        if (request != pending) return false;
        pending = null; attempted = true;
        return true;
    }

    // JOIN may invalidate unfinished preparation, but cannot retry a committed publication.
    void joined() { pending = null; }
    void disconnected() { pending = null; server = null; attempted = false; }
    String status() { return pending != null ? "preparing" : attempted ? "attempted" : "waiting"; }
}
