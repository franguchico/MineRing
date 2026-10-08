package dev.ermc.bridge;

import dev.ermc.bridge.link.Protocol;

/** Shared, pure control policy: flight flags only describe a body Minecraft actually drives. */
public final class FlightControl {
	private FlightControl() {
	}

	public static int bodyFlags(boolean standIn, boolean spectator, boolean creativeFlight, boolean gliding, boolean onGround) {
		if (!standIn || spectator) return 0;
		return Protocol.CTRL_MOVE_HUNTER | Protocol.CTRL_HIDE_HUNTER
			| (!onGround && (creativeFlight || gliding) ? Protocol.CTRL_FREE_FLIGHT : 0);
	}
}
