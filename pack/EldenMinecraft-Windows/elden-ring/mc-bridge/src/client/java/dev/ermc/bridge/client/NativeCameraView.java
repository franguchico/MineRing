package dev.ermc.bridge.client;

import dev.ermc.bridge.CoordMap;
import dev.ermc.bridge.link.ControlState;
import dev.ermc.bridge.link.GameState;
import dev.ermc.bridge.link.Protocol;
import net.minecraft.world.phys.Vec3;
import org.joml.Matrix3f;
import org.joml.Quaternionf;
import org.joml.Vector3f;

/** Validated native view, including camera roll. Does not move either game's player. */
final class NativeCameraView {
    private NativeCameraView() {}
    record View(Vec3 position, Quaternionf rotation, float yaw, float pitch, float fov, float aspect) {}

    static View read(GameState state, CoordMap.Mapping map) {
        if (map == null || map.provisional() || map.zone() != state.stageId
                || !Double.isFinite(map.unitsPerMeter()) || map.unitsPerMeter() <= 0
                || !state.has(Protocol.STATE_CAMERA_VALID) || !state.has(Protocol.STATE_PLAYER_VALID)
                || state.has(Protocol.STATE_PLAYER_DEAD) || state.has(Protocol.STATE_HOST_BUSY)
                || !Float.isFinite(state.fovYDeg) || state.fovYDeg <= 5 || state.fovYDeg >= 170
                || !Float.isFinite(state.aspect) || state.aspect <= .1F || state.aspect >= 10) return null;
        for (int i = 0; i < 3; i++) {
            if (!Float.isFinite(state.camPos[i]) || !Float.isFinite(state.camTarget[i])
                    || !Float.isFinite(state.camUp[i])) return null;
        }
        Vec3 position = map.toMc(state.camPos[0], state.camPos[1], state.camPos[2]);
        if (!Double.isFinite(position.x) || !Double.isFinite(position.y) || !Double.isFinite(position.z)) return null;
        Vec3 direction = map.dirToMc((double) state.camTarget[0] - state.camPos[0],
            (double) state.camTarget[1] - state.camPos[1], (double) state.camTarget[2] - state.camPos[2]);
        Vec3 up = map.dirToMc(state.camUp[0], state.camUp[1], state.camUp[2]);
        if (direction.lengthSqr() < 1e-12 || up.lengthSqr() < 1e-12) return null;
        direction = direction.normalize();
        Vec3 right = direction.cross(up.normalize());
        if (right.lengthSqr() < 1e-10) return null;
        right = right.normalize();
        up = right.cross(direction).normalize();
        // Camera.rotation rotates vanilla's forward -Z, up +Y and screen-right +X.
        Matrix3f basis = new Matrix3f().setColumn(0, vector(right)).setColumn(1, vector(up))
            .setColumn(2, vector(direction.scale(-1)));
        Quaternionf rotation = new Quaternionf().setFromNormalized(basis).normalize();
        float yaw = (float) Math.toDegrees(Math.atan2(-direction.x, direction.z));
        float pitch = (float) Math.toDegrees(-Math.asin(Math.max(-1, Math.min(1, direction.y))));
        return new View(position, rotation, yaw, pitch, state.fovYDeg, state.aspect);
    }

    private static Vector3f vector(Vec3 v) { return new Vector3f((float) v.x, (float) v.y, (float) v.z); }

    static void capturedControl(GameState state, ControlState control) {
        control.flags = Protocol.CTRL_COMPOSITE | Protocol.CTRL_PASSIVE_COMPOSITE;
        control.poseLag = 0;
        control.fovYDeg = state.fovYDeg;
        System.arraycopy(state.camPos, 0, control.camPos, 0, 3);
        System.arraycopy(state.camTarget, 0, control.camTarget, 0, 3);
        System.arraycopy(state.camUp, 0, control.camUp, 0, 3);
        java.util.Arrays.fill(control.hunterPos, 0);
        control.hunterYawDeg = 0;
    }
}
