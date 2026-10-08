package dev.ermc.bridge.client;

import dev.ermc.bridge.CoordMap;
import dev.ermc.bridge.TerrainManager;
import dev.ermc.bridge.coop.CoopPayloads;
import dev.ermc.bridge.coop.CoopProtocol;
import dev.ermc.bridge.coop.CoopSession;
import dev.ermc.bridge.link.ErLink;
import dev.ermc.bridge.link.GameState;
import dev.ermc.bridge.link.Protocol;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.Minecraft;
import net.minecraft.network.chat.Component;
import org.slf4j.LoggerFactory;

/** Each machine talks only to its own ER mmap. Shared mapping arrives from the MC server. */
public final class CoopClient {
    private CoopClient() {}
    private static final GameState STATE = new GameState();
    private static final float[] EVENTS = new float[9];
    private static long epoch, sequence, recall = 1, deathAck, resample, lastSend, lastAck, settledAt, recalled;
    private static int previousLife = -1, previousDeaths = -1, ticks;
    private static float previousHits = -1, previousDamage = -1, previousStatus = -1;
    private static CoopPayloads.Ack ack;
    private static final CoopPublishGate PUBLISH = new CoopPublishGate();
    private static final java.util.Map<Long, Long> RESET_SAMPLES = new java.util.LinkedHashMap<>();
    private static long resetPublished;
    public static void register() {
        ClientPlayConnectionEvents.JOIN.register((handler, sender, mc) -> { reset(); PUBLISH.joined(); });
        ClientPlayConnectionEvents.DISCONNECT.register((handler, mc) -> {
            reset(); PUBLISH.disconnected(); CameraSync.disconnected();
            if (CoopSession.guest()) CoordMap.set(null);
        });
        ClientPlayNetworking.registerGlobalReceiver(CoopPayloads.Ack.TYPE, (p, c) -> receive(c.client(), p));
        ClientPlayNetworking.registerGlobalReceiver(CoopPayloads.Handshake.TYPE, (p, c) -> {
            var hello = p.value();
            // Only this original-layout bootstrap packet can establish an epoch.
            if (hello.version() != CoopProtocol.VERSION || (epoch == 0 && hello.sequence() == 0
                && hello.status() == CoopProtocol.HANDSHAKE && hello.deathSequence() == 0)) receive(c.client(), hello);
        });
    }
    private static void reset() {
        ErLink.get().clearExplicitReset();
        RESET_SAMPLES.clear();
        resetPublished = 0;
        CoopModeClient.reset();
        epoch = sequence = deathAck = resample = lastSend = lastAck = settledAt = recalled = 0;
        recall = 1; previousLife = previousDeaths = -1; ack = null; ticks = 0;
        previousHits = previousDamage = previousStatus = -1;
        java.util.Arrays.fill(EVENTS, 0);
    }
    private static void disconnect(Minecraft mc, String reason) {
        ErLink.get().clearExplicitReset();
        CameraSync.disconnected();
        if (mc.getConnection() != null) mc.getConnection().getConnection().disconnect(Component.literal(reason));
    }
    private static void receive(Minecraft mc, CoopPayloads.Ack p) {
        if (!CoopSession.enabled() || p.version() != CoopProtocol.VERSION || p.epoch() <= 0
            || p.status() < 0 || p.status() > CoopProtocol.MAX_STATUS
            || !CoopProtocol.counter(p.sequence()) || !CoopProtocol.counter(p.recalled())
            || !CoopProtocol.counter(p.clientRecall()) || !CoopProtocol.counter(p.deathSequence())
            || p.deathLife() < 0 || (p.deathKind() != CoopPayloads.PASSIVE_DEATH && p.deathKind() != CoopPayloads.EXPLICIT_RESET)
            || (p.deathSequence() == 0 && p.deathKind() != CoopPayloads.PASSIVE_DEATH)
            || !CoopProtocol.counter(p.deathSample()) || p.deathSample() > p.sequence()
            || (p.deathKind() == CoopPayloads.EXPLICIT_RESET && p.deathSample() == 0)
            || (p.mapping() != null && !CoordMap.validShared(p.mapping()))) {
            disconnect(mc, "ER Bridge: protocolo co-op incompatível ou inválido"); return;
        }
        if (epoch != 0 && epoch != p.epoch()) {
            disconnect(mc, "ER Bridge: sessão mudou; reconecte ao servidor"); return;
        }
        if (p.sequence() > sequence || (ack != null && (p.sequence() < ack.sequence()
            || p.recalled() < ack.recalled() || p.deathSequence() < ack.deathSequence()))) return;
        if (ack != null && p.deathSequence() == ack.deathSequence()
            && (p.deathLife() != ack.deathLife() || p.deathKind() != ack.deathKind() || p.deathSample() != ack.deathSample())) return;
        if (ack == null || ack.status() != p.status()) {
            LoggerFactory.getLogger("erbridge").info("[coop-state] status={} reason={} recall={}/{} placement={}",
                p.status(), CoopProtocol.reason(p.status()), p.clientRecall(), recall, p.recalled());
        }
        epoch = p.epoch(); ack = p; lastAck = System.nanoTime();
        if (CoopSession.guest()) CoordMap.set(p.mapping()); // separate JVM; never overwrite the integrated host's map
        if (recalled != p.recalled()) { recalled = p.recalled(); settledAt = System.nanoTime(); }
    }
    public static boolean ready(GameState local) {
        return ack != null && ack.status() == CoopProtocol.READY && ack.clientRecall() == recall
            && ack.epoch() == epoch && System.nanoTime() >= lastAck
            && System.nanoTime() - lastAck <= CoopProtocol.FRESH_NANOS && recalled != 0
            && System.nanoTime() - settledAt >= 400_000_000L && ack.mapping() != null
            && ack.mapping().zone() == local.stageId && local.has(Protocol.STATE_PLAYER_VALID)
            && !local.has(Protocol.STATE_HOST_BUSY) && !local.has(Protocol.STATE_PLAYER_DEAD);
    }
    /** Effective status includes local recall/settling; a previous READY is never a new lease. */
    static int recoveryStatus() {
        long now = System.nanoTime();
        if (ack == null || epoch == 0 || ack.epoch() != epoch) return CoopProtocol.HANDSHAKE;
        if (now < lastAck || now - lastAck > CoopProtocol.FRESH_NANOS) return CoopProtocol.HOST_UNAVAILABLE;
        if (ack.status() != CoopProtocol.READY) return ack.status();
        if (ack.clientRecall() != recall || recalled == 0 || now < settledAt || now - settledAt < 400_000_000L)
            return CoopProtocol.RECALL_PENDING;
        return CoopProtocol.READY;
    }
    static boolean returnBlocked() {
        if (!CoopSession.enabled()) return false;
        int status = recoveryStatus();
        return status == CoopProtocol.HOST_UNAVAILABLE || status == CoopProtocol.PEER_UNAVAILABLE
            || status == CoopProtocol.OTHER_ZONE || status == CoopProtocol.TOO_FAR
            || status == CoopProtocol.UNSUPPORTED_GROUND;
    }
    static String recoveryReason(GameState local) {
        if (local.has(Protocol.STATE_PLAYER_DEAD)) return "Elden Ring: aguardando renascimento";
        if (local.has(Protocol.STATE_HOST_BUSY)) return "Elden Ring: cena, menu ou carregamento em andamento";
        int status = recoveryStatus();
        return CoopSession.enabled() && status != CoopProtocol.READY ? CoopProtocol.reason(status)
            : "ER Bridge: aguardando posicionamento seguro do Minecraft";
    }
    /** A remote client has no TerrainManager server ticks; its placement completes through ACKs. */
    public static boolean placementReady(GameState local) {
        return CoopSession.enabled() ? ready(local) : TerrainManager.recallSettled(400);
    }
    static long recallGeneration() { return recall; }
    static long connectionEpoch() { return epoch; }
    public static void requestRecall() {
        if (CoopSession.enabled()) { recall++; settledAt = System.nanoTime(); }
        else TerrainManager.requestRecall();
    }
    public static void resample() { if (CoopSession.enabled()) CoopSession.clientTerrainChanged(); }
    public static void tick(Minecraft mc) {
        if (!CoopSession.enabled()) return;
        publish(mc);
        if (mc.player == null || mc.getConnection() == null) return;
        ticks++;
        if (ticks % 20 == 0 && (ack == null || ack.status() != CoopProtocol.READY || System.nanoTime() - lastAck > CoopProtocol.FRESH_NANOS)) {
            int status = ack == null ? CoopProtocol.HANDSHAKE : System.nanoTime() - lastAck > CoopProtocol.FRESH_NANOS
                ? CoopProtocol.HOST_UNAVAILABLE : ack.status();
            mc.player.displayClientMessage(Component.literal(CoopProtocol.reason(status) + " · F8: controlar ER"), true);
        }
        if (epoch == 0 || !ClientPlayNetworking.canSend(CoopPayloads.Snapshot.TYPE)
            || System.nanoTime() - lastSend < 100_000_000L) return;
        ErLink link = ErLink.get();
        boolean have = link.poll() && link.snapshot(STATE);
        if (have && !link.bindExplicitReset(epoch)) {
            disconnect(mc, "ER Bridge: identidade do Elden Ring mudou. Reconecte ao mundo co-op."); return;
        }
        if (have && !link.explicitResetBound(epoch)) return;
        int life = have ? link.hostLife() : Math.max(0, previousLife);
        int deaths = have ? link.hostDeaths() : Math.max(0, previousDeaths);
        if (have && (life < previousLife || deaths < previousDeaths)) {
            disconnect(mc, "ER Bridge: Elden Ring reiniciou. Reconecte ao mundo co-op para iniciar uma nova sessão."); return;
        }
        if (have && (!link.readHunterEvents(EVENTS) || link.hostLife() != life)) return;
        if (have && life == previousLife && (EVENTS[0] < previousHits || EVENTS[1] < previousDamage || EVENTS[8] < previousStatus)) {
            disconnect(mc, "ER Bridge: contadores locais reiniciaram. Reconecte ao mundo co-op."); return;
        }
        if (have) {
            previousLife = life; previousDeaths = deaths;
            previousHits = EVENTS[0]; previousDamage = EVENTS[1]; previousStatus = EVENTS[8];
        }
        if (have) deliverDeath(link, life, System.nanoTime());
        ClientPlayNetworking.send(new CoopPayloads.Snapshot(CoopProtocol.VERSION, true, epoch, ++sequence,
            have ? STATE.flags : 0, have ? STATE.stageId : 0, life, deaths,
            have ? STATE.playerPos[0] : 0, have ? STATE.playerPos[1] : 0, have ? STATE.playerPos[2] : 0,
            EVENTS, recall, deathAck, CoopSession.clientTerrainGeneration()));
        lastSend = System.nanoTime();
        if (have) {
            RESET_SAMPLES.put(sequence, lastSend);
            if (RESET_SAMPLES.size() > 32) RESET_SAMPLES.remove(RESET_SAMPLES.keySet().iterator().next());
        }
    }
    /** Actual tick delivery step, isolated from Minecraft's transport and renderer. */
    static void deliverDeath(ErLink link, int life, long now) {
        if (ack != null && ack.deathSequence() > deathAck) {
            // Reliable delivery, but never deliver an old-life death after ER has respawned/warped.
            if (ack.deathLife() != life || STATE.has(Protocol.STATE_PLAYER_DEAD)) deathAck = ack.deathSequence();
            else if (ack.deathKind() == CoopPayloads.EXPLICIT_RESET) {
                // Retries never renew the mailbox deadline. Native rejection is terminal too.
                if (link.explicitResetAcknowledged(epoch, ack.deathSequence())) deathAck = ack.deathSequence();
                else if (resetPublished != ack.deathSequence()) {
                    long issued = RESET_SAMPLES.getOrDefault(ack.deathSample(), Long.MIN_VALUE);
                    if (issued == Long.MIN_VALUE || now < issued || now - issued > CoopProtocol.FRESH_NANOS)
                        deathAck = ack.deathSequence();
                    else if (now >= lastAck && now - lastAck <= CoopProtocol.FRESH_NANOS && STATE.has(Protocol.STATE_PLAYER_VALID))
                        if (link.writeExplicitReset(epoch, ack.deathSequence(), life)) resetPublished = ack.deathSequence();
                }
            } else {
                link.bumpMcDeaths(); deathAck = ack.deathSequence();
            }
        }
    }
    private static void publish(Minecraft mc) {
        var server = mc.getSingleplayerServer();
        var connection = mc.getConnection();
        if (CoopSession.guest() || server == null || !TerrainManager.isBridgeWorld()
            || connection == null || !connection.getConnection().isConnected()) return;
        var request = PUBLISH.begin(server, connection, mc.player, mc.level,
            CoopSession.isHostAuthority(server), server.isPublished());
        if (request == null) return;
        server.execute(() -> {
            try {
                // Only the server thread touches its whitelist/player-list preparation.
                CoopSession.secure(server);
            } catch (RuntimeException ex) {
                mc.execute(() -> { if (PUBLISH.fail(request)) LoggerFactory.getLogger("erbridge").error("Co-op preparation refused", ex); });
                return;
            }
            mc.execute(() -> {
                // Vanilla's Share to LAN screen invokes publishServer on this thread; it reads mc.player.
                boolean connected = mc.getConnection() == connection && connection.getConnection().isConnected();
                if (!PUBLISH.commit(request, mc.getSingleplayerServer(), connected ? connection : null,
                    mc.player, mc.level, CoopSession.isHostAuthority(server), server.isPublished())) return;
                try {
                    boolean ok = server.publishServer(server.getDefaultGameType(), false, 25565);
                    LoggerFactory.getLogger("erbridge").info("Co-op LAN publish: {} (online auth, whitelist, max 2, port 25565)", ok);
                    if (!ok) mc.player.displayClientMessage(Component.literal(
                        "ER Bridge: porta 25565 indisponível; servidor co-op não publicado"), false);
                } catch (RuntimeException ex) {
                    LoggerFactory.getLogger("erbridge").error("Co-op publish refused", ex);
                }
            });
        });
    }

    /** Read-only diagnostics; invoked on the client thread by dev command coopstatus. */
    public static void logStatus(Minecraft mc) {
        long age = lastAck == 0 ? -1 : (System.nanoTime() - lastAck) / 1_000_000;
        int status = ack == null ? CoopProtocol.HANDSHAKE : age > CoopProtocol.FRESH_NANOS / 1_000_000
            ? CoopProtocol.HOST_UNAVAILABLE : ack.status();
        LoggerFactory.getLogger("erbridge").info(
            "[devcmd] coopstatus client enabled={} role={} connected={} publish={} published={} epoch={} sentSeq={} acceptedSeq={} ackStatus={} effectiveStatus={} reason={} ackAgeMs={} recall={}/{} recalled={} map={}",
            CoopSession.enabled(), CoopSession.guest() ? "guest" : "host", mc.getConnection() != null,
            PUBLISH.status(), mc.getSingleplayerServer() != null && mc.getSingleplayerServer().isPublished(),
            epoch, sequence, ack == null ? 0 : ack.sequence(), ack == null ? -1 : ack.status(), status,
            CoopProtocol.reason(status), age, ack == null ? 0 : ack.clientRecall(), recall, recalled,
            ack == null ? null : ack.mapping());
        var server = mc.getSingleplayerServer();
        if (server != null) server.execute(() -> CoopSession.logStatus(server));
        else LoggerFactory.getLogger("erbridge").info("[devcmd] coopstatus server: remote or absent; use coopstatus on host for auth/whitelist");
    }
}
