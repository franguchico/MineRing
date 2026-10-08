package dev.ermc.bridge;

import java.util.function.IntConsumer;
import java.util.function.IntUnaryOperator;

/** Resumable column transaction: install replacement collision before clearing older collision. */
final class TerrainColumnWork {
	private int placeY, clearY;
	private final int bottom, clearEnd, keepBottom, keepTop;
	TerrainColumnWork(int bottom, int top, int clearFirst, int clearEnd) {
		this(bottom,top,clearFirst,clearEnd,bottom,top);
	}
	TerrainColumnWork(int bottom, int top, int clearFirst, int clearEnd, int keepBottom, int keepTop) {
		this.bottom=bottom; placeY=top; clearY=clearFirst; this.clearEnd=clearEnd;
		this.keepBottom=keepBottom; this.keepTop=keepTop;
	}
	boolean drain(TerrainWorkBudget budget, IntConsumer place, IntUnaryOperator clear) {
		while (placeY != Integer.MIN_VALUE && placeY >= bottom) {
			if (!budget.block()) return false;
			place.accept(placeY--);
		}
		while (clearY <= clearEnd) {
			if (!budget.block()) return false;
			if (keepTop != Integer.MIN_VALUE && clearY >= keepBottom && clearY <= keepTop) clearY = keepTop + 1;
			else clearY = Math.max(clearY + 1, clear.applyAsInt(clearY));
		}
		return true;
	}
}
