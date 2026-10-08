package dev.ermc.bridge;

import java.util.function.LongSupplier;

/** Shared cooperative tick limit. One Minecraft mutation may itself exceed the time limit. */
final class TerrainWorkBudget {
	static final int BLOCK_STEPS = 96;
	static final int SCAN_STEPS = 256;
	static final int APPLY_COLUMNS = 4;
	static final long TICK_NANOS = 2_000_000;
	private final LongSupplier clock;
	private final long deadline;
	private int blocks, scans;

	TerrainWorkBudget() { this(BLOCK_STEPS, SCAN_STEPS, TICK_NANOS, System::nanoTime); }
	TerrainWorkBudget(int blocks, int scans, long nanos, LongSupplier clock) {
		this.blocks = blocks; this.scans = scans; this.clock = clock;
		deadline = clock.getAsLong() + nanos;
	}
	boolean block() { if (blocks <= 0 || expired()) return false; blocks--; return true; }
	boolean scan() { if (scans <= 0 || expired()) return false; scans--; return true; }
	boolean expired() { return clock.getAsLong() - deadline >= 0; }
}
