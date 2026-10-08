package dev.ermc.bridge;

import java.util.ArrayList;

/** Precomputed nearest-first rings; no repeated traversal of each ring's filled square. */
final class TerrainScan {
	private TerrainScan() { }
	/** Resume a horizon scan across ticks; restart only when its centre changes. */
	static final class Cursor {
		private int x = Integer.MIN_VALUE, z = Integer.MIN_VALUE, next;
		void centre(int x, int z) { if (this.x != x || this.z != z) { this.x=x; this.z=z; next=0; } }
		int next(int size) { int result=next; next=(next+1)%size; return result; }
	}
	static int[][] offsets(int radius) {
		if (radius < 0 || radius > 64) throw new IllegalArgumentException("radius");
		var result = new ArrayList<int[]>();
		for (int r = 0; r <= radius; r++) {
			for (int x = -r; x <= r; x++) {
				for (int z = -r; z <= r; z++) {
					if (Math.max(Math.abs(x), Math.abs(z)) == r && x*x + z*z <= radius*radius)
						result.add(new int[] {x, z});
				}
			}
		}
		return result.toArray(int[][]::new);
	}
}
