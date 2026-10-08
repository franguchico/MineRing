package dev.ermc.bridge.coop;

import dev.ermc.bridge.CoordMap;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.minecraft.network.RegistryFriendlyByteBuf;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.ResourceLocation;

public final class CoopPayloads {
    private CoopPayloads() {}
    public static final int PASSIVE_DEATH = PeerChannel.PASSIVE_DEATH, EXPLICIT_RESET = PeerChannel.EXPLICIT_RESET;
    public record Snapshot(int version, boolean enabled, long epoch, long sequence, int flags, int stage,
        int life, int deaths, double x, double y, double z, float[] events, long recall, long deathAck,
        long resample) implements CustomPacketPayload {
        public Snapshot {
            if (events.length != 9) throw new IllegalArgumentException("Expected nine hunter event scalars");
            events = events.clone();
        }
        @Override public float[] events() { return events.clone(); }
        public static final Type<Snapshot> TYPE = new Type<>(ResourceLocation.fromNamespaceAndPath("erbridge", "coop_snapshot_v1"));
        public static final StreamCodec<RegistryFriendlyByteBuf, Snapshot> CODEC = StreamCodec.of((b, p) -> {
            b.writeInt(p.version); b.writeBoolean(p.enabled); b.writeLong(p.epoch); b.writeLong(p.sequence);
            b.writeInt(p.flags); b.writeInt(p.stage); b.writeInt(p.life); b.writeInt(p.deaths);
            b.writeDouble(p.x); b.writeDouble(p.y); b.writeDouble(p.z);
            for (float v : p.events) b.writeFloat(v);
            b.writeLong(p.recall); b.writeLong(p.deathAck); b.writeLong(p.resample);
        }, b -> {
            int v = b.readInt(); boolean on = b.readBoolean(); long e = b.readLong(), s = b.readLong();
            int f = b.readInt(), zone = b.readInt(), life = b.readInt(), deaths = b.readInt();
            double x = b.readDouble(), y = b.readDouble(), z = b.readDouble();
            float[] events = new float[9]; for (int i = 0; i < 9; i++) events[i] = b.readFloat();
            return new Snapshot(v, on, e, s, f, zone, life, deaths, x, y, z, events, b.readLong(), b.readLong(), b.readLong());
        });
        @Override public Type<Snapshot> type() { return TYPE; }
        public PeerState peer(long now) { return new PeerState(epoch, sequence, flags, stage, life, deaths, x, y, z, events, now); }
    }
    public record Ack(int version, long epoch, long sequence, int status, CoordMap.Mapping mapping,
        long recalled, long clientRecall, long deathSequence, int deathLife, int deathKind, long deathSample) implements CustomPacketPayload {
        public Ack(int version, long epoch, long sequence, int status, CoordMap.Mapping mapping,
                long recalled, long clientRecall, long deathSequence, int deathLife) {
            this(version, epoch, sequence, status, mapping, recalled, clientRecall, deathSequence, deathLife, PASSIVE_DEATH, 0);
        }
        public static final Type<Ack> TYPE = new Type<>(ResourceLocation.fromNamespaceAndPath("erbridge", "coop_ack_v2"));
        public static final StreamCodec<RegistryFriendlyByteBuf, Ack> CODEC = StreamCodec.of(
            (b, p) -> writeAck(b, p, true), b -> readAck(b, true));
        @Override public Type<Ack> type() { return TYPE; }
    }
    /** Original layout only: a v3 client can read the version refusal before any v4 event. */
    public record Handshake(Ack value) implements CustomPacketPayload {
        public static final Type<Handshake> TYPE = new Type<>(ResourceLocation.fromNamespaceAndPath("erbridge", "coop_ack_v1"));
        public static final StreamCodec<RegistryFriendlyByteBuf, Handshake> CODEC = StreamCodec.of(
            (b, p) -> writeAck(b, p.value(), false), b -> new Handshake(readAck(b, false)));
        @Override public Type<Handshake> type() { return TYPE; }
    }
    private static void writeAck(RegistryFriendlyByteBuf b, Ack p, boolean withKind) {
            b.writeInt(p.version); b.writeLong(p.epoch); b.writeLong(p.sequence); b.writeInt(p.status);
            b.writeBoolean(p.mapping != null);
            if (p.mapping != null) {
                var m = p.mapping;
                b.writeDouble(m.ax()); b.writeDouble(m.ay()); b.writeDouble(m.az()); b.writeDouble(m.unitsPerMeter());
                b.writeBoolean(m.flipZ()); b.writeBoolean(m.provisional()); b.writeInt(m.zone()); b.writeInt(m.region());
            }
            b.writeLong(p.recalled); b.writeLong(p.clientRecall); b.writeLong(p.deathSequence); b.writeInt(p.deathLife);
            if (withKind) { b.writeInt(p.deathKind); b.writeLong(p.deathSample); }
    }
    private static Ack readAck(RegistryFriendlyByteBuf b, boolean withKind) {
            int v = b.readInt(); long e = b.readLong(), seq = b.readLong(); int status = b.readInt();
            CoordMap.Mapping m = b.readBoolean() ? new CoordMap.Mapping(b.readDouble(), b.readDouble(), b.readDouble(),
                b.readDouble(), b.readBoolean(), b.readBoolean(), b.readInt(), b.readInt()) : null;
            return new Ack(v, e, seq, status, m, b.readLong(), b.readLong(), b.readLong(), b.readInt(),
                withKind ? b.readInt() : PASSIVE_DEATH, withKind ? b.readLong() : 0);
    }
    public static void register() {
        PayloadTypeRegistry.playC2S().register(Snapshot.TYPE, Snapshot.CODEC);
        PayloadTypeRegistry.playS2C().register(Ack.TYPE, Ack.CODEC);
        PayloadTypeRegistry.playS2C().register(Handshake.TYPE, Handshake.CODEC);
    }
}
