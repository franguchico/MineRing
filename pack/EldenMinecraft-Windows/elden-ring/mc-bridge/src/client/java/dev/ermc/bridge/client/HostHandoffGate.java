package dev.ermc.bridge.client;

/** A death or subsequent placement must finish under the host game's controls. */
public final class HostHandoffGate {
	private boolean haveLife;
	private int lastLife;

	public boolean update(boolean connected, boolean haveState, int life, boolean dead, boolean driving) {
		if (!connected) {
			haveLife = false;
			return false;
		}
		if (!haveState) {
			return false;
		}
		boolean placedAgain = haveLife && life != lastLife;
		lastLife = life;
		haveLife = true;
		return driving && (dead || placedAgain);
	}
}
