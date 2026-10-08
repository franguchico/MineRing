package dev.ermc.bridge.client;

/** Pure, monotonic notice lifetime. It supplies no camera/world/publication proof. */
public final class RecoveryNoticePolicy {
    public static final int MAX_TTL_MS = 8000;
    public static final String RETRY = "Controle no ER · F8 tenta Minecraft novamente";
    private static final long TTL_NS = MAX_TTL_MS * 1_000_000L;

    /** Reference identity, so a reconnect, world replacement or respawn revokes the notice. */
    public record Identity(Object connection, Object level, Object player) {
        boolean usable() { return connection != null && level != null && player != null; }
        boolean same(Identity other) {
            return other != null && connection == other.connection && level == other.level && player == other.player;
        }
    }

    private Identity identity;
    private String reason;
    private long shownAtNs;

    /** Duplicate callbacks for the same reason/session do not renew even an expired notice. */
    public boolean show(Identity current, String message, long nowNs) {
        if (current == null || !current.usable()) { clear(); return false; }
        String text = shortReason(message);
        if (current.same(identity) && text.equals(reason)) return false;
        identity = current;
        reason = text;
        shownAtNs = nowNs;
        return true;
    }

    public int remainingTtlMs(Identity current, long nowNs) {
        if (reason == null) return 0;
        if (current == null || !current.usable() || !current.same(identity)) { clear(); return 0; }
        long elapsed = nowNs - shownAtNs; // System.nanoTime may have a negative origin and wrap.
        if (elapsed < 0 || elapsed >= TTL_NS) return 0;
        return (int) ((TTL_NS - elapsed + 999_999L) / 1_000_000L);
    }

    public String reason() { return reason; }

    public void clear() { identity = null; reason = null; shownAtNs = 0; }

    private static String shortReason(String message) {
        if (message == null || message.isBlank()) return "Captura do Minecraft indisponível";
        StringBuilder text = new StringBuilder();
        boolean space = false, formatCode = false;
        for (int offset = 0, count = 0; offset < message.length() && count < 160; count++) {
            int cp = message.codePointAt(offset);
            offset += Character.charCount(cp);
            if (formatCode) { formatCode = false; continue; }
            if (cp == 0xA7) { formatCode = true; continue; }
            if (Character.getType(cp) == Character.FORMAT) continue;
            if (Character.isWhitespace(cp) || Character.isISOControl(cp)) { space = text.length() > 0; continue; }
            if (space) text.append(' ');
            text.appendCodePoint(cp);
            space = false;
        }
        return text.isEmpty() ? "Captura do Minecraft indisponível" : text.toString();
    }
}
