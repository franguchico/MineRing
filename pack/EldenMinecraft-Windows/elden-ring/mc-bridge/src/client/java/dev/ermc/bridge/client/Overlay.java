package dev.ermc.bridge.client;

import dev.ermc.bridge.link.GameState;
import dev.ermc.bridge.link.ErLink;
import dev.ermc.bridge.link.Protocol;
import dev.ermc.bridge.TerrainManager;
import net.minecraft.client.Minecraft;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.network.chat.Component;
import net.minecraft.world.phys.Vec3;
import org.lwjgl.glfw.GLFW;
import org.lwjgl.glfw.GLFWNativeCocoa;
import org.lwjgl.system.JNI;
import org.lwjgl.system.Platform;
import org.lwjgl.system.macosx.ObjCRuntime;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * Overlay mode: Minecraft's window becomes a borderless, always-on-top, transparent layer
 * glued to the host game's window. Only Minecraft's own content (blocks, entities, hand, HUD)
 * is opaque; everywhere else the host game shows through when desktop compositing supports it.
 *
 * <p>The window is always created with a transparent framebuffer so overlay mode can turn
 * on/off at runtime (e.g. when the host game starts after Minecraft). While off, the final
 * blit forces alpha to 1 so the window looks completely normal.
 */
public final class Overlay {
	private Overlay() {
	}

	private static final Logger LOG = LoggerFactory.getLogger("erbridge");

	private static boolean transparentWindow;
	private static boolean transparencyRequested;
	private static boolean mousePassthroughSupported;
	private static boolean active;
	/** One Windows input handoff per activation, after the auto-opened world is usable. */
	private static final InputHandoffGate INPUT_HANDOFF = new InputHandoffGate();
	private static long unreadySinceNs;
	private static boolean unready;
	private static String recoveryMessage;
	/** The player handed control to the host game (F8); its window is hidden but rendering continues. */
	private static boolean hostMode;
	private static int lastSwitchReq = Integer.MIN_VALUE;
	private static long window;
	private static int savedX, savedY, savedW, savedH;
	private static boolean windowStateSaved;
	private static int appliedX = Integer.MIN_VALUE, appliedY, appliedW, appliedH;
	private static long lastInWorldMs;
	private static boolean busy;
	/** Dev ("spin" command): turn the player this fast, every frame, to check that both games show the same pose. */
	public static float spinDegPerSec;
	private static long lastSpinNs;
	private static long lastWantMs;
	private static final GameState STATE = new GameState();
	private static final HostHandoffGate HOST_HANDOFF = new HostHandoffGate();

	public static boolean active() {
		return active;
	}

	public static boolean transparentWindow() {
		return transparentWindow;
	}

	public static boolean hostMode() {
		return hostMode;
	}

	public static boolean awaitingInput() { return INPUT_HANDOFF.pending(); }
	public static boolean blockGameplayInput() { return hostMode || busy || awaitingInput() || CameraSync.holding(); }
	static String recoveryMessage() { return recoveryMessage; }

	/** Native input/camera with an offscreen Minecraft world layer. No MC hand, HUD or local body. */
	public static boolean passive() {
		return active && hostMode;
	}

	/**
	 * Elden Ring is showing its own screens (the Tarnished dying, "YOU DIED", a loading screen) or
	 * Steve is still being moved to the Tarnished: the overlay stays but draws nothing.
	 */
	public static boolean hostBusy() {
		return busy;
	}

	/** Same placement authority as the camera: a guest cannot await a nonexistent local server. */
	static boolean awaitingPlacement(GameState state) {
		return !CoopClient.placementReady(state);
	}

	/** F8: keep the shared world ticking offscreen, give the host game its camera and input. */
	public static void switchToHost(Minecraft mc) {
		switchToHost(mc, false);
	}

	private static void switchToHost(Minecraft mc, boolean automatic) {
		if (hostMode || window == 0L) {
			return;
		}
		if (!automatic) RecoveryNotice.clear();
		hostMode = true;
		ErLink.get().beginHostHandoff(automatic); // terminal transaction before any control release
		CameraSync.handoff(automatic); // write reason before the hostFocusReq release store below
		cancelInputFocus();
		unready = false;
		CoopModeClient.localModeChanged(mc);
		if (mc.screen instanceof PauseScreen) mc.setScreen(null);
		KeyMapping.releaseAll();
		parkMinecraftBody(mc);
		mc.mouseHandler.releaseMouse();
		GLFW.glfwSetWindowAttrib(window, GLFW.GLFW_FLOATING, GLFW.GLFW_FALSE);
		GLFW.glfwHideWindow(window);
		ErLink.get().requestHostFocus();
		LOG.info("Control -> Elden Ring");
	}

	/** A lease failure releases controls/pixels exactly like F8, and leaves F8 available for retry. */
	static void recoverToHost(Minecraft mc, String reason) {
		recoveryMessage = reason + " · Controle no ER; F8 tenta Minecraft novamente";
		LOG.warn("Minecraft control released: {}", reason);
		switchToHost(mc, true);
		RecoveryNotice.show(mc, reason);
		if (mc.player != null) mc.player.displayClientMessage(Component.literal(recoveryMessage), false);
	}

	/** F8 in the host game (reported by the DLL): bring Minecraft back on top. */
	public static void switchToMc(Minecraft mc) {
		if (!hostMode || window == 0L) {
			return;
		}
		hostMode = false;
		recoveryMessage = null;
		RecoveryNotice.clear();
		INPUT_HANDOFF.begin(System.nanoTime());
		CameraSync.handoff();
		KeyMapping.releaseAll();
		parkMinecraftBody(mc);
		CoopModeClient.localModeChanged(mc);
		// Returning from a passive camera must recall the MC body through the existing server authority.
		CoopClient.requestRecall();
		// Leave the window hidden while recall/terrain are pending: even a shown, unfocused
		// transparent window intercepts clicks meant for Elden Ring.
		if (mc.screen instanceof PauseScreen) {
			mc.setScreen(null);
		}
		LOG.info("Control -> Minecraft");
	}

	/** Called right before GLFW creates Minecraft's window. */
	public static void applyWindowHints() {
		transparencyRequested = !Boolean.getBoolean("erbridge.disableOverlay");
		if (!transparencyRequested) {
			return;
		}
		GLFW.glfwWindowHint(GLFW.GLFW_TRANSPARENT_FRAMEBUFFER, GLFW.GLFW_TRUE);
		if (Platform.get() == Platform.MACOSX) {
			// Match the host game's pixel density (CrossOver renders it at 1x) and save 4x fill
			// rate. Always, not only when the host game is already up: a Retina framebuffer is
			// twice its size, too big to be drawn inside its frame.
			GLFW.glfwWindowHint(GLFW.GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW.GLFW_FALSE);
		} else if (Platform.get() == Platform.WINDOWS) {
			// The native host reports physical client pixels. Do not resize that rectangle
			// again when moving between monitors with different DPI scales.
			GLFW.glfwWindowHint(GLFW.GLFW_SCALE_TO_MONITOR, GLFW.GLFW_FALSE);
		}
	}

	public static void onWindowCreated(long handle) {
		window = handle;
		if (handle == 0L) {
			transparentWindow = false;
			return;
		}
		// A hint is only a request: do not activate an opaque overlay that hides the host.
		transparentWindow = transparencyRequested
			&& GLFW.glfwGetWindowAttrib(handle, GLFW.GLFW_TRANSPARENT_FRAMEBUFFER) == GLFW.GLFW_TRUE;
		if (transparencyRequested && !transparentWindow) {
			LOG.warn("Transparent framebuffer unavailable; overlay mode is disabled");
		}
		if (Platform.get() == Platform.WINDOWS) {
			int[] major = new int[1], minor = new int[1], revision = new int[1];
			GLFW.glfwGetVersion(major, minor, revision);
			mousePassthroughSupported = major[0] > 3 || (major[0] == 3 && minor[0] >= 4);
		}
	}

	/** Once per frame on the render thread, before anything is drawn. */
	public static void onFrame(Minecraft mc) {
		if (!hostMode && spinDegPerSec != 0.0F && mc.player != null) {
			long now = System.nanoTime();
			if (lastSpinNs != 0L) {
				float yaw = mc.player.getYRot() + spinDegPerSec * (now - lastSpinNs) / 1.0e9F;
				mc.player.setYRot(yaw);
				mc.player.yRotO = yaw;
			}
			lastSpinNs = now;
		} else {
			lastSpinNs = 0L;
		}
		ErLink link = ErLink.get();
		boolean alive = link.poll();
		if (!alive || mc.level == null || mc.player == null || mc.getConnection() == null
				|| !mc.getConnection().getConnection().isConnected()) RecoveryNotice.clear();
		link.bumpMcHeartbeat();
		int req = link.mcSwitchRequests();
		if (lastSwitchReq == Integer.MIN_VALUE) {
			lastSwitchReq = req;
		} else if (req != lastSwitchReq) {
			lastSwitchReq = req;
			switchToMc(mc);
		}
		boolean haveState = alive && link.snapshot(STATE);
		// The initial Stranded Graveyard respawn can still be lying down behind a
		// tutorial popup when PLAYER_VALID returns. Give the game its input and
		// body back for deaths/warps; F8 explicitly resumes Minecraft afterwards.
		if (HOST_HANDOFF.update(alive, haveState, link.hostLife(),
				haveState && STATE.has(Protocol.STATE_PLAYER_DEAD), active && !hostMode)) {
			LOG.info("Death or new host placement: control -> Elden Ring; close its screens, then F8 resumes Minecraft");
			switchToHost(mc);
		}
		// Only take over once a hunter is actually in the world, so the host game's title screen and
		// menus stay usable. Short gaps (area loads) don't toggle the window.
		long now = System.currentTimeMillis();
		if (haveState && STATE.has(Protocol.STATE_PLAYER_VALID)) {
			lastInWorldMs = now;
		}
		// A death or loading screen within a session keeps the overlay (drawing nothing), so the
		// window doesn't jump back and forth; only a longer absence (title screen) hands it back.
		boolean hostBusyNow = haveState && STATE.has(Protocol.STATE_HOST_BUSY);
		boolean inWorld = haveState && (now - lastInWorldMs < 3000 || hostBusyNow);
		boolean passiveAvailable = hostMode && FramePassthrough.enabled()
			&& dev.ermc.bridge.coop.CoopSession.enabled() && mc.level != null && mc.player != null
			&& mc.getConnection() != null && mc.getConnection().getConnection().isConnected();
		boolean want = transparentWindow && window != 0L && inWorld && (!hostMode || passiveAvailable)
			&& !mc.getWindow().isFullscreen();
		if (want) {
			lastWantMs = now;
		}
		// Turn on immediately, but only turn off after a sustained reason (or a user toggle).
		if (want && !active) {
			setActive(mc, true);
		} else if (!want && active && (hostMode || now - lastWantMs > 1500)) {
			setActive(mc, false);
		}
		if (active && STATE.has(Protocol.STATE_WINDOW_VALID)) {
			follow(STATE.winX, STATE.winY, STATE.winW, STATE.winH);
		}
		boolean nowBusy = active && (hostBusyNow || (!hostMode && awaitingPlacement(STATE)));
		if (nowBusy && mc.screen instanceof PauseScreen) {
			// Nothing is drawn while Elden Ring shows its own screen, so a pause menu would be
			// invisible yet take the clicks meant for Elden Ring.
			mc.setScreen(null);
		}
		if (nowBusy != busy) {
			busy = nowBusy;
			LOG.info(busy ? "Elden Ring shows its own screen (death, loading or recall): drawing nothing"
				: "Drawing again");
		}
		if (!hostMode && active && busy) {
			suspendGameplayInput(mc);
			mc.mouseHandler.releaseMouse();
			if (!awaitingInput()) {
				long ns = System.nanoTime();
				if (!unready) { unready = true; unreadySinceNs = ns; }
				if (ns < unreadySinceNs || ns - unreadySinceNs >= CoopControlHold.MAX_HOLD_NANOS)
					recoverToHost(mc, CoopClient.recoveryReason(STATE));
			}
		} else if (unready) {
			unready = false;
			if (!hostMode && active && !awaitingInput()) INPUT_HANDOFF.begin(System.nanoTime());
		}
		updateInputFocus(mc);
	}

	static void parkMinecraftBody(Minecraft mc) {
		if (mc.player == null) return;
		mc.player.setDeltaMovement(Vec3.ZERO);
		mc.player.fallDistance = 0;
		// A short terrain/network hold is still Minecraft flight. F8/fallback ends it.
		if (hostMode && mc.player.isFallFlying()) mc.player.stopFallFlying();
		mc.player.input.forwardImpulse = mc.player.input.leftImpulse = 0;
		mc.player.input.jumping = mc.player.input.shiftKeyDown = false;
		mc.player.setSprinting(false);
		if (mc.gameMode != null) mc.gameMode.stopDestroyBlock();
		if (mc.player.isUsingItem()) {
			if (mc.gameMode != null) mc.gameMode.releaseUsingItem(mc.player);
			else mc.player.stopUsingItem();
		}
	}

	/** Drain gameplay clicks during a pause, preserving the bridge's F8 binding. */
	public static void suspendGameplayInput(Minecraft mc) {
		for (KeyMapping key : new KeyMapping[]{mc.options.keyUp, mc.options.keyDown, mc.options.keyLeft,
				mc.options.keyRight, mc.options.keyJump, mc.options.keyShift, mc.options.keySprint,
				mc.options.keyAttack, mc.options.keyUse, mc.options.keyDrop, mc.options.keySwapOffhand,
				mc.options.keyInventory}) {
			key.setDown(false);
			while (key.consumeClick()) { }
		}
		parkMinecraftBody(mc);
	}

	private static void cancelInputFocus() {
		INPUT_HANDOFF.cancel();
	}

	private static void requestInputFocus() {
		// FLOATING/DECORATED use SWP_NOACTIVATE on Windows. Ask once; GLFW's focus
		// callback may not update Minecraft.isWindowActive until the next event poll.
		GLFW.glfwFocusWindow(window);
	}

	private static void updateInputFocus(Minecraft mc) {
		if (!awaitingInput()) {
			return;
		}
		if (hostMode || window == 0L) {
			cancelInputFocus();
			return;
		}
		boolean ready = active && !busy && ErLink.get().alive() && mc.level != null && mc.player != null
			&& !mc.player.isDeadOrDying() && mc.getConnection() != null
			&& mc.getConnection().getConnection().isConnected() && CameraSync.placementReadyForInput(mc, STATE);
		boolean blocked = CoopClient.returnBlocked() || !ErLink.get().alive()
			|| STATE.has(Protocol.STATE_PLAYER_DEAD) || STATE.has(Protocol.STATE_HOST_BUSY);
		if (ready && !blocked && GLFW.glfwGetWindowAttrib(window, GLFW.GLFW_VISIBLE) == GLFW.GLFW_FALSE) {
			GLFW.glfwSetWindowAttrib(window, GLFW.GLFW_FOCUS_ON_SHOW, GLFW.GLFW_FALSE);
			GLFW.glfwSetWindowAttrib(window, GLFW.GLFW_FLOATING, GLFW.GLFW_TRUE);
			GLFW.glfwShowWindow(window);
		}
		boolean focused = mc.isWindowActive() && GLFW.glfwGetWindowAttrib(window, GLFW.GLFW_FOCUSED) == GLFW.GLFW_TRUE;
		switch (INPUT_HANDOFF.update(blocked, ready, mc.screen != null, focused, System.nanoTime())) {
			case FOCUS -> requestInputFocus();
			case GRAB -> {
				KeyMapping.releaseAll();
				parkMinecraftBody(mc);
				mc.mouseHandler.grabMouse();
				if (mc.mouseHandler.isMouseGrabbed()) {
					cancelInputFocus();
					CoopModeClient.localModeChanged(mc);
					LOG.info("Minecraft input handoff completed after safe placement");
				} else recoverToHost(mc, "Minecraft: captura do mouse recusada");
			}
			case MENU -> {
				KeyMapping.releaseAll();
				parkMinecraftBody(mc);
				mc.mouseHandler.releaseMouse();
				cancelInputFocus();
				CoopModeClient.localModeChanged(mc);
				LOG.info("Minecraft input handoff completed in menu; mouse released");
			}
			case RETURN -> recoverToHost(mc, !ErLink.get().alive() ? "Elden Ring: conexão local indisponível"
				: ready && !blocked ? "Minecraft: janela não recebeu foco" : CoopClient.recoveryReason(STATE));
			case WAIT -> { }
		}
	}

	private static void setActive(Minecraft mc, boolean on) {
		boolean returning = awaitingInput();
		active = on;
		if (!on) {
			cancelInputFocus();
			CameraSync.suspendView();
			KeyMapping.releaseAll();
			mc.mouseHandler.releaseMouse();
			if (returning && !hostMode) recoverToHost(mc, "Minecraft: overlay indisponível; use modo janela");
		} else if (!hostMode && !awaitingInput()) {
			INPUT_HANDOFF.begin(System.nanoTime());
		}
		LOG.info("Overlay mode {}", on ? "ON" : "OFF");
		if (on) {
			// Taking over (startup, back from the host game with F8, after a loading screen):
			// its hunter is where the player really is, so Steve starts there, and nothing
			// drives its camera or hunter until he has been moved.
			if (!hostMode && !returning) CoopClient.requestRecall();
			if (!windowStateSaved) {
				int[] x = new int[1], y = new int[1], w = new int[1], h = new int[1];
				GLFW.glfwGetWindowPos(window, x, y);
				GLFW.glfwGetWindowSize(window, w, h);
				savedX = x[0];
				savedY = y[0];
				savedW = w[0];
				savedH = h[0];
				windowStateSaved = true;
			}
			GLFW.glfwSetWindowAttrib(window, GLFW.GLFW_DECORATED, GLFW.GLFW_FALSE);
			GLFW.glfwSetWindowAttrib(window, GLFW.GLFW_FLOATING, hostMode ? GLFW.GLFW_FALSE : GLFW.GLFW_TRUE);
			setShadow(false);
			// When the host game draws our frames, this window is fully transparent; macOS would
			// then let clicks fall through to it unless told otherwise.
			objcBool("setIgnoresMouseEvents:", false);
			if (Platform.get() == Platform.WINDOWS && mousePassthroughSupported) {
				// Keep Minecraft's input window interactive even when all its pixels are clear.
				// This attribute is available in GLFW 3.4; older GLFW keeps its input defaults.
				GLFW.glfwSetWindowAttrib(window, GLFW.GLFW_MOUSE_PASSTHROUGH, GLFW.GLFW_FALSE);
			}
			appliedX = Integer.MIN_VALUE;
		} else if (!hostMode) {
			GLFW.glfwSetWindowAttrib(window, GLFW.GLFW_FLOATING, GLFW.GLFW_FALSE);
			GLFW.glfwSetWindowAttrib(window, GLFW.GLFW_DECORATED, GLFW.GLFW_TRUE);
			setShadow(true);
			if (savedW > 0 && savedH > 0) {
				GLFW.glfwSetWindowSize(window, savedW, savedH);
				GLFW.glfwSetWindowPos(window, savedX, savedY);
			}
			windowStateSaved = false;
		}
	}

	/**
	 * The host game reports its client area in Windows screen coordinates. CrossOver (Retina
	 * mode off) maps one Windows pixel to one macOS point with the same top-left origin as GLFW.
	 * Native Windows expects the host's physical client pixels; SCALE_TO_MONITOR is disabled.
	 */
	private static void follow(int x, int y, int w, int h) {
		if (w < 64 || h < 64) {
			return;
		}
		if (x == appliedX && y == appliedY && w == appliedW && h == appliedH) {
			return;
		}
		GLFW.glfwSetWindowSize(window, w, h);
		GLFW.glfwSetWindowPos(window, x, y);
		appliedX = x;
		appliedY = y;
		appliedW = w;
		appliedH = h;
		LOG.info("Overlay glued to host-game client area {}x{} at {},{}", w, h, x, y);
	}

	/** Borderless transparent windows would otherwise cast a shadow around every block. */
	private static void setShadow(boolean shadow) {
		objcBool("setHasShadow:", shadow);
	}

	/** Calls an NSWindow setter taking a BOOL. */
	private static void objcBool(String selector, boolean value) {
		if (Platform.get() != Platform.MACOSX) {
			return;
		}
		try {
			CocoaWindow.setBoolean(window, selector, value);
		} catch (Throwable t) {
			LOG.warn("NSWindow {} failed: {}", selector, t.toString());
		}
	}

	/** Loaded only after the macOS platform guard, never on native Windows. */
	private static final class CocoaWindow {
		private static void setBoolean(long handle, String selector, boolean value) {
			long nsWindow = GLFWNativeCocoa.glfwGetCocoaWindow(handle);
			long msgSend = ObjCRuntime.getLibrary().getFunctionAddress("objc_msgSend");
			if (nsWindow != 0L && msgSend != 0L) {
				JNI.invokePPV(nsWindow, ObjCRuntime.sel_getUid(selector), value, msgSend);
			}
		}
	}
}
