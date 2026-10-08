package dev.ermc.bridge.link;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Local frames.shm transport only; independent of bridge control and co-op network versions. */
public final class FrameProtocol {
    private FrameProtocol() {}

    public static final int MAGIC = 0x524D484D;
    public static final int VERSION = 3;
    public static final int MAX_W = 2560;
    // The requested window is 2560x1440; allow the observed extra framebuffer row on Windows.
    public static final int MAX_H = 1441;
    public static final int SLOTS = 3;
    public static final int HDR = 0x100;
    public static final int FILE_HDR = 0x1000;
    public static final long SLOT_SIZE = HDR + (long) MAX_W * MAX_H * 16;
    public static final long FILE_SIZE = FILE_HDR + SLOTS * SLOT_SIZE;
    private static final VarHandle INT = MethodHandles.byteBufferViewVarHandle(int[].class, ByteOrder.LITTLE_ENDIAN);

    public static boolean fits(int width, int height) {
        return width > 0 && height > 0 && width <= MAX_W && height <= MAX_H;
    }

    public static long layerBytes(int width, int height) {
        if (!fits(width, height)) throw new IllegalArgumentException("Frame dimensions exceed the local transport");
        return (long) width * height * 4;
    }

    public static int slotOffset(int slot) {
        if (slot < 0 || slot >= SLOTS) throw new IllegalArgumentException("Invalid frame slot");
        return Math.toIntExact(FILE_HDR + slot * SLOT_SIZE);
    }

    /** Invalidate all frame metadata before publishing a fresh layout, retaining pixel storage. */
    public static void initialize(ByteBuffer buffer) {
        if (buffer.capacity() < FILE_SIZE) throw new IllegalArgumentException("Frame mapping is too small");
        ByteBuffer data = buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN);
        INT.setRelease(data, 0, 0);
        // A full fence prevents metadata writes from becoming visible before invalidation.
        VarHandle.fullFence();
        for (int offset = 4; offset < FILE_HDR; offset += 4) data.putInt(offset, 0);
        for (int slot = 0; slot < SLOTS; slot++) {
            int base = slotOffset(slot);
            for (int offset = 0; offset < HDR; offset += 4) data.putInt(base + offset, 0);
        }
        data.putInt(4, VERSION);
        INT.setRelease(data, 0, MAGIC);
    }
}
