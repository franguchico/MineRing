package dev.ermc.bridge.client;

import dev.ermc.bridge.CoordMap;
import dev.ermc.bridge.link.ControlState;
import dev.ermc.bridge.link.GameState;
import dev.ermc.bridge.link.ErLink;
import dev.ermc.bridge.link.Protocol;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.world.phys.Vec3;
import org.joml.Vector3f;
import org.joml.Quaternionf;

/**
 * Keeps the two cameras identical so Minecraft's layer lines up with the host game's frame.
 *
 * <ul>
 *   <li>FOLLOW_HOST: Minecraft renders from the host game's camera (it stays in charge). Needs
 *   only read access to its camera; used to verify alignment.</li>
 *   <li>DRIVE_HOST: the host game renders from Minecraft's camera, so the Minecraft player can
 *   walk around its world. Needs the DLL's camera override.</li>
 * </ul>
 */
public final class CameraSync {
	private CameraSync() {
	}

	public enum Mode {
		FOLLOW_HOST,
		DRIVE_HOST
	}

	public interface CameraAccess {
		void erbridge$setPosition(Vec3 pos);

		void erbridge$setRotation(float yaw, float pitch);

		void erbridge$setNativeRotation(float yaw, float pitch, Quaternionf rotation);
	}

	private static Mode mode = Mode.DRIVE_HOST;
	/** The hunter follows the Minecraft player (hidden) and takes the host game's hits for it. */
	private static boolean standIn = true;
	/** Debug knobs for the in-host-game compositor (see dev commands "pt ..."). */
	public static int poseLag = 1;
	public static int depthIndex = 0;
	public static boolean noDepthTest;
	public static boolean debugDepth;
	private static final GameState STATE = new GameState();
	private static final ControlState CONTROL = new ControlState();
	private static float fov = 70.0F;
	private static boolean controlling;
	/** Retain the reason through native focus acknowledgement and passive captures, until explicit F8. */
	private static boolean automaticRecovery;
	private static boolean wasSpectator;
	private static final CoopControlHold COOP_HOLD = new CoopControlHold();
	private static boolean holding;
	private static boolean passive;
	private static float passiveAspect;
	private static final PassiveRecallGate PASSIVE_RECALL = new PassiveRecallGate();
	public static boolean passive() { return passive; }
	public static float passiveAspect() { return passiveAspect; }
	public static boolean holding() { return holding; }

	public static Mode mode() {
		return mode;
	}

	public static void toggleMode() {
		mode = mode == Mode.FOLLOW_HOST ? Mode.DRIVE_HOST : Mode.FOLLOW_HOST;
	}

	/** True when the Minecraft player may drive the host game (right zone, already placed at the hunter). */
	static boolean placementReadyForInput(Minecraft mc, GameState state) {
		CoordMap.Mapping map = CoordMap.get();
		if (map == null || map.provisional() || !state.has(Protocol.STATE_PLAYER_VALID) || map.zone() != state.stageId) {
			return false;  // the server hasn't switched to this host-game zone yet
		}
		if (mc.player == null || !map.contains(mc.player.getX())) {
			return false;  // teleport into this zone's region not received yet
		}
		// Whenever the Tarnished is the truth (control back from Elden Ring, a respawn on either
		// side, a warp), keep our hands off its camera and body until the server has moved the
		// Minecraft player to it and the teleport has reached this client.
		return CoopClient.placementReady(state);
	}

	public static boolean toggleStandIn() {
		standIn = !standIn;
		return standIn;
	}
	public static boolean standInEnabled() { return standIn; }

	/** Field of view Minecraft must render with while the overlay is active. */
	public static float fov() {
		return fov;
	}

	/** True once the host game confirmed it rendered with our camera. */
	public static boolean hostFollowing() {
		return STATE.has(Protocol.STATE_CAM_OVERRIDDEN);
	}

	private static boolean driving;

	/** True while Minecraft's camera is driving the host game's this frame. */
	public static boolean driving() {
		return driving;
	}

	/** True while the host game draws our frames into its own image. */
	public static boolean hostCompositing() {
		return STATE.has(Protocol.STATE_COMPOSITING);
	}

	public static void copyCurrentPose(ControlState out) {
		out.copyFrom(CONTROL);
	}

	public static void afterCameraSetup(Camera camera) {
		Minecraft mc = Minecraft.getInstance();
		boolean spectator = mc.player != null && mc.player.isSpectator();
		if (wasSpectator && !spectator && (dev.ermc.bridge.TerrainManager.isBridgeWorld() || dev.ermc.bridge.coop.CoopSession.enabled())) {
			// A free camera can cross floors and doors. Returning to a physical player
			// must start at the host's feet, rather than transplanting that free camera.
			CoopClient.requestRecall();
		}
		wasSpectator = spectator;
		CoordMap.Mapping map = CoordMap.get();
		boolean haveState = ErLink.get().alive() && ErLink.get().snapshot(STATE);
		holding = false;
		passive = false;
		if (Overlay.hostMode()) {
			driving = false;
			COOP_HOLD.reset();
			// A transition can still expose the previous override for one native frame.
			NativeCameraView.View view = Overlay.passive() && haveState && ErLink.get().alive()
				&& !STATE.has(Protocol.STATE_CAM_OVERRIDDEN) && mc.player != null && !mc.player.isDeadOrDying()
				&& mc.getConnection() != null && mc.getConnection().getConnection().isConnected()
				? NativeCameraView.read(STATE, map) : null;
			if (view == null) {
				releaseHostCamera();
				fov = mc.options.fov().get();
				return;
			}
			CameraAccess access = (CameraAccess) camera;
			access.erbridge$setPosition(view.position());
			access.erbridge$setNativeRotation(view.yaw(), view.pitch(), view.rotation());
			fov = view.fov();
			passiveAspect = view.aspect();
			NativeCameraView.capturedControl(STATE, CONTROL);
			retainHandoffReason(CONTROL);
			passive = true;
			controlling = true; // release the compositor request on invalid view/disconnect too
			FramePassthrough.beginView(new CompositeFrameGate.Context(mc.getConnection(), mc.level,
				mc.player, map, ErLink.get().hostLife(), true));
			if (PASSIVE_RECALL.request(CoopClient.placementReady(STATE),
					map.toMc(STATE.playerPos[0], STATE.playerPos[1], STATE.playerPos[2]),
					mc.player.position(), System.nanoTime())) {
				CoopClient.requestRecall(); // keep chunks near the native camera using the same mapping/authority
			}
			if (!FramePassthrough.wanted()) releaseHostCamera();
			// Published only after its own world pixels are ready; never override native camera.
			return;
		}
		if (!Overlay.active() || Overlay.awaitingInput() || map == null || !haveState
				|| (mode == Mode.DRIVE_HOST && !placementReadyForInput(mc, STATE))) {
			// A missing network ACK is not an F8 handoff. Keep only a previously acquired
			// local body parked at the native bridge's last accepted position. No newly
			// predicted Minecraft position is allowed through while placement is unsafe.
			boolean eligible = dev.ermc.bridge.coop.CoopSession.enabled() && Overlay.active()
				&& !Overlay.hostMode() && !Overlay.awaitingInput() && mode == Mode.DRIVE_HOST && standIn && !spectator
				&& haveState && STATE.has(Protocol.STATE_PLAYER_VALID)
				&& !STATE.has(Protocol.STATE_PLAYER_DEAD) && !STATE.has(Protocol.STATE_HOST_BUSY)
				&& map != null && !map.provisional() && map.zone() == STATE.stageId
				&& mc.player != null && !mc.player.isDeadOrDying() && map.contains(mc.player.getX())
				&& mc.getConnection() != null && mc.getConnection().getConnection().isConnected();
			if (COOP_HOLD.mayHold(eligible, mc.getConnection(), mc.player, map,
					ErLink.get().hostLife(), CoopClient.recallGeneration(), System.nanoTime())) {
				boolean cachedFrame = FramePassthrough.holdLastPublishedPose(CONTROL);
				CONTROL.flags = Protocol.CTRL_HOLD_HUNTER | Protocol.CTRL_HIDE_HUNTER | Protocol.CTRL_OVERRIDE_CAMERA
					| (cachedFrame ? Protocol.CTRL_COMPOSITE : 0);
				// mcFrame identifies the cached pixels; incrementing it would break pose matching.
				holding = true;
				Overlay.suspendGameplayInput(mc);
				ErLink.get().writeControl(CONTROL);
			} else {
				if (dev.ermc.bridge.coop.CoopSession.enabled() && Overlay.active() && !Overlay.hostMode()
						&& !Overlay.awaitingInput() && mode == Mode.DRIVE_HOST)
					Overlay.recoverToHost(mc, CoopClient.recoveryReason(STATE));
				else releaseHostCamera();
			}
			fov = mc.options.fov().get();
			driving = false;
			return;
		}
		driving = mode == Mode.DRIVE_HOST;
		if (driving) FramePassthrough.beginView(new CompositeFrameGate.Context(mc.getConnection(), mc.level,
			mc.player, map, ErLink.get().hostLife(), false));

		if (mode == Mode.FOLLOW_HOST) {
			releaseHostCamera();
			if (!STATE.has(Protocol.STATE_CAMERA_VALID)) {
				fov = mc.options.fov().get();
				return;
			}
			Vec3 pos = map.toMc(STATE.camPos[0], STATE.camPos[1], STATE.camPos[2]);
			Vec3 dir = map.dirToMc(STATE.camTarget[0] - STATE.camPos[0], STATE.camTarget[1] - STATE.camPos[1],
				STATE.camTarget[2] - STATE.camPos[2]).normalize();
			float yaw = (float) Math.toDegrees(Math.atan2(-dir.x, dir.z));
			float pitch = (float) Math.toDegrees(-Math.asin(Math.max(-1.0, Math.min(1.0, dir.y))));
			CameraAccess access = (CameraAccess) camera;
			access.erbridge$setRotation(yaw, pitch);
			access.erbridge$setPosition(pos);
			fov = STATE.fovYDeg > 1.0F ? STATE.fovYDeg : mc.options.fov().get();
			return;
		}

		// DRIVE_HOST: send our camera to the host game.
		fov = mc.options.fov().get();
		Vec3 p = camera.getPosition();
		Vector3f f = camera.getLookVector();
		Vector3f u = camera.getUpVector();
		double[] pos = map.toHost(p.x, p.y, p.z);
		double[] target = map.toHost(p.x + f.x(), p.y + f.y(), p.z + f.z());
		double[] up = map.dirToHost(u.x(), u.y(), u.z());
		boolean passthrough = FramePassthrough.wanted();
		CONTROL.flags = Protocol.CTRL_OVERRIDE_CAMERA
			| (mc.player == null ? 0 : dev.ermc.bridge.FlightControl.bodyFlags(standIn, spectator,
				mc.player.getAbilities().flying, mc.player.isFallFlying(), mc.player.onGround()))
			| (passthrough ? Protocol.CTRL_COMPOSITE : 0) | (noDepthTest ? Protocol.CTRL_NO_DEPTH_TEST : 0)
			| (debugDepth ? Protocol.CTRL_DEBUG_DEPTH : 0);
		CONTROL.poseLag = poseLag;
		CONTROL.depthIndex = depthIndex;
		CONTROL.mcFrame++;
		for (int i = 0; i < 3; i++) {
			CONTROL.camPos[i] = (float) pos[i];
			CONTROL.camTarget[i] = (float) target[i];
			CONTROL.camUp[i] = (float) up[i];
		}
		CONTROL.fovYDeg = fov;
		if (mc.player != null) {
			float pt = camera.getPartialTickTime();
			double[] feet = map.toHost(net.minecraft.util.Mth.lerp(pt, mc.player.xo, mc.player.getX()),
				net.minecraft.util.Mth.lerp(pt, mc.player.yo, mc.player.getY()),
				net.minecraft.util.Mth.lerp(pt, mc.player.zo, mc.player.getZ()));
			for (int i = 0; i < 3; i++) {
				CONTROL.hunterPos[i] = (float) feet[i];
			}
			CONTROL.hunterYawDeg = mc.player.getViewYRot(pt);
		}
		controlling = true;
		if (dev.ermc.bridge.coop.CoopSession.enabled() && (CONTROL.flags & Protocol.CTRL_MOVE_HUNTER) != 0) {
			COOP_HOLD.acquire(mc.getConnection(), mc.player, map, ErLink.get().hostLife(), CoopClient.recallGeneration());
		} else {
			COOP_HOLD.reset();
		}
		if (!passthrough) {
			ErLink.get().writeControl(CONTROL);
		}
		// With passthrough, FramePassthrough sends this pose once the frame's pixels are ready.
	}

	private static void releaseHostCamera() {
		COOP_HOLD.reset();
		holding = false;
		passive = false;
		PASSIVE_RECALL.reset();
		FramePassthrough.invalidateView();
		if (controlling) {
			CONTROL.flags = automaticRecovery ? Protocol.CTRL_AUTO_RECOVERY : 0;
			CONTROL.mcFrame++;
			ErLink.get().writeControl(CONTROL);
			controlling = false;
		}
	}

	/** F8 invalidates both buffered pixels and all local-body controls immediately. */
	public static void handoff() {
		handoff(false);
	}

	static void handoff(boolean automatic) {
		automaticRecovery = automatic;
		controlling = true; // publish the reason even when a failed F8 never acquired a camera
		releaseHostCamera();
		driving = false;
	}

	static void retainHandoffReason(ControlState out) {
		if (automaticRecovery) out.flags |= Protocol.CTRL_AUTO_RECOVERY;
	}

	/** Rendering can become unavailable while a native handoff is still being observed. */
	static void suspendView() {
		releaseHostCamera();
		driving = false;
	}

	public static void disconnected() {
		handoff(false);
		driving = false;
		wasSpectator = false;
	}
}
