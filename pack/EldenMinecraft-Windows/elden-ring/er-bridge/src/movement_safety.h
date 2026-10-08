#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// Pure geometry policy. No engine pointers, raycasts, IPC or remembered floor.
// Coordinates and normals must already be in the same, Y-up frame. The caller
// supplies collision observations; this helper never invents a normal. The
// current game raycast synthesizes ny=1 for a downward vertical probe: that is
// acceptable as a probe-direction contract, NOT evidence of a walkable slope.
// Use this policy only for a fresh strictly vertical ground probe, never for a
// general collision ray or a cached Java terrain response.
namespace mb {

// A continuously republished HOLD is not progress. Only a real MOVE ends this
// lease; a timed-out owner must hand control back rather than renew the freeze.
struct StandInHoldLease {
    static constexpr uint64_t limitMs = 2000;
    uint64_t startedAt = 0;
    bool started = false;
    void reset() { *this = {}; }
    bool expired(uint64_t now, bool hold) {
        if (!hold) { reset(); return false; }
        if (!started) { started = true; startedAt = now; }
        return now < startedAt || now - startedAt >= limitMs;
    }
};

constexpr float kGroundSweepStartLift = 0.10f;
constexpr float kGroundSweepMargin = 0.02f;
constexpr float kGroundSweepHitTolerance = 0.01f;

struct GroundSweepHit {
    float pos[3] = {};
    float normal[3] = {};
    bool hit = false;
};

struct GroundSweepResult {
    bool valid = false;
    bool clamped = false;
    float feetY = 0;
    double fraction = 1;
    std::size_t hitIndex = std::size_t(-1);
};

inline bool movement_position_finite(const float* p) {
    return p && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

// Reacquisition is a synchronization with the body's CURRENT pose, not a
// teleport to the last client's pose from another life/map. Ordinary driven
// movement and free flight are not distance-limited by this takeover policy.
inline bool movement_acquisition_near(const float* nativeFeet, const float* requestedFeet) {
    if (!movement_position_finite(nativeFeet) || !movement_position_finite(requestedFeet)) return false;
    double d2 = 0;
    for (int i = 0; i < 3; ++i) {
        double d = double(nativeFeet[i]) - requestedFeet[i];
        d2 += d * d;
    }
    return d2 <= 4.0;  // two metres accommodates a normal render/tick handoff
}

inline bool movement_recovery_near(const float* currentFeet, const float* anchorFeet) {
    if (!movement_position_finite(currentFeet) || !movement_position_finite(anchorFeet)) return false;
    double d2 = 0;
    for (int i = 0; i < 3; ++i) {
        double d = double(currentFeet[i]) - anchorFeet[i];
        d2 += d * d;
    }
    return d2 <= 64.0 * 64.0;  // no recovery across a distant/unloaded part of the map
}

constexpr float kPlacementSupportDepth = 1.0f;
inline bool prepare_placement_support(const float* feet, float* start, float* end) {
    if (!movement_position_finite(feet) || !start || !end) return false;
    for (int i = 0; i < 3; ++i) start[i] = end[i] = feet[i];
    start[1] += kGroundSweepStartLift;
    end[1] -= kPlacementSupportDepth;
    return movement_position_finite(start) && movement_position_finite(end) &&
        start[1] > feet[1] && end[1] < feet[1];
}
// This only proves nearby collision support, not Havok walkability or clearance.
// No normal is invented and no saved/cached terrain can satisfy the observation.
inline bool placement_support_hit(const float* feet, const float* hit) {
    return movement_position_finite(feet) && movement_position_finite(hit) &&
        std::fabs(double(hit[0]) - feet[0]) <= kGroundSweepHitTolerance &&
        std::fabs(double(hit[2]) - feet[2]) <= kGroundSweepHitTolerance &&
        double(hit[1]) <= double(feet[1]) + kGroundSweepHitTolerance &&
        double(feet[1]) - hit[1] <= kPlacementSupportDepth;
}

// A vertical segment at the REQUESTED X/Z tests the floor under the destination.
// A diagonal ray from the old X/Z could hold the player on the edge they just left.
// False means no sweep: invalid input, upward/level motion, or an unrepresentable
// lift. It does not authorize writing an invalid requested position.
inline bool prepare_ground_sweep(const float* previousFeet, const float* requestedFeet,
                                 float* start, float* end) {
    if (!start || !end || !movement_position_finite(previousFeet) ||
        !movement_position_finite(requestedFeet) || requestedFeet[1] >= previousFeet[1]) return false;
    const float previousY = previousFeet[1];
    const float x = requestedFeet[0], y = requestedFeet[1], z = requestedFeet[2];
    const float liftedY = previousY + kGroundSweepStartLift;
    if (!std::isfinite(liftedY) || liftedY <= previousY) return false;
    start[0] = x; start[1] = liftedY; start[2] = z;
    end[0] = x; end[1] = y; end[2] = z;
    return true;
}

// Select the first walkable crossing, independent of candidate order. Hits must
// lie on the segment (within a small absolute position tolerance), have a finite
// approximately unit normal with ny > 0.5, and be at/below the PREVIOUS feet.
// An inferred upward normal classifies the vertical probe only; this function
// cannot establish real Havok walkability without an actual surface normal.
// The result never raises the previous feet or snaps down to an un-crossed floor.
// For applying feetY at requested X/Z, use prepare_ground_sweep's vertical ray.
// Apply every valid clamp BEFORE accepting the new feet. A descent/correction
// size threshold can accept a small floor crossing; the next probe then sees
// that floor above its previous feet and correctly rejects an upward teleport.
// Notification thresholds belong after the safety clamp, not around it.
inline GroundSweepResult select_ground_crossing(const float* start, const float* end,
                                                float previousFeetY,
                                                const GroundSweepHit* hits, std::size_t count,
                                                float margin = kGroundSweepMargin,
                                                float hitTolerance = kGroundSweepHitTolerance) {
    GroundSweepResult result;
    if (end && std::isfinite(end[1])) result.feetY = end[1];
    if (!movement_position_finite(start) || !movement_position_finite(end) ||
        !std::isfinite(previousFeetY) || !std::isfinite(margin) ||
        !std::isfinite(hitTolerance) || margin < 0 || margin > kGroundSweepStartLift ||
        hitTolerance < 0 || hitTolerance > 0.05f || (count && !hits) ||
        start[0] != end[0] || start[2] != end[2]) return result;
    const double lift = double(start[1]) - previousFeetY;
    if (lift < 0 || lift > double(kGroundSweepStartLift) + hitTolerance) return result;
    result.valid = true;
    if (end[1] >= previousFeetY) return result;  // jumps and level movement are free
    const double dx = double(end[0]) - start[0], dy = double(end[1]) - start[1], dz = double(end[2]) - start[2];
    const double lengthSquared = dx * dx + dy * dy + dz * dz;
    if (!(lengthSquared > 0) || !(dy < 0)) { result.valid = false; return result; }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& hit = hits[i];
        if (!hit.hit || !movement_position_finite(hit.pos) || !movement_position_finite(hit.normal)) continue;
        // Reject overhead floors, hits beyond the desired descent, and malformed normals.
        if (hit.pos[1] > previousFeetY || hit.pos[1] < end[1] || hit.normal[1] <= 0.5f) continue;
        const double nx = hit.normal[0], ny = hit.normal[1], nz = hit.normal[2];
        const double normalSquared = nx * nx + ny * ny + nz * nz;
        if (normalSquared < 0.99 || normalSquared > 1.01) continue;
        const double hx = double(hit.pos[0]) - start[0], hy = double(hit.pos[1]) - start[1], hz = double(hit.pos[2]) - start[2];
        const double t = (hx * dx + hy * dy + hz * dz) / lengthSquared;
        if (t < 0 || t > 1 || (result.hitIndex != std::size_t(-1) && t >= result.fraction)) continue;
        const double ex = hx - t * dx, ey = hy - t * dy, ez = hz - t * dz;
        if (ex * ex + ey * ey + ez * ez > double(hitTolerance) * hitTolerance) continue;
        double landingY = double(hit.pos[1]) + margin;
        if (landingY > previousFeetY) landingY = previousFeetY;  // no upward correction, even for the margin
        result.feetY = float(landingY);
        result.clamped = result.feetY > end[1];
        result.fraction = t;
        result.hitIndex = i;
    }
    return result;
}

}  // namespace mb
