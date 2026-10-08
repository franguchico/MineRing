package dev.ermc.bridge.client;

import java.util.Locale;

/** Render-thread counters only. A summary never waits, opens a file or runs per publication. */
final class FrameCaptureDiagnostics {
    static final long SUMMARY_NS = 30_000_000_000L;
    long successes, fenceSkips, expired, rejected, nullMaps, unmapFailures, syncFailures, mapErrors;
    long worstMapNs, worstCopyNs;
    private boolean started;
    private long windowAt;

    void reject(CompositeFrameGate.Ticket ticket, long now) {
        if (ticket != null && now >= ticket.capturedAt()
                && now - ticket.capturedAt() > CompositeFrameGate.MAX_FRAME_AGE_NS) expired++;
        else rejected++;
    }

    String summary(long now) {
        if (!started) { started = true; windowAt = now; return null; }
        if (now < windowAt || now - windowAt < SUMMARY_NS) return null;
        windowAt = now;
        if (successes + fenceSkips + expired + rejected + nullMaps + unmapFailures + syncFailures + mapErrors == 0) return null;
        String text = String.format(Locale.ROOT,
            "frame-readback (30s): published %d, fence-skips %d, expired %d, rejected %d, null-map %d, unmap-failed %d, sync-failed %d, map-errors %d, worst map %.3f ms, copy %.3f ms",
            successes, fenceSkips, expired, rejected, nullMaps, unmapFailures, syncFailures, mapErrors,
            worstMapNs / 1_000_000.0, worstCopyNs / 1_000_000.0);
        successes = fenceSkips = expired = rejected = nullMaps = unmapFailures = syncFailures = mapErrors = 0;
        worstMapNs = worstCopyNs = 0;
        return text;
    }
}
