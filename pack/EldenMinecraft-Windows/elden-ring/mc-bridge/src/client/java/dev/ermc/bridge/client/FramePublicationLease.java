package dev.ermc.bridge.client;

/** Publication age and continuous lack of progress, independent of GL polling frequency. */
final class FramePublicationLease {
    static final long MAX_UNUSABLE_NS = 750_000_000L;
    private CompositeFrameGate.Context context;
    private boolean watching, proof, recoveryIssued;
    private long progressAt, capturedAt;

    void expect(CompositeFrameGate.Context next, long now) {
        if (!watching || !CompositeFrameGate.same(context, next)) {
            context = next;
            progressAt = now;
            watching = true;
            proof = recoveryIssued = false;
        }
    }

    void published(long captureTime, long now) {
        capturedAt = captureTime;
        progressAt = now;
        proof = true;
        recoveryIssued = false;
    }

    boolean fresh(long now) {
        return proof && now >= capturedAt && now - capturedAt <= CompositeFrameGate.MAX_FRAME_AGE_NS;
    }

    boolean expireProof(long now) {
        if (!proof || fresh(now)) return false;
        proof = false;
        return true;
    }

    void clearProof() { proof = false; }

    boolean takeRecovery(long now) {
        if (!watching || recoveryIssued || now < progressAt || now - progressAt < MAX_UNUSABLE_NS) return false;
        proof = false;
        recoveryIssued = true;
        return true;
    }

    void reset() { context = null; watching = proof = recoveryIssued = false; }
}
