package dev.ermc.bridge.link;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * Process-wide handle on the bridge to the host game. Used from both the client render thread and the
 * integrated server thread, so all access is synchronized (every operation is cheap).
 */
public final class ErLink {
	private static final Logger LOG = LoggerFactory.getLogger("erbridge");
	private static final ErLink INSTANCE = new ErLink();
	/** The host game counts as connected if its frame counter moved within this window. */
	private static final long ALIVE_TIMEOUT_MS = 2000;

	private BridgeShm shm;
	private long lastOpenAttempt;
	private long lastHeartbeat = Long.MIN_VALUE;
	private long lastHeartbeatChange;
	private final GameState latest = new GameState();
	private final GameState scratch = new GameState();
	private boolean hasState;

	public static ErLink get() {
		return INSTANCE;
	}

	/** Refreshes the cached state. Returns true while the host game is running and publishing frames. */
	public synchronized boolean poll() {
		long now = System.currentTimeMillis();
		if (shm == null) {
			if (now - lastOpenAttempt < 1000) {
				return false;
			}
			lastOpenAttempt = now;
			try {
				shm = BridgeShm.open();
				LOG.info("Mapped {}", Protocol.SHM_PATH);
			} catch (Exception e) {
				LOG.warn("Could not map {}: {}", Protocol.SHM_PATH, e.toString());
				return false;
			}
		}
		long hb = shm.hostHeartbeat();
		if (lastHeartbeat == Long.MIN_VALUE) {
			// First sample: a stale counter from a previous session must not count as live.
			lastHeartbeat = hb;
			lastHeartbeatChange = 0;
		} else if (hb != lastHeartbeat) {
			lastHeartbeat = hb;
			lastHeartbeatChange = now;
		}
		boolean alive = lastHeartbeatChange > 0 && now - lastHeartbeatChange < ALIVE_TIMEOUT_MS;
		if (alive && shm.readState(scratch)) {
			latest.copyFrom(scratch);
			hasState = true;
		}
		return alive;
	}

	/** The host game's frame counter (one per presented frame) as of the last {@link #poll()}. */
	public synchronized long hostFrame() {
		return lastHeartbeat;
	}

	public synchronized boolean alive() {
		return shm != null && lastHeartbeatChange > 0 && System.currentTimeMillis() - lastHeartbeatChange < ALIVE_TIMEOUT_MS;
	}

	/** Copies the latest host-game state into {@code out}; returns false if none is available. */
	public synchronized boolean snapshot(GameState out) {
		if (!hasState) {
			return false;
		}
		out.copyFrom(latest);
		return true;
	}

	public synchronized void writeControl(ControlState c) {
		if (shm != null) {
			shm.writeControl(c);
		}
	}

	public synchronized NativePeerInfo peerInfo() {
		return shm != null && alive() ? shm.readPeerInfo() : NativePeerInfo.EMPTY;
	}

	public synchronized void writePeerMode(long steamId, int mode, boolean canRender, long epoch, long rosterSequence) {
		if (shm != null) shm.writePeerMode(steamId, mode, canRender, epoch, rosterSequence);
	}

	/** Returns the request sequence, or -1 if a batch is still in flight / the host game is not attached. */
	public synchronized int submitRays(float[] rays, int count, int flags, int[] filter) {
		if (shm == null || !alive() || !shm.raysIdle()) {
			return -1;
		}
		return shm.submitRays(rays, count, flags, filter);
	}

	public synchronized boolean raysDone(int seq) {
		return shm != null && shm.raysDone(seq);
	}

	public synchronized void readHits(int count, float[] out, int[] hit, int[] attr) {
		if (shm != null) {
			shm.readHits(count, out, hit, attr);
		}
	}

	public synchronized boolean readHunterEvents(float[] out) {
		return shm != null && shm.readHunterEvents(out);
	}

	public synchronized boolean readEntities(java.util.List<EntityInfo> out) {
		return shm != null && alive() && shm.readEntities(out);
	}

	/** Open doorways (see {@link BridgeShm#readPassages}); -1 when unavailable. */
	public synchronized int readPassages(float[] out, int[] zoneOut) {
		return shm != null && alive() ? shm.readPassages(out, zoneOut) : -1;
	}

	public synchronized boolean pushDamage(long id, float amount, float x, float y, float z, int flags) {
		return shm != null && shm.pushDamage(id, amount, x, y, z, flags);
	}

	public synchronized int mcSwitchRequests() {
		return shm != null ? shm.mcSwitchRequests() : 0;
	}

	public synchronized void requestHostFocus() {
		if (shm != null) {
			shm.requestHostFocus();
		}
	}

	/** Independent committed ownership event, published before old controls are cleared. */
	public synchronized void beginHostHandoff(boolean automatic) {
		if (shm != null) shm.beginHostHandoff(automatic);
	}

	public synchronized boolean bindExplicitReset(long connection) {
		return shm != null && alive() && shm.bindExplicitReset(connection);
	}

	public synchronized boolean explicitResetBound(long connection) {
		return shm != null && shm.explicitResetBound(connection);
	}

	public synchronized boolean writeExplicitReset(long connection, long event, int life) {
		return shm != null && alive() && shm.writeExplicitReset(connection, event, life);
	}

	public synchronized boolean explicitResetAcknowledged(long connection, long event) {
		return shm != null && shm.explicitResetAcknowledged(connection, event);
	}

	public synchronized void clearExplicitReset() {
		if (shm != null) shm.clearExplicitReset();
	}

	/** See {@link Protocol#H_HOST_LIFE}; 0 if not attached. */
	public synchronized int hostLife() {
		return shm != null ? shm.hostLife() : 0;
	}

	/** See {@link Protocol#H_HOST_DEATHS}; 0 if not attached. */
	public synchronized int hostDeaths() {
		return shm != null ? shm.hostDeaths() : 0;
	}

	/** Asks Elden Ring to perform its current action; returns the request number (0 if not attached). */
	public synchronized int requestAction() {
		return shm != null ? shm.requestAction() : 0;
	}

	public synchronized int actionAck() {
		return shm != null ? shm.actionAck() : 0;
	}

	public synchronized int actionResult() {
		return shm != null ? shm.actionResult() : 0;
	}

	/** The action Elden Ring offers right now ("" if none). */
	public synchronized String hostPrompt() {
		return shm != null ? shm.hostPrompt() : "";
	}

	public synchronized void bumpMcDeaths() {
		if (shm != null) {
			shm.bumpMcDeaths();
		}
	}

	public synchronized void bumpMcHeartbeat() {
		if (shm != null) {
			shm.bumpMcHeartbeat();
		}
	}
}
