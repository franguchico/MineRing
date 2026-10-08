package dev.ermc.bridge.coop;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.minecraft.network.RegistryFriendlyByteBuf;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.ResourceLocation;

/** Optional additive v1 mode channels; legacy Snapshot/Ack remain unchanged. No native handles. */
public final class CoopModePayloads {
    private CoopModePayloads() {}
    public static final int VERSION = CoopModeLedger.VERSION, MAX_PLAYERS = CoopModeLedger.MAX_PLAYERS;
    public static final int UNKNOWN = CoopModeLedger.UNKNOWN, MINECRAFT = CoopModeLedger.MINECRAFT,
        ELDEN_RING = CoopModeLedger.ELDEN_RING;
    public static final int SNAPSHOT_BYTES = 32, ROSTER_HEADER_BYTES = 24, PEER_BYTES = 44;
    public static final int MAX_ROSTER_BYTES = ROSTER_HEADER_BYTES + MAX_PLAYERS * PEER_BYTES;

    /** Own mode only: the server supplies UUID from the authenticated ServerPlayer. */
    public record ModeSnapshot(int version, long epoch, long sequence, long steamId64, int mode)
            implements CustomPacketPayload {
        public ModeSnapshot {
            if (!CoopModeLedger.valid(version, epoch, sequence, steamId64, mode))
                throw new IllegalArgumentException("Invalid mode snapshot");
        }
        public static final Type<ModeSnapshot> TYPE = new Type<>(
            ResourceLocation.fromNamespaceAndPath("erbridge", "coop_mode_snapshot_v1"));
        public static final StreamCodec<RegistryFriendlyByteBuf, ModeSnapshot> CODEC = StreamCodec.of((b, p) -> {
            b.writeInt(p.version); b.writeLong(p.epoch); b.writeLong(p.sequence);
            b.writeLong(p.steamId64); b.writeInt(p.mode);
        }, b -> {
            if (b.readableBytes() != SNAPSHOT_BYTES) throw new IllegalArgumentException("Invalid mode snapshot length");
            return new ModeSnapshot(b.readInt(), b.readLong(), b.readLong(), b.readLong(), b.readInt());
        });
        @Override public Type<ModeSnapshot> type() { return TYPE; }
    }

    public record ModePeer(UUID playerId, long steamId64, int mode, long sequence, long connectionEpoch) {
        public ModePeer {
            if (playerId == null || !CoopModeLedger.valid(VERSION, connectionEpoch, sequence, steamId64, mode))
                throw new IllegalArgumentException("Invalid mode peer");
        }
    }

    /** epoch belongs to this recipient, sequence is the server's per-recipient roster sequence. */
    public record ModeRoster(int version, long epoch, long sequence, List<ModePeer> players)
            implements CustomPacketPayload {
        public ModeRoster {
            if (!CoopModeLedger.valid(version, epoch, sequence, 0, UNKNOWN)
                    || players == null || players.size() > MAX_PLAYERS)
                throw new IllegalArgumentException("Invalid mode roster");
            players = List.copyOf(players);
            if (players.size() == 2 && players.get(0).playerId().equals(players.get(1).playerId()))
                throw new IllegalArgumentException("Duplicate mode peer UUID");
        }
        public static final Type<ModeRoster> TYPE = new Type<>(
            ResourceLocation.fromNamespaceAndPath("erbridge", "coop_mode_roster_v1"));
        public static final StreamCodec<RegistryFriendlyByteBuf, ModeRoster> CODEC = StreamCodec.of((b, p) -> {
            b.writeInt(p.version); b.writeLong(p.epoch); b.writeLong(p.sequence); b.writeInt(p.players.size());
            for (ModePeer peer : p.players) {
                b.writeUUID(peer.playerId); b.writeLong(peer.steamId64); b.writeInt(peer.mode);
                b.writeLong(peer.sequence); b.writeLong(peer.connectionEpoch);
            }
        }, b -> {
            int length = b.readableBytes();
            if (length < ROSTER_HEADER_BYTES || length > MAX_ROSTER_BYTES)
                throw new IllegalArgumentException("Invalid mode roster length");
            int version = b.readInt(); long epoch = b.readLong(), sequence = b.readLong(); int count = b.readInt();
            // Check count and exact remaining length before allocating; never allocate from an untrusted count.
            if (count < 0 || count > MAX_PLAYERS || b.readableBytes() != count * PEER_BYTES
                    || !CoopModeLedger.valid(version, epoch, sequence, 0, UNKNOWN))
                throw new IllegalArgumentException("Invalid mode roster header");
            var players = new ArrayList<ModePeer>(count);
            for (int i = 0; i < count; i++)
                players.add(new ModePeer(b.readUUID(), b.readLong(), b.readInt(), b.readLong(), b.readLong()));
            return new ModeRoster(version, epoch, sequence, players);
        });
        @Override public Type<ModeRoster> type() { return TYPE; }
    }

    public static void register() {
        PayloadTypeRegistry.playC2S().register(ModeSnapshot.TYPE, ModeSnapshot.CODEC);
        PayloadTypeRegistry.playS2C().register(ModeRoster.TYPE, ModeRoster.CODEC);
    }
}
