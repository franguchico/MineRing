package dev.ermc.bridge.link;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashSet;
import java.util.Set;

/** Single Java writer, native ACK writer; explicit commands never use H_MC_DEATHS. */
public final class ExplicitResetMailbox {
    public static final int OFFSET = 0xD00, SIZE = 64, VERSION = 1;
    public static final int SEQ = 0, SCHEMA = 4, MC_START = 8, HOST_START = 16,
        CONNECTION = 24, EVENT = 32, LIFE = 40, REASON = 44, ISSUED_AT = 48, ACK = 56;
    public static final int INACTIVE = 0, GENERIC_KILL = 1, CONSUMED = 1, REJECTED = 2;
    public static final long MAX_AGE_MS = 2000;
    private static final VarHandle INT = MethodHandles.byteBufferViewVarHandle(int[].class, ByteOrder.LITTLE_ENDIAN);
    private static final VarHandle LONG = MethodHandles.byteBufferViewVarHandle(long[].class, ByteOrder.LITTLE_ENDIAN);
    private final ByteBuffer buffer;
    private long mcStart, hostStart, connection, event;
    private boolean bindingAccepted, exhausted;
    private final Set<Long> retired = new HashSet<>();
    private static final int MAX_RETIRED = 256;
    private int life, stamp;

    public ExplicitResetMailbox(ByteBuffer buffer) {
        if (!buffer.isDirect() || buffer.isReadOnly() || buffer.capacity() < OFFSET + SIZE)
            throw new IllegalArgumentException("Writable aligned direct bridge buffer required");
        this.buffer = buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN);
    }

    /** Capture process identities before this connection can send its first native snapshot. */
    public synchronized boolean bind(long mc, long host, long epoch, long nowMs) {
        if (mc <= 0 || host <= 0 || epoch <= 0 || nowMs <= 0) return false;
        if (epoch == connection) {
            if (mc != mcStart || host != hostStart) return false;
            if (event == 0 && !bound(epoch)) {
                long ack = (long) LONG.getAcquire(buffer, OFFSET + ACK);
                long issued = buffer.getLong(OFFSET + ISSUED_AT);
                boolean rejected = (int) ack == stamp && (int) (ack >>> 32) == REJECTED;
                // Binding carries no lethal event. Delayed native startup or clock
                // rollback may retry it; explicit command stamps never renew.
                if (rejected || nowMs < issued || nowMs - issued > MAX_AGE_MS) {
                    int retry = write(mc, host, epoch, 0, 0, INACTIVE, nowMs);
                    if (retry == 0) return false;
                    stamp = retry;
                }
            }
            return true;
        }
        // Server epochs are identities: they need not be ordered across server restarts.
        if (retired.contains(epoch) || retired.size() >= MAX_RETIRED - (connection != 0 ? 1 : 0)) return false;
        int published = write(mc, host, epoch, 0, 0, INACTIVE, nowMs);
        if (published == 0) return false;
        if (connection != 0) retired.add(connection);
        mcStart = mc; hostStart = host; connection = epoch;
        bindingAccepted = false;
        event = 0; life = 0; stamp = published;
        return true;
    }

    /** Native must observe and ACK the binding before the client publishes any snapshot. */
    public synchronized boolean bound(long epoch) {
        if (epoch != connection || epoch <= 0) return false;
        if (bindingAccepted) return true;
        if (event != 0 || (int) INT.getAcquire(buffer, OFFSET + SEQ) != stamp
            || buffer.getInt(OFFSET + REASON) != INACTIVE || buffer.getInt(OFFSET + SCHEMA) != VERSION
            || buffer.getLong(OFFSET + MC_START) != mcStart || buffer.getLong(OFFSET + HOST_START) != hostStart
            || buffer.getLong(OFFSET + CONNECTION) != connection || buffer.getLong(OFFSET + EVENT) != 0) return false;
        long ack = (long) LONG.getAcquire(buffer, OFFSET + ACK);
        bindingAccepted = (int) ack == stamp && (int) (ack >>> 32) == CONSUMED;
        return bindingAccepted;
    }

    /** Repeated delivery returns success without republishing or renewing the native deadline. */
    public synchronized boolean publish(long mc, long host, long epoch, long request, int targetLife, long nowMs) {
        if (epoch != connection || epoch <= 0 || !bound(epoch) || mc != mcStart || host != hostStart
            || request <= 0 || request >= (1L << 52) || targetLife < 0 || nowMs <= 0) return false;
        if (request == event) return targetLife == life && currentRequest();
        if (request < event || (event != 0 && targetLife == life && !acknowledged(epoch, event))) return false;
        int published = write(mc, host, epoch, request, targetLife, GENERIC_KILL, nowMs);
        if (published == 0) return false;
        event = request; life = targetLife; stamp = published;
        return true;
    }

    /** A native rejection is terminal too: it must never cause a reissued command. */
    public synchronized boolean acknowledged(long epoch, long request) {
        if (epoch != connection || request <= 0 || request != event || !currentRequest()) return false;
        long ack = (long) LONG.getAcquire(buffer, OFFSET + ACK);
        int result = (int) (ack >>> 32);
        return (int) ack == stamp && (result == CONSUMED || result == REJECTED);
    }

    public synchronized void clear(long mc, long host, long nowMs) {
        if (write(mc, host, 0, 0, 0, INACTIVE, nowMs) == 0)
            INT.setRelease(buffer, OFFSET + SEQ, 0); // invalidate even after stamp exhaustion
        if (connection != 0) retired.add(connection);
        connection = event = 0; stamp = 0;
        bindingAccepted = false;
    }

    private boolean currentRequest() {
        return stamp != 0 && (int) INT.getAcquire(buffer, OFFSET + SEQ) == stamp
            && buffer.getLong(OFFSET + MC_START) == mcStart && buffer.getLong(OFFSET + HOST_START) == hostStart
            && buffer.getLong(OFFSET + CONNECTION) == connection && buffer.getLong(OFFSET + EVENT) == event
            && buffer.getInt(OFFSET + LIFE) == life && buffer.getInt(OFFSET + REASON) == GENERIC_KILL;
    }

    private int write(long mc, long host, long epoch, long request, int targetLife, int reason, long nowMs) {
        if (exhausted) return 0;
        int previous = (int) INT.getAcquire(buffer, OFFSET + SEQ);
        // Never wrap/reuse an ACK stamp, even across Minecraft reconnects.
        if (previous < 0 || previous >= Integer.MAX_VALUE - 2) { exhausted = true; return 0; }
        int next = (previous & ~1) + 2;
        INT.setOpaque(buffer, OFFSET + SEQ, next - 1);
        VarHandle.releaseFence();
        buffer.putInt(OFFSET + SCHEMA, VERSION);
        buffer.putLong(OFFSET + MC_START, mc);
        buffer.putLong(OFFSET + HOST_START, host);
        buffer.putLong(OFFSET + CONNECTION, epoch);
        buffer.putLong(OFFSET + EVENT, request);
        buffer.putInt(OFFSET + LIFE, targetLife);
        buffer.putInt(OFFSET + REASON, reason);
        buffer.putLong(OFFSET + ISSUED_AT, nowMs);
        INT.setRelease(buffer, OFFSET + SEQ, next);
        return next;
    }
}
