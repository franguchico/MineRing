package dev.ermc.bridge.client;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.platform.GlStateManager;
import dev.ermc.bridge.CoordMap;
import dev.ermc.bridge.link.BridgePaths;
import dev.ermc.bridge.link.ControlState;
import dev.ermc.bridge.link.ErLink;
import dev.ermc.bridge.link.FrameProtocol;
import net.minecraft.client.Minecraft;
import org.lwjgl.opengl.GL11;
import org.lwjgl.opengl.GL12;
import org.lwjgl.opengl.GL15;
import org.lwjgl.opengl.GL21;
import org.lwjgl.opengl.GL30;
import org.lwjgl.opengl.GL32;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

/**
 * Sends Minecraft's frames to the host game, which draws them into its own frame (see
 * elden-ring/er-bridge). Per frame:
 * <ol>
 *   <li>after the world is drawn (before the hand): async readback of world color + depth,
 *   then the color buffer is cleared so the hand draws onto a transparent layer;</li>
 *   <li>after the hand: async readback of the hand layer (the host game lights it like the
 *   world, but never hides it behind its walls), then the color buffer is cleared again;</li>
 *   <li>at the end of the frame: async readback of the HUD layer (drawn as is);</li>
 *   <li>after a zero-timeout fence poll confirms completion: the readbacks are copied into
 *   a frames.shm slot, and only then is that
 *   frame's camera pose handed to the host game, so it always has the matching pixels for the
 *   pose it renders.</li>
 * </ol>
 * Minecraft's own window then shows nothing (it only keeps the input focus).
 */
public final class FramePassthrough {
	private FramePassthrough() {
	}

	private static final Logger LOG = LoggerFactory.getLogger("erbridge");
	private static final VarHandle INT = MethodHandles.byteBufferViewVarHandle(int[].class, ByteOrder.LITTLE_ENDIAN);
	private static final VarHandle LONG = MethodHandles.byteBufferViewVarHandle(long[].class, ByteOrder.LITTLE_ENDIAN);
	private static final Path PATH = BridgePaths.framesShm();
	private static final int MAX_W = FrameProtocol.MAX_W;
	private static final int MAX_H = FrameProtocol.MAX_H;
	private static final int SLOTS = FrameProtocol.SLOTS;
	private static final int HDR = FrameProtocol.HDR;
	/** Layout version 3: QHD four-layer slots, with one extra Windows framebuffer row. */
	private static final long FILE_SIZE = FrameProtocol.FILE_SIZE;

	/**
	 * User toggle (F6). Effective only once the host game confirms it is compositing (its
	 * D3D12 Present hook, elden-ring/er-bridge/src/compositor.cpp); until then the separate
	 * overlay window (Overlay.java) shows Minecraft.
	 */
	private static boolean enabled = true;
	private static MappedByteBuffer frames;
	private static boolean failed;

	// Two sets of PBOs (world color, world depth, HUD color, hand color), alternating per frame.
	private static final int[][] PBO = new int[2][4];
	private static int pboW, pboH;
	private static final boolean[] PENDING = new boolean[2];
	// A discarded ticket still owns its GPU storage until its fence completes.
	private static final long[] FENCE = new long[2];
	private static final boolean[] UNCERTIFIED = new boolean[2];
	/** The set's hand layer was captured (else the hand, if any, is in the HUD layer). */
	private static final boolean[] HAND = new boolean[2];
	private static final boolean[] PASSIVE = new boolean[2];
	private static final CompositeFrameGate.Ticket[] TICKET = new CompositeFrameGate.Ticket[2];
	private static final FrameNoticeCapture[] NOTICE = new FrameNoticeCapture[2];
	private static final CompositeFrameGate VIEW = new CompositeFrameGate();
	private static final FramePublicationLease PUBLICATION = new FramePublicationLease();
	private static final FrameCaptureDiagnostics DIAGNOSTICS = new FrameCaptureDiagnostics();
	private static boolean publishedPassive;
	private static final long[] FRAME_ID = new long[2];
	private static final ControlState[] POSE = {new ControlState(), new ControlState()};
	private static final float[][] CLIP = new float[2][3];
	private static boolean worldCaptured;
	private static int capturingSet = -1;
	private static long frameCounter;
	/**
	 * Host-game frame counter at the last capture. The host game takes one pose per frame and
	 * presents it a frame later, so capturing faster than it runs (60 vs ~40 fps in big areas)
	 * would overwrite the slot it still needs, and it would show a frame with the wrong pose.
	 */
	private static long capturedAtHostFrame = Long.MIN_VALUE;
	private static int cur;
	private static final ControlState LAST_PUBLISHED_POSE = new ControlState();
	private static CompositeFrameGate.Context publishedContext;
	private static long lastPublishedFrame;
	// Cached pixels for the existing bounded HOLD are separate from fresh peer proof.
	private static boolean havePublishedPose;

	/** Keep the matching pixels and pose together during an explicitly leased body hold. */
	static boolean holdLastPublishedPose(ControlState out) {
		if (!holdCacheUsable()) return false;
		out.copyFrom(LAST_PUBLISHED_POSE);
		return true;
	}

	private static boolean holdCacheUsable() {
		return enabled && !failed && havePublishedPose && !publishedPassive && ErLink.get().alive()
			&& CompositeFrameGate.same(publishedContext, currentContext(Minecraft.getInstance()));
	}

	private static boolean preservingHoldCache() {
		return !wanted() && CameraSync.holding() && holdCacheUsable();
	}

	static void clearPublishedPose() {
		havePublishedPose = false;
		publishedContext = null;
		PUBLICATION.clearProof();
		CoopModeClient.clearRenderProof();
	}
	static boolean hasPublishedWorld() {
		long now = System.nanoTime();
		if (havePublishedPose && (!ErLink.get().alive()
				|| !CompositeFrameGate.same(publishedContext, currentContext(Minecraft.getInstance())))) clearPublishedPose();
		if (PUBLICATION.expireProof(now)) CoopModeClient.clearRenderProof();
		return havePublishedPose && PUBLICATION.fresh(now);
	}

	static void beginView(CompositeFrameGate.Context context) {
		if (VIEW.update(context)) discardCaptures();
	}

	static void invalidateView() {
		VIEW.invalidate();
		discardCaptures();
	}

	private static void discardCaptures() {
		clearPublishedPose();
		PENDING[0] = PENDING[1] = false;
		TICKET[0] = TICKET[1] = null;
		NOTICE[0] = NOTICE[1] = null;
		worldCaptured = false;
		capturedAtHostFrame = Long.MIN_VALUE;
	}

	public static boolean enabled() {
		return enabled;
	}

	public static void toggle() {
		enabled = !enabled;
		CameraSync.handoff();
	}

	/** True while the host game is drawing our frames, so our own window should stay empty. */
	public static boolean activeInHost() {
		boolean fresh = hasPublishedWorld();
		return enabled && !failed && Overlay.active() && CameraSync.hostCompositing()
			&& ((fresh && (CameraSync.passive() || CameraSync.driving()))
				|| (CameraSync.holding() && holdCacheUsable()));
	}

	/** Whether frames are being captured (then CameraSync leaves pose publishing to us). */
	public static boolean wanted() {
		return enabled && !failed && Overlay.active()
			&& (CameraSync.passive() || (!Overlay.hostMode() && CameraSync.mode() == CameraSync.Mode.DRIVE_HOST && CameraSync.driving()))
			&& fits(Minecraft.getInstance().getMainRenderTarget());
	}

	private static boolean tooBigLogged;

	/**
	 * Frames larger than a frames.shm slot can't be sent. Then nothing is captured and the
	 * camera pose goes to the host game directly (overlay window mode) instead of never at all.
	 */
	private static boolean fits(RenderTarget main) {
		boolean fits = FrameProtocol.fits(main.width, main.height);
		if (!fits && !tooBigLogged) {
			tooBigLogged = true;
			LOG.warn("Frame {}x{} is larger than {}x{}; showing Minecraft in its own window instead of inside Elden Ring",
				main.width, main.height, MAX_W, MAX_H);
		}
		return fits;
	}

	private static boolean open() {
		if (frames != null) {
			return true;
		}
		try {
			Path p = PATH;
			Files.createDirectories(p.getParent());
			try (FileChannel ch = FileChannel.open(p, StandardOpenOption.READ, StandardOpenOption.WRITE, StandardOpenOption.CREATE)) {
				if (ch.size() < FILE_SIZE) {
					ch.write(ByteBuffer.wrap(new byte[1]), FILE_SIZE - 1);
				}
				frames = ch.map(FileChannel.MapMode.READ_WRITE, 0, FILE_SIZE);
				frames.order(ByteOrder.LITTLE_ENDIAN);
			}
			FrameProtocol.initialize(frames);
			LOG.info("Frame passthrough: mapped {} ({} MB)", PATH, FILE_SIZE >> 20);
			return true;
		} catch (IOException | RuntimeException e) {
			LOG.warn("Frame passthrough unavailable: {}", e.toString());
			failed = true;
			return false;
		}
	}

	private static boolean ensurePbos(int w, int h) {
		if (w == pboW && h == pboH && PBO[0][0] != 0) {
			return true;
		}
		// Defer a resize, including invalidated old captures, without deleting/reallocating
		// a buffer whose queued readback has not been certified complete.
		for (int set = 0; set < 2; set++) {
			if (FENCE[set] != 0 || UNCERTIFIED[set] || PENDING[set]) return false;
		}
		clearPublishedPose();
		for (int[] set : PBO) {
			for (int i = 0; i < set.length; i++) {
				if (set[i] != 0) {
					GL15.glDeleteBuffers(set[i]);
				}
				set[i] = GL15.glGenBuffers();
				GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, set[i]);
				GL15.glBufferData(GL21.GL_PIXEL_PACK_BUFFER, (long) w * h * 4, GL15.GL_STREAM_READ);
			}
		}
		GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, 0);
		PENDING[0] = PENDING[1] = false;
		pboW = w;
		pboH = h;
		return true;
	}

	private static void readInto(int pbo, int format, int type) {
		GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, pbo);
		GL11.glReadPixels(0, 0, pboW, pboH, format, type, 0L);
	}

	/** GameRenderer.renderLevel, right after the level (before the hand). */
	public static void afterWorld(Minecraft mc) {
		// If a previous render exited before endFrame, fence its already queued reads
		// instead of reusing their storage as though no capture occurred.
		if (capturingSet >= 0) {
			TICKET[capturingSet] = null;
			sealCapture(mc, false);
		}
		worldCaptured = false;
		processReadbacks(mc, -1);
		checkProgress(mc);
		String summary = DIAGNOSTICS.summary(System.nanoTime());
		if (summary != null) LOG.info(summary);
		if (!wanted() || !open()) {
			return;
		}
		long hostFrame = ErLink.get().hostFrame();
		if (hostFrame == capturedAtHostFrame) {
			return;  // the host game hasn't shown a frame since the last capture
		}
		RenderTarget main = mc.getMainRenderTarget();
		if (!ensurePbos(main.width, main.height)) return;
		cur = -1;
		for (int i = 0; i < 2; i++) {
			int set = (int) ((frameCounter + i) & 1);
			if (FENCE[set] == 0 && !UNCERTIFIED[set] && !PENDING[set]) { cur = set; break; }
		}
		if (cur < 0) return; // both sets are busy; no glReadPixels/map/reuse
		HAND[cur] = false;
		NOTICE[cur] = null;
		PASSIVE[cur] = CameraSync.passive();
		TICKET[cur] = VIEW.capture(System.nanoTime());
		if (TICKET[cur] == null) return;
		capturedAtHostFrame = hostFrame;
		capturingSet = cur;
		GlStateManager._glBindFramebuffer(GL30.GL_READ_FRAMEBUFFER, main.frameBufferId);
		GL11.glPixelStorei(GL11.GL_PACK_ALIGNMENT, 4);
		readInto(PBO[cur][0], GL12.GL_BGRA, GL12.GL_UNSIGNED_INT_8_8_8_8_REV);
		readInto(PBO[cur][1], GL11.GL_DEPTH_COMPONENT, GL11.GL_FLOAT);
		GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, 0);
		// The hand and screen effects, then the HUD, go onto transparent layers over the world.
		clearColor(main);
		CameraSync.copyCurrentPose(POSE[cur]);
		CLIP[cur][0] = 0.05F;
		CLIP[cur][1] = mc.gameRenderer.getDepthFar();
		CLIP[cur][2] = PASSIVE[cur] ? CameraSync.passiveAspect() : (float) main.width / main.height;
		worldCaptured = true;
	}

	private static void clearColor(RenderTarget main) {
		main.bindWrite(false);
		GlStateManager._colorMask(true, true, true, true);
		GlStateManager._clearColor(0, 0, 0, 0);
		GlStateManager._clear(GL11.GL_COLOR_BUFFER_BIT, Minecraft.ON_OSX);
	}

	/** GameRenderer.renderLevel, right after the hand: read back the hand layer. */
	public static void afterHand(Minecraft mc) {
		if (!worldCaptured || PASSIVE[cur]) {
			return;
		}
		RenderTarget main = mc.getMainRenderTarget();
		GlStateManager._glBindFramebuffer(GL30.GL_READ_FRAMEBUFFER, main.frameBufferId);
		readInto(PBO[cur][3], GL12.GL_BGRA, GL12.GL_UNSIGNED_INT_8_8_8_8_REV);
		GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, 0);
		clearColor(main);
		HAND[cur] = true;
	}

	/** End of frame, before the main target is shown: read back the overlay layer, publish last frame. */
	public static void endFrame(Minecraft mc) {
		int captured = capturingSet;
		sealCapture(mc, worldCaptured);
		processReadbacks(mc, captured);
		checkProgress(mc);
		// Publish visibility after this frame's camera and pixels, not before camera setup.
		CoopModeClient.onFrame(mc);
	}

	private static CompositeFrameGate.Context currentContext(Minecraft mc) {
		return new CompositeFrameGate.Context(mc.getConnection(), mc.level, mc.player,
			CoordMap.get(), ErLink.get().hostLife(), CameraSync.passive());
	}

	private static boolean captureValid(Minecraft mc, CompositeFrameGate.Ticket ticket, boolean passive) {
		RenderTarget main = mc.getMainRenderTarget();
		return wanted() && ErLink.get().alive() && passive == CameraSync.passive()
			&& main.width == pboW && main.height == pboH
			&& VIEW.accepts(ticket, currentContext(mc), System.nanoTime());
	}

	private static void checkProgress(Minecraft mc) {
		hasPublishedWorld(); // expire eligibility even if the host counter stopped advancing
		if (!wanted()) {
			// A deliberate HOLD has its own existing deadline. A handoff releases this watch.
			if (!CameraSync.holding()) PUBLICATION.reset();
			return;
		}
		long now = System.nanoTime();
		PUBLICATION.expect(currentContext(mc), now);
		if (PUBLICATION.takeRecovery(now)) {
			clearPublishedPose();
			if (Overlay.hostMode()) {
				// Passive composition owns no input/body. Revoke its request, retaining native input.
				CameraSync.suspendView();
			} else {
				Overlay.recoverToHost(mc, "Minecraft: leitura da imagem indisponível");
			}
		}
	}

	private static void sealCapture(Minecraft mc, boolean complete) {
		int set = capturingSet;
		if (set < 0) return;
		capturingSet = -1;
		worldCaptured = false;
		if (!complete || !captureValid(mc, TICKET[set], PASSIVE[set])) {
			if (TICKET[set] != null) DIAGNOSTICS.reject(TICKET[set], System.nanoTime());
			TICKET[set] = null;
		}
		try {
			if (TICKET[set] != null) {
				RenderTarget gui = mc.getMainRenderTarget();
				if (PASSIVE[set]) {
					// draw clears the GUI target and renders only the opt-in reason banner.
					CompositeFrameGate.Context noticeContext = currentContext(mc);
					long noticeAt = System.nanoTime();
					int ttl = RecoveryNotice.draw(mc);
					gui = RecoveryNotice.colorTarget();
					if (ttl > 0 && ttl <= FrameNoticeCapture.MAX_TTL_MILLIS && gui != null
							&& gui.width == pboW && gui.height == pboH && captureValid(mc, TICKET[set], true))
						NOTICE[set] = new FrameNoticeCapture(ttl, noticeAt, noticeContext);
				}
				if (!PASSIVE[set] || NOTICE[set] != null) {
					GlStateManager._glBindFramebuffer(GL30.GL_READ_FRAMEBUFFER, gui.frameBufferId);
					try { readInto(PBO[set][2], GL12.GL_BGRA, GL12.GL_UNSIGNED_INT_8_8_8_8_REV); }
					finally {
						if (PASSIVE[set]) GlStateManager._glBindFramebuffer(GL30.GL_READ_FRAMEBUFFER, mc.getMainRenderTarget().frameBufferId);
					}
				}
			}
			FRAME_ID[set] = ++frameCounter;
			POSE[set].mcFrame = FRAME_ID[set];
		} catch (RuntimeException e) {
			PENDING[set] = false;
			TICKET[set] = null;
			NOTICE[set] = null;
			clearPublishedPose();
			DIAGNOSTICS.syncFailures++;
		} finally {
			// World reads were already queued before optional GUI/notice rendering.
			// Even a rejected capture needs a completion fence so its storage can
			// be reclaimed after a zero-time poll instead of becoming stranded.
			try {
				FENCE[set] = GL32.glFenceSync(GL32.GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
				if (FENCE[set] == 0) {
					UNCERTIFIED[set] = true;
					PENDING[set] = false;
					TICKET[set] = null;
					clearPublishedPose();
					DIAGNOSTICS.syncFailures++;
				} else {
					PENDING[set] = TICKET[set] != null;
					GL11.glFlush(); // hidden-window progress without a blocking wait
				}
			} catch (RuntimeException e) {
				UNCERTIFIED[set] = true;
				PENDING[set] = false;
				TICKET[set] = null;
				clearPublishedPose();
				DIAGNOSTICS.syncFailures++;
			}
			GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, 0);
		}
	}

	private static void processReadbacks(Minecraft mc, int justCaptured) {
		for (int set = 0; set < 2; set++) {
			if (set == justCaptured || FENCE[set] == 0) continue;
			int status;
			try { status = GL32.glClientWaitSync(FENCE[set], 0, 0L); }
			catch (RuntimeException e) { status = GL32.GL_WAIT_FAILED; }
			if (status != GL32.GL_ALREADY_SIGNALED && status != GL32.GL_CONDITION_SATISFIED) {
				if (status == GL32.GL_TIMEOUT_EXPIRED) DIAGNOSTICS.fenceSkips++;
				else DIAGNOSTICS.syncFailures++;
				if (status == GL32.GL_WAIT_FAILED || (PENDING[set] && !captureValid(mc, TICKET[set], PASSIVE[set]))) {
					if (PENDING[set]) DIAGNOSTICS.reject(TICKET[set], System.nanoTime());
					PENDING[set] = false;
					TICKET[set] = null;
					if (status == GL32.GL_WAIT_FAILED || !preservingHoldCache()) clearPublishedPose();
				}
				continue; // keep both fence and PBO storage, including after expiry/handoff
			}
			GL32.glDeleteSync(FENCE[set]);
			FENCE[set] = 0;
			if (UNCERTIFIED[set]) UNCERTIFIED[set] = false; // a retained fence eventually certified completion
			boolean pending = PENDING[set];
			PENDING[set] = false;
			if (pending) {
				// GPU completion can occur between polls: the newer fence may be signaled
				// after the older one timed out. Never regress the published camera/frame.
				if (FRAME_ID[set] <= lastPublishedFrame) {
					DIAGNOSTICS.rejected++;
				} else if (!captureValid(mc, TICKET[set], PASSIVE[set])) {
					DIAGNOSTICS.reject(TICKET[set], System.nanoTime());
					if (!preservingHoldCache()) clearPublishedPose();
				} else if (!publish(mc, set)) clearPublishedPose();
			}
			TICKET[set] = null;
		}
	}

	private static boolean publish(Minecraft mc, int set) {
		CompositeFrameGate.Ticket ticket = TICKET[set];
		int w = pboW;
		int h = pboH;
		long frameId = FRAME_ID[set];
		int slot = (int) (frameId % SLOTS);
		int base = FrameProtocol.slotOffset(slot);
		// Odd while the slot is being written: the host game skips it (and a copy it started
		// before sees the sequence change).
		int s = (int) INT.getOpaque(frames, base);
		int writingSeq = s | 1;
		INT.setOpaque(frames, base, writingSeq);
		VarHandle.releaseFence();
		long layer = FrameProtocol.layerBytes(w, h);
		boolean hand = HAND[set];
		boolean passive = PASSIVE[set];
		FrameNoticeCapture notice = NOTICE[set];
		int noticeTtl = passive ? remainingNotice(mc, notice) : 0;
		int layers = passive ? (noticeTtl > 0 ? 3 : 2) : (hand ? 4 : 3);
		long copyNs = 0;
		try {
			for (int i = 0; i < layers; i++) {
				if (passive && i == 2 && remainingNotice(mc, notice) == 0) break;
				GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, PBO[set][i]);
				long mapStart = System.nanoTime();
				ByteBuffer src;
				try { src = GL30.glMapBufferRange(GL21.GL_PIXEL_PACK_BUFFER, 0, layer, GL30.GL_MAP_READ_BIT); }
				finally { DIAGNOSTICS.worstMapNs = Math.max(DIAGNOSTICS.worstMapNs, System.nanoTime() - mapStart); }
				if (src == null) {
					// Leave this slot odd/unpublished; old texture bytes are never declared fresh pixels.
					DIAGNOSTICS.nullMaps++;
					return false;
				}
				boolean intact;
				long copyStart = System.nanoTime();
				try {
					ByteBuffer dst = frames.duplicate().order(ByteOrder.LITTLE_ENDIAN);
					dst.position((int) (base + HDR + layer * i));
					dst.put(src);
				} finally {
					copyNs += System.nanoTime() - copyStart;
					intact = GL15.glUnmapBuffer(GL21.GL_PIXEL_PACK_BUFFER);
				}
				if (!intact) { DIAGNOSTICS.unmapFailures++; return false; }
			}
			frames.putInt(base + 0x04, w);
			frames.putInt(base + 0x08, h);
			frames.putLong(base + 0x10, frameId);
			frames.putLong(base + 0x18, frameId);
			frames.putFloat(base + 0x20, CLIP[set][0]);
			frames.putFloat(base + 0x24, CLIP[set][1]);
			frames.putFloat(base + 0x28, POSE[set].fovYDeg);
			frames.putFloat(base + 0x2C, CLIP[set][2]);
			// Mapping/copying can cross the deadline or a handoff/context replacement. The
			// pre-map check is insufficient. Do not certify pixels or send an old pose then.
			if ((int) INT.getOpaque(frames, base) != writingSeq || frames.getInt(0) != FrameProtocol.MAGIC
					|| frames.getInt(4) != FrameProtocol.VERSION || !captureValid(mc, ticket, passive)) {
				INT.setRelease(frames, base, writingSeq);
				DIAGNOSTICS.reject(ticket, System.nanoTime());
				return false;
			}
			// The optional GUI may expire during any map/copy. Its bytes then remain ignored:
			// flag 1 / TTL 0 declares only world+depth, including a skipped third-layer copy.
			noticeTtl = passive ? remainingNotice(mc, notice) : 0;
			frames.putInt(base + 0x0C, layerFlags(passive, hand, noticeTtl));
			frames.putInt(base + 0x30, noticeTtl);
			INT.setRelease(frames, base, writingSeq + 1);
			frames.putInt(0x08, slot);
			LONG.setRelease(frames, 0x10, frameId);
			// Only now may the host game render this pose: the pixels for it are in place.
			ErLink.get().writeControl(POSE[set]);
			LAST_PUBLISHED_POSE.copyFrom(POSE[set]);
			publishedContext = currentContext(mc);
			lastPublishedFrame = frameId;
			havePublishedPose = true;
			publishedPassive = passive;
			PUBLICATION.published(ticket.capturedAt(), System.nanoTime());
			DIAGNOSTICS.successes++;
			return true;
			} catch (RuntimeException e) {
			// An exception must not leave a copied slot advertised as usable control.
			INT.setRelease(frames, base, writingSeq);
			DIAGNOSTICS.mapErrors++;
			return false;
		} finally {
			DIAGNOSTICS.worstCopyNs = Math.max(DIAGNOSTICS.worstCopyNs, copyNs);
			GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, 0);
		}
	}

	private static int remainingNotice(Minecraft mc, FrameNoticeCapture notice) {
		return notice == null ? 0 : Math.max(0, Math.min(notice.remainingMillis(currentContext(mc), System.nanoTime()),
			RecoveryNotice.remainingTtlMs(mc)));
	}

	static int layerFlags(boolean passive, boolean hand) { return layerFlags(passive, hand, 0); }
	static int layerFlags(boolean passive, boolean hand, int noticeTtl) { return passive ? noticeTtl > 0 ? 11 : 1 : hand ? 7 : 3; }
}
