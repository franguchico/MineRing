package dev.ermc.bridge;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.function.LongConsumer;

/** Bounded cross-thread door/strike requests; large areas resume instead of walking a square in one tick. */
final class TerrainRefreshQueue {
	static final int CAPACITY = 64;
	static final int MAX_RADIUS = 32;
	private static final class Region {
		final int x, z, radius;
		final long due;
		int cursor;
		Region(int x, int z, int radius, long due) { this.x=x; this.z=z; this.radius=radius; this.due=due; }
	}
	private final ArrayList<Region> regions = new ArrayList<>();
	private long dropped;
	synchronized void add(int x, int z, int radius, long due) {
		radius = Math.max(0, Math.min(MAX_RADIUS, radius));
		for (Region r : regions) {
			if (r.cursor == 0 && r.x == x && r.z == z && r.radius == radius && r.due / 500 == due / 500) return;
		}
		if (regions.size() == CAPACITY) {
			// Keep the earliest work, including the region already being drained.
			Region last = regions.get(regions.size()-1);
			dropped++;
			if (last.cursor != 0 || last.due <= due) return;
			regions.remove(regions.size()-1);
		}
		regions.add(new Region(x,z,radius,due));
		regions.sort(Comparator.comparingLong(r -> r.due));
	}
	synchronized int drain(long now, TerrainWorkBudget budget, LongConsumer invalidate) {
		int visits = 0;
		while (visits < 64 && !regions.isEmpty() && regions.get(0).due <= now && budget.scan()) {
			Region r = regions.get(0);
			int side = r.radius * 2 + 1;
			int x = r.x - r.radius + r.cursor / side;
			int z = r.z - r.radius + r.cursor % side;
			invalidate.accept((x & 0xffffffffL) | ((z & 0xffffffffL) << 32));
			visits++;
			if (++r.cursor == side * side) regions.remove(0);
		}
		return visits;
	}
	synchronized int size() { return regions.size(); }
	synchronized long dropped() { return dropped; }
	synchronized void clear() { regions.clear(); dropped=0; }
}
