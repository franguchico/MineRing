package dev.ermc.bridge.entity;

/** Keep the last complete entity snapshot through a brief concurrent publication. */
final class EntitySnapshotGate {
	static final long GRACE_NANOS = 500_000_000L;
	enum Result { REPLACE, RETAIN, CLEAR }
	private boolean hasSnapshot;
	private long lastSuccess;

	void reset() {
		hasSnapshot = false;
	}

	Result read(boolean contextValid, boolean complete, long now) {
		if (!contextValid) {
			reset();
			return Result.CLEAR;
		}
		if (complete) {
			hasSnapshot = true;
			lastSuccess = now;
			return Result.REPLACE;
		}
		long age = now - lastSuccess;
		if (hasSnapshot && age >= 0 && age < GRACE_NANOS) return Result.RETAIN;
		reset();
		return Result.CLEAR;
	}
}
