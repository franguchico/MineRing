package dev.ermc.bridge.coop;

/** Small, fixed-size protocol. Local native addresses and entity IDs never cross the network. */
public final class CoopProtocol {
    private CoopProtocol() {}
    // v4 distinguishes explicit resets from ordinary deaths. Both endpoints must
    // agree before exchanging the extended acknowledgement and lifecycle rules.
    public static final int VERSION = 4;
    public static final int MAX_PLAYERS = 2;
    public static final double RANGE_METERS = 64;
    public static final long FRESH_NANOS = 1_500_000_000L;
    public static final long MAX_SEQUENCE = 1L << 52;
    public static final int READY = 0, HANDSHAKE = 1, HOST_UNAVAILABLE = 2,
        PEER_UNAVAILABLE = 3, OTHER_ZONE = 4, TOO_FAR = 5, RECALL_PENDING = 6, GROUND_PENDING = 7,
        UNSUPPORTED_GROUND = 8;
    public static final int MAX_STATUS = UNSUPPORTED_GROUND;

    public static boolean coordinate(double n) { return Double.isFinite(n) && Math.abs(n) <= 30_000_000; }
    public static boolean counter(long n) { return n >= 0 && n < MAX_SEQUENCE; }
    public static boolean valid(PeerState p) {
        if (p == null || p.connectionEpoch() <= 0 || !counter(p.sequence()) || p.sequence() == 0
            || (p.flags() & ~1023) != 0 || p.hostLife() < 0 || p.hostDeaths() < 0
            || !coordinate(p.hostX()) || !coordinate(p.hostY()) || !coordinate(p.hostZ())) return false;
        float[] e = p.hunterEvents();
        if (e.length != 9) return false;
        for (float v : e) if (!Float.isFinite(v)) return false;
        return e[0] >= 0 && e[0] <= 16_777_216 && e[0] == Math.floor(e[0])
            && e[1] >= 0 && e[1] <= 1e12 && e[2] >= 0 && e[2] <= 1e9
            && coordinate(e[3]) && coordinate(e[4]) && coordinate(e[5])
            && e[6] >= 0 && e[6] <= 1e9 && e[7] >= 0 && e[7] <= 1e9
            && e[8] >= 0 && e[8] <= 1e12;
    }
    public static boolean usable(PeerState p) {
        return p != null && (p.flags() & 2) != 0 && (p.flags() & (128 | 256)) == 0
            && p.stageId() != 0 && p.stageId() != -1;
    }
    public static int spatialStatus(PeerState host, PeerState peer, double unitsPerMeter) {
        if (!usable(host)) return HOST_UNAVAILABLE;
        if (!usable(peer)) return PEER_UNAVAILABLE;
        if (host.stageId() != peer.stageId()) return OTHER_ZONE;
        if (!Double.isFinite(unitsPerMeter) || unitsPerMeter < .001 || unitsPerMeter > 10000) return HOST_UNAVAILABLE;
        double dx = (peer.hostX() - host.hostX()) / unitsPerMeter;
        double dy = (peer.hostY() - host.hostY()) / unitsPerMeter;
        double dz = (peer.hostZ() - host.hostZ()) / unitsPerMeter;
        return dx * dx + dy * dy + dz * dz <= RANGE_METERS * RANGE_METERS ? READY : TOO_FAR;
    }
    public static String reason(int status) {
        return switch (status) {
            case READY -> "Co-op: conectado";
            case HANDSHAKE -> "Co-op: aguardando protocolo ER Bridge";
            case HOST_UNAVAILABLE -> "Co-op: aguardando Elden Ring do anfitrião";
            case PEER_UNAVAILABLE -> "Co-op: aguardando seu Elden Ring";
            case OTHER_ZONE -> "Co-op: volte à mesma zona do anfitrião";
            case TOO_FAR -> "Co-op: aproxime-se do anfitrião (limite 64 m)";
            case GROUND_PENDING -> "Co-op: medindo o terreno sob seus pés";
            case UNSUPPORTED_GROUND -> "Co-op: local sem chão válido; volte a uma área acessível no Elden Ring";
            default -> "Co-op: sincronizando sua posição";
        };
    }
}
