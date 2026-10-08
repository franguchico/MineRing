package dev.ermc.bridge;

import java.util.Objects;

/** Pure ray geometry and batch provenance; no Minecraft or native runtime is needed. */
final class TerrainSampling {
	private TerrainSampling() {
	}

	static final double RAY_ABOVE = 2.2;
	static final double RAY_BELOW = 40.0;
	static final double FLIGHT_RAY_BELOW = 256.0;
	static final double FLIGHT_RAY_ABOVE = 0.25;
	static final double RAY_HIGH = 40.0;
	static final double PROBE_HEIGHT = 1.0;
	static final int RAYS_PER_COLUMN = 6;
	/** Bounded mailbox; native frame time is checked after every cast, so completion time varies. */
	static final int BATCH_RAYS = 96;
	static final long RAY_WARN_MS = 5000;
	static final double RESAMPLE_DY = 2.0;
	static final int FIRST_OBSTACLE_RAY = 2;
	static final int PASSAGE_FIELDS = 7;

	@FunctionalInterface
	interface Mapper {
		double[] toHost(double x, double y, double z);
	}

	/** The immutable mapping and lifecycle at submission must still match at consumption. */
	record BatchContext<M>(M mapping, int hostLife, int recallGeneration) {
		boolean matches(M currentMapping, int currentLife, int currentRecall) {
			return Objects.equals(mapping, currentMapping) && hostLife == currentLife && recallGeneration == currentRecall;
		}
	}

	enum Ground {
		UNANSWERED, SURFACE, VOID
	}

	enum Query {
		WAITING, DELAYED, READY
	}

	/** Finite floor contact, independent of ray completion or an overhead/remote landing hit. */
	static boolean supportsFeet(double surfaceY, double feetY) {
		return Double.isFinite(surfaceY) && Double.isFinite(feetY) && feetY >= surfaceY - 0.1 && feetY <= surfaceY + 1.25;
	}
	static Ground footprintGround(boolean anySurface, boolean allAnswered) {
		return anySurface ? Ground.SURFACE : allAnswered ? Ground.VOID : Ground.UNANSWERED;
	}
	/** Sub-centimetre native pose noise must not repeatedly throw away a door's ray queue. */
	static boolean samePassages(double[] a, double[] b) {
		if (a.length!=b.length) return false;
		for (int i=0;i<a.length;i++) if (!Double.isFinite(a[i]) || !Double.isFinite(b[i]) || Math.abs(a[i]-b[i])>0.02) return false;
		return true;
	}

	/** A timeout reports a slow queue; it never turns an unanswered ray into a miss. */
	static Query queryState(boolean answered, long elapsedMs) {
		return answered ? Query.READY : elapsedMs >= RAY_WARN_MS ? Query.DELAYED : Query.WAITING;
	}

	record Point(double x, double y, double z) {
	}
	/** Native collision corrections often repeat every frame. Reuse actual support for small corrections. */
	static boolean correctionNeedsPrime(Point previous, Point current, boolean measuredSupport) {
		if (measuredSupport) return false;
		if (previous==null) return true;
		double x=current.x-previous.x, y=current.y-previous.y, z=current.z-previous.z;
		return x*x+y*y+z*z>0.75*0.75;
	}

	/** An unanswered query is not a void. Only an actual no-ground reply releases a void fall. */
	static final class GroundGuard {
		private Point safe;
		private boolean confirmedVoid;
		private long supportedAt = Long.MIN_VALUE;

		GroundGuard(Point safe) {
			this.safe = Objects.requireNonNull(safe);
		}

		Point update(Point current, Ground ground, boolean onGround, boolean ownSupport) {
			return update(current, ground, onGround, ownSupport, false, false, 0);
		}

		Point update(Point current, Ground ground, boolean onGround, boolean ownSupport,
				 boolean flying, boolean canLaunch, long tick) {
			// Keep a local air checkpoint throughout flight. If flight ends before terrain answers,
			// hold here for landing rather than teleporting back to a distant launch platform.
			if (!onGround && (flying || (canLaunch && supportedAt != Long.MIN_VALUE && tick - supportedAt <= 12))) {
				safe = current;
				confirmedVoid = false;
				return null;
			}
			if (onGround && (ownSupport || ground == Ground.SURFACE)) supportedAt = tick;
			if (onGround) confirmedVoid = false;
			if (ownSupport) {
				confirmedVoid = false;
				if (onGround) safe = current;
				return null;
			}
			if (ground == Ground.VOID) {
				confirmedVoid = true;
				return null;
			}
			if (ground == Ground.SURFACE) {
				if (onGround) {
					safe = current;
					confirmedVoid = false;
				}
				return null;
			}
			return confirmedVoid ? null : safe;
		}
	}

	/** Server player velocity is often packet-driven: estimate it from one observation per tick. */
	static final class Motion {
		private Point previous;
		private long previousTick;
		private Point velocity = new Point(0, 0, 0);

		void reset(Point current, long tick) {
			previous = current;
			previousTick = tick;
			velocity = new Point(0, 0, 0);
		}

		void observe(Point current, long tick) {
			if (previous == null || tick - previousTick > 20 || tick < previousTick) {
				reset(current, tick);
				return;
			}
			long dt = tick - previousTick;
			if (dt == 0) return; // START/END callbacks cannot double the measured speed
			double x = (current.x - previous.x) / dt, y = (current.y - previous.y) / dt, z = (current.z - previous.z) / dt;
			velocity = x * x + y * y + z * z <= 64 ? new Point(x, y, z) : new Point(0, 0, 0);
			previous = current;
			previousTick = tick;
		}

		Point[] ahead(Point current) {
			// Up to 0.9 seconds of observed travel; cap reach to avoid probing across teleports.
			double length = Math.sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
			if (length < 0.05) return new Point[0];
			double scale = Math.min(1, 48 / (length * 18));
			Point[] points = new Point[3];
			for (int i = 0; i < points.length; i++) {
				double ticks = (i + 1) * 6 * scale;
				points[i] = new Point(current.x + velocity.x * ticks, current.y + velocity.y * ticks, current.z + velocity.z * ticks);
			}
			return points;
		}
	}

	static boolean outsideSurvey(int top, int bottom, double scanBottom, double scanTop) {
		return top != Integer.MIN_VALUE && (top + 1.0 < scanBottom || bottom > scanTop);
	}

	static int groundRay(boolean lowHit, boolean highHit, double highNormalY, boolean flight) {
		// A walker's high ray can find an uphill step. For a glider it may be a ceiling
		// overhead, which must never turn the empty air below into a solid cliff column.
		return lowHit ? 0 : !flight && highHit && highNormalY > 0.3 ? 1 : -1;
	}

	/** Keep a measured landing surface valid during descent; resample when climbing above it. */
	static boolean sampleCurrent(double sampleY, double surfaceY, double feetY) {
		return Math.abs(sampleY - feetY) < RESAMPLE_DY
			|| (Double.isFinite(surfaceY) && feetY <= sampleY && feetY >= surfaceY - 0.1);
	}

	static boolean sampleCurrent(double sampleY, double surfaceY, double feetY, boolean sampledFlight, boolean flight) {
		return sampledFlight == flight && sampleCurrent(sampleY, surfaceY, feetY);
	}
	/** A real landing surface can serve either mode; air/wall surveys remain mode-specific. */
	static boolean sampleCurrent(double sampleY, double surfaceY, double supportY, double feetY, boolean sampledFlight, boolean flight) {
		boolean floor=Double.isFinite(supportY) && Math.abs(supportY-surfaceY)<0.01;
		if (floor && feetY<supportY-0.1) return false;
		return (sampledFlight==flight || floor) && sampleCurrent(sampleY,surfaceY,feetY);
	}

	/** Every column touched by the player's feet, including both sides of a block boundary. */
	static int[] footprint(double x, double z, double width) {
		double half = width * 0.5;
		return new int[] {(int) Math.floor(x - half + 1e-7), (int) Math.floor(x + half - 1e-7),
			(int) Math.floor(z - half + 1e-7), (int) Math.floor(z + half - 1e-7)};
	}

	/** Two ground rays, two central cross rays, and two diagonals for off-centre door leaves. */
	static int writeColumn(float[] rays, int first, double x, double feetY, double z, Mapper map) {
		return writeColumn(rays, first, x, feetY, z, RAY_ABOVE, RAY_BELOW, map);
	}

	static int writeColumn(float[] rays, int first, double x, double feetY, double z, double above, double below, Mapper map) {
		Objects.checkFromIndexSize(first * 6, RAYS_PER_COLUMN * 6, rays.length);
		int n = first;
		put(rays, n++, map, x + 0.5, feetY + above, z + 0.5, x + 0.5, feetY - below, z + 0.5);
		put(rays, n++, map, x + 0.5, feetY + RAY_HIGH, z + 0.5, x + 0.5, feetY + above, z + 0.5);
		double y = feetY + PROBE_HEIGHT;
		put(rays, n++, map, x, y, z + 0.5, x + 1, y, z + 0.5);
		put(rays, n++, map, x + 0.5, y, z, x + 0.5, y, z + 1);
		put(rays, n++, map, x, y, z, x + 1, y, z + 1);
		put(rays, n++, map, x, y, z + 1, x + 1, y, z);
		return n;
	}

	private static void put(float[] rays, int i, Mapper map, double x0, double y0, double z0, double x1, double y1, double z1) {
		double[] s = map.toHost(x0, y0, z0);
		double[] e = map.toHost(x1, y1, z1);
		for (int k = 0; k < 3; k++) {
			rays[i * 6 + k] = (float) s[k];
			rays[i * 6 + 3 + k] = (float) e[k];
		}
	}

	/** Open corridors suppress obstacle columns only at their own floor and orientation. */
	static boolean inPassage(double[] passages, double x, double z, double groundY) {
		for (int i = 0; i + PASSAGE_FIELDS <= passages.length; i += PASSAGE_FIELDS) {
			if (Math.abs(groundY - passages[i + 2]) > 1.5) {
				continue;
			}
			double dx = x - passages[i], dz = z - passages[i + 1];
			double along = dx * passages[i + 3] + dz * passages[i + 4];
			double across = dx * passages[i + 4] - dz * passages[i + 3];
			if (Math.abs(along) <= passages[i + 6] && Math.abs(across) <= passages[i + 5]) {
				return true;
			}
		}
		return false;
	}
}
