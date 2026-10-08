package dev.ermc.bridge.entity;

/** Native model IDs are diagnostics, not names suitable for kill messages. */
public final class EntityDisplayName {
    private EntityDisplayName() {}
    private static final java.util.regex.Pattern MODEL_ID =
        java.util.regex.Pattern.compile("c[0-9]{4,}(?:[_-][0-9]+)*", java.util.regex.Pattern.CASE_INSENSITIVE);
    public static boolean needsFallback(String raw) {
        if (raw == null || raw.isBlank()) return true;
        String value = raw.trim();
        return value.equalsIgnoreCase("[unknown]") || value.equalsIgnoreCase("unknown")
            || MODEL_ID.matcher(value).matches();
    }
}
