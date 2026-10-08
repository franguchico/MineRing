package dev.ermc.bridge.coop;

/** Only the selected, ready Minecraft body simulates hazards. No native HP is changed. */
public final class CoopBodyPolicy {
    public static final long RESUME_FALL_GRACE_NANOS = 250_000_000L;
    private boolean initialized, suspended;
    private long resumedAt;
    private boolean resumed;

    public boolean update(boolean minecraftMode, boolean worldReady, long now) {
        boolean next = !minecraftMode || !worldReady;
        if (initialized && suspended && !next) { resumedAt = now; resumed = true; }
        if (next) resumed = false;
        initialized = true;
        suspended = next;
        return suspended;
    }

    public boolean blocksDamage(boolean pairedDeath, boolean explicitKill, boolean fall, long now) {
        // /kill remains a recovery command, and an actual paired ER death remains authoritative.
        if (pairedDeath || explicitKill) return false;
        if (suspended) return true;
        long elapsed = now - resumedAt;
        return fall && resumed && elapsed >= 0 && elapsed < RESUME_FALL_GRACE_NANOS;
    }
}
