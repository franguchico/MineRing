package dev.ermc.bridge.coop;

/** Authenticated connection's immutable local-ER sample. Never contains native handles. */
public record PeerState(long connectionEpoch, long sequence, int flags, int stageId,
		int hostLife, int hostDeaths, double hostX, double hostY, double hostZ,
		float[] hunterEvents, long receivedAtNanos) {
	public PeerState { hunterEvents = hunterEvents.clone(); }
	@Override public float[] hunterEvents() { return hunterEvents.clone(); }
}
