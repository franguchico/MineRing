import dev.ermc.bridge.link.BridgeShm;
import dev.ermc.bridge.link.ControlState;
import dev.ermc.bridge.link.GameState;
import dev.ermc.bridge.link.Protocol;

public final class IpcClientProbe {
    public static void main(String[] args) throws Exception {
        BridgeShm shm = BridgeShm.open();
        GameState state = new GameState();
        if (shm.hostHeartbeat() != 987654321 || !shm.readState(state) || state.frame != 12345 ||
            state.camPos[0] != 10.5f || state.camPos[1] != -2.25f || state.camPos[2] != 100.f) {
            throw new AssertionError("C++ state/header ABI mismatch");
        }
        ControlState control = new ControlState();
        control.mcFrame = 424242;
        control.camPos[0] = 3.25f;
        control.hunterPos[2] = -7.5f;
        shm.writeControl(control);
        if (!shm.pushDamage(0x1122334455667788L, 9.5f, 1, 2, 3, 0)) {
            throw new AssertionError("Damage queue is unexpectedly full");
        }
        int seq = shm.submitRays(new float[]{10, 20, 30, 10, -20, 30}, 1, 0, null);
        long deadline = System.nanoTime() + 10_000_000_000L;
        while (!shm.raysDone(seq) && System.nanoTime() < deadline) Thread.sleep(1);
        if (!shm.raysDone(seq)) throw new AssertionError("No native ray reply");
        float[] hits = new float[6]; int[] hit = new int[1], attr = new int[1];
        shm.readHits(1, hits, hit, attr);
        if (hit[0] != 1 || attr[0] != 42 || hits[0] != 4.5f || hits[1] != 5.5f || hits[4] != 1.0f) {
            throw new AssertionError("C++ ray reply ABI mismatch");
        }
        shm.bumpMcHeartbeat(); shm.bumpMcHeartbeat();
        System.out.println("JAVA_IPC_PASS: " + Protocol.SHM_PATH);
    }
}
