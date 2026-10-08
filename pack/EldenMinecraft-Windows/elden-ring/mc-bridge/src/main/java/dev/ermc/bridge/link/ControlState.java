package dev.ermc.bridge.link;

/** What Minecraft asks the host game to do this frame (ErmcControl). */
public final class ControlState {
	public int flags;
	public long mcFrame;
	public final float[] camPos = new float[3];
	public final float[] camTarget = new float[3];
	public final float[] camUp = new float[3];
	public float fovYDeg;
	public final float[] hunterPos = new float[3];
	public int poseLag = 1;
	public int depthIndex;
	public float hunterYawDeg;

	public void copyFrom(ControlState source) {
		flags = source.flags; mcFrame = source.mcFrame; fovYDeg = source.fovYDeg;
		poseLag = source.poseLag; depthIndex = source.depthIndex; hunterYawDeg = source.hunterYawDeg;
		System.arraycopy(source.camPos, 0, camPos, 0, 3);
		System.arraycopy(source.camTarget, 0, camTarget, 0, 3);
		System.arraycopy(source.camUp, 0, camUp, 0, 3);
		System.arraycopy(source.hunterPos, 0, hunterPos, 0, 3);
	}
}
