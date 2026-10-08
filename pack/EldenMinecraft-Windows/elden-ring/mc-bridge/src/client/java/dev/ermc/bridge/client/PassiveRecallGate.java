package dev.ermc.bridge.client;

import net.minecraft.world.phys.Vec3;

/** Follow native travel through the existing authenticated server recall, never a client teleport. */
final class PassiveRecallGate {
    private long lastRequest;
    void reset() { lastRequest = 0; }
    boolean request(boolean placementReady, Vec3 nativeFeet, Vec3 mcFeet, long now) {
        if (!placementReady || nativeFeet == null || mcFeet == null || !finite(nativeFeet) || !finite(mcFeet)
                || nativeFeet.distanceToSqr(mcFeet) <= 16 * 16
                || (lastRequest != 0 && (now < lastRequest || now - lastRequest < 1_000_000_000L))) return false;
        lastRequest = now;
        return true;
    }
    private static boolean finite(Vec3 p) { return Double.isFinite(p.x) && Double.isFinite(p.y) && Double.isFinite(p.z); }
}
