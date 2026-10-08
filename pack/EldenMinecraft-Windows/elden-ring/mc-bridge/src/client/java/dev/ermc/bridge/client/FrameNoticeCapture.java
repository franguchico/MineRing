package dev.ermc.bridge.client;

/** Notice lifetime starts at rendering, never at a delayed readback/publication. */
record FrameNoticeCapture(int ttlMillis, long capturedAt, CompositeFrameGate.Context context) {
    static final int MAX_TTL_MILLIS = 8000;

    int remainingMillis(CompositeFrameGate.Context current, long now) {
        if (ttlMillis <= 0 || ttlMillis > MAX_TTL_MILLIS || context == null || !context.passive()
                || !CompositeFrameGate.same(context, current) || now < capturedAt) return 0;
        long remaining = ttlMillis * 1_000_000L - (now - capturedAt);
        // Round down: a sub-millisecond remainder must not advertise another millisecond.
        return remaining <= 0 ? 0 : (int) (remaining / 1_000_000L);
    }
}
