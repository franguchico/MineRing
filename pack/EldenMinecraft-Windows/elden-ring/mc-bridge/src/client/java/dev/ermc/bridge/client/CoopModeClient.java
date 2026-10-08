package dev.ermc.bridge.client;

import dev.ermc.bridge.coop.CoopModePayloads;
import dev.ermc.bridge.coop.CoopSession;
import dev.ermc.bridge.link.ErLink;
import dev.ermc.bridge.link.GameState;
import dev.ermc.bridge.link.NativePeerInfo;
import dev.ermc.bridge.link.Protocol;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.AbstractClientPlayer;
import net.minecraft.world.entity.Entity;

/** F8 mode is owned by the remote authenticated player, never inferred from a nearby body. */
public final class CoopModeClient {
    private CoopModeClient() {}
    private static final CoopModeView VIEW = new CoopModeView();
    private static final GameState STATE = new GameState();
    private static NativePeerInfo nativePeer = NativePeerInfo.EMPTY;
    private static long sendSequence, sendEpoch, lastSend;
    public static void register() {
        ClientPlayNetworking.registerGlobalReceiver(CoopModePayloads.ModeRoster.TYPE,
            (p, c) -> VIEW.accept(p, CoopClient.connectionEpoch(), System.nanoTime()));
    }
    public static void reset() {
        VIEW.reset(); nativePeer = NativePeerInfo.EMPTY; sendSequence = sendEpoch = lastSend = 0;
        ErLink.get().writePeerMode(0, CoopModePayloads.UNKNOWN, false, 0, 0);
    }
    public static void tick(Minecraft mc) {
        if (!CoopSession.enabled() || mc.player == null || mc.getConnection() == null
                || !mc.getConnection().getConnection().isConnected()) return;
        long epoch = CoopClient.connectionEpoch(), now = System.nanoTime();
        if (epoch <= 0 || !ClientPlayNetworking.canSend(CoopModePayloads.ModeSnapshot.TYPE)
                || now - lastSend < 100_000_000L) return;
        if (sendEpoch != epoch) { sendEpoch = epoch; sendSequence = 0; }
        ErLink link = ErLink.get();
        nativePeer = link.peerInfo();
		boolean localBody = link.alive() && link.snapshot(STATE) && STATE.has(Protocol.STATE_PLAYER_VALID);
		int mode = localMode(localBody, Overlay.hostMode(), Overlay.awaitingInput(), Overlay.active(),
			CameraSync.mode() == CameraSync.Mode.DRIVE_HOST, CameraSync.standInEnabled(),
			CameraSync.driving() || CameraSync.holding() || CameraSync.placementReadyForInput(mc, STATE));
        ClientPlayNetworking.send(new CoopModePayloads.ModeSnapshot(CoopModePayloads.VERSION, epoch,
            ++sendSequence, nativePeer.localValid() ? nativePeer.localSteamId() : 0, mode));
        lastSend = now;
	}
	static int localMode(boolean body, boolean hostMode, boolean pending, boolean overlay,
			boolean driveMode, boolean standIn, boolean acquiredOrReady) {
		if (!body) return CoopModePayloads.UNKNOWN;
		if (hostMode || pending) return CoopModePayloads.ELDEN_RING;
		return overlay && driveMode && standIn && acquiredOrReady ? CoopModePayloads.MINECRAFT : CoopModePayloads.UNKNOWN;
	}
	static void clearRenderProof() { ErLink.get().writePeerMode(0, CoopModePayloads.UNKNOWN, false, 0, 0); }
    /** F8 publishes the new choice immediately instead of waiting for the 10 Hz heartbeat. */
    static void localModeChanged(Minecraft mc) {
        lastSend = 0;
        tick(mc);
    }
    public static void onFrame(Minecraft mc) {
        if (!CoopSession.enabled()) return;
        ErLink link = ErLink.get();
        nativePeer = link.peerInfo();
        var remote = mc.player == null ? null : VIEW.remote(mc.player.getUUID(), CoopClient.connectionEpoch(), System.nanoTime());
        var avatar = remote == null || mc.level == null ? null : mc.level.getPlayerByUUID(remote.playerId());
        boolean present = remote != null && remote.steamId64() != 0 && mc.getConnection() != null
            && mc.getConnection().getConnection().isConnected() && avatar != null && avatar != mc.player
            && !avatar.isDeadOrDying() && !avatar.isSpectator() && !avatar.isInvisibleTo(mc.player)
            && nativePeer.matchesRemote(remote.steamId64());
		boolean render = present && FramePassthrough.hasPublishedWorld() && canRenderPeer(Overlay.active(), FramePassthrough.activeInHost(),
            Overlay.hostMode(), CameraSync.driving(), CameraSync.holding(), CameraSync.passive());
        link.writePeerMode(present ? remote.steamId64() : 0,
            present ? remote.mode() : CoopModePayloads.UNKNOWN, render,
            present ? remote.connectionEpoch() : 0, present ? VIEW.sequence() : 0);
    }
    static boolean canRenderPeer(boolean overlay, boolean composite, boolean hostMode,
            boolean driving, boolean holding, boolean passive) {
        return overlay && composite && (hostMode ? passive : driving || holding);
    }
    public static boolean freezeMinecraftBody(Entity entity) {
        Minecraft mc = Minecraft.getInstance();
		return freezeMinecraftBody(CoopSession.enabled(), Overlay.blockGameplayInput(), entity == mc.player,
            mc.level != null && entity.level() == mc.level && mc.getConnection() != null
                && mc.getConnection().getConnection().isConnected());
    }
	static boolean freezeMinecraftBody(boolean coop, boolean suspended, boolean local, boolean connected) {
		return coop && suspended && local && connected;
    }
    public static boolean hideMinecraft(Entity entity) {
        Minecraft mc = Minecraft.getInstance();
        // The local ER body is rendered natively in passive mode; never draw its old MC copy.
        if (Overlay.passive() && entity == mc.player) return true;
        if (!CoopSession.enabled() || !(entity instanceof AbstractClientPlayer) || entity == mc.player
                || mc.level == null || entity.level() != mc.level || !Overlay.active()
                || mc.getConnection() == null || !mc.getConnection().getConnection().isConnected()) return false;
        return CoopModeView.hideMinecraft(VIEW.peer(entity.getUUID(), CoopClient.connectionEpoch(), System.nanoTime()), nativePeer);
    }
}
