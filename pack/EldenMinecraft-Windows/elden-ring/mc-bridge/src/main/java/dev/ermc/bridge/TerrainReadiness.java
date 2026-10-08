package dev.ermc.bridge;

/** Local terrain evidence; an air survey deliberately does not establish a walkable floor. */
public enum TerrainReadiness {
	PENDING, SUPPORTED, UNSUPPORTED, AIRBORNE, SWIMMING, CLIMBING;

	boolean permitsMovement() {
		return this==SUPPORTED || this==AIRBORNE || this==SWIMMING || this==CLIMBING;
	}
}
