// Standalone geometry fixtures: compile this file only, no Windows/game libraries.
#include "../src/movement_safety.h"
#include <cstdio>
#include <limits>
#include <algorithm>
#include <vector>

static int failures = 0, checks = 0;
static void check(bool pass, const char* what) {
    ++checks;
    std::printf("%s: %s\n", pass ? "PASS" : "FAIL", what);
    if (!pass) ++failures;
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
static mb::GroundSweepHit floor(float x, float y, float z, float ny = 1) {
    return {{x, y, z}, {float(std::sqrt(1.0 - double(ny) * ny)), ny, 0}, true};
}
static mb::GroundSweepResult sweep(const float* previous, const float* requested,
                                   const mb::GroundSweepHit* hits, std::size_t count) {
    float start[3], end[3];
    if (!mb::prepare_ground_sweep(previous, requested, start, end)) return {};
    return mb::select_ground_crossing(start, end, previous[1], hits, count);
}

static void test_movement_cases() {
    float previous[] = {0, 1, 0}, requested[] = {2, -8, 3};
    float start[3] = {}, end[3] = {};
    check(mb::prepare_ground_sweep(previous, requested, start, end) &&
          near(start[0], 2) && near(start[1], 1.10f) && near(start[2], 3) &&
          near(end[0], 2) && near(end[1], -8) && near(end[2], 3),
          "destination-column sweep covers a delayed multi-metre descent");
    auto ground = floor(2, 0, 3);
    auto r = sweep(previous, requested, &ground, 1);
    check(r.valid && r.clamped && near(r.feetY, 0.02f) && r.hitIndex == 0,
          "late streamed floor prevents fall-through with a small landing margin");
    auto voidResult = sweep(previous, requested, nullptr, 0);
    check(voidResult.valid && !voidResult.clamped && near(voidResult.feetY, -8),
          "no observed floor allows a real void fall instead of inventing flat terrain");
    auto missed = ground; missed.hit = false;
    check(!sweep(previous, requested, &missed, 1).clamped, "ray miss cannot become a floor");

    auto overhead = floor(2, 1.05f, 3);
    r = sweep(previous, requested, &overhead, 1);
    check(r.valid && !r.clamped && near(r.feetY, -8),
          "floor inside the start lift but above old feet cannot teleport the player upward");
    auto atFeet = floor(2, 1, 3);
    r = sweep(previous, requested, &atFeet, 1);
    check(r.clamped && near(r.feetY, 1), "landing margin never raises the previous feet");
    auto farBelow = floor(2, -10, 3);
    check(!sweep(previous, requested, &farBelow, 1).clamped,
          "floor below the segment cannot snap the player down prematurely");

    float stepRequest[] = {2, 0.7f, 3};
    auto stepBelow = floor(2, 0.5f, 3);
    r = sweep(previous, stepRequest, &stepBelow, 1);
    check(r.valid && !r.clamped && near(r.feetY, 0.7f), "small drop stays airborne until it actually crosses the lower step");
    stepRequest[1] = 0.4f;
    r = sweep(previous, stepRequest, &stepBelow, 1);
    check(r.clamped && near(r.feetY, 0.52f), "descending onto a lower step lands below the old feet");
    auto slope = floor(2, 0.6f, 3, 0.8f);
    stepRequest[1] = 0.3f;
    r = sweep(previous, stepRequest, &slope, 1);
    check(r.clamped && near(r.feetY, 0.62f), "walkable descending slope uses its observed destination height");
    auto steep = floor(2, 0.6f, 3, 0.5f);
    check(!sweep(previous, stepRequest, &steep, 1).clamped, "normal ny exactly 0.5 is not walkable");
    auto justWalkable = floor(2, 0.6f, 3, 0.5001f);
    check(sweep(previous, stepRequest, &justWalkable, 1).clamped, "unit normal just above the threshold is walkable");
    auto wall = floor(2, 0.6f, 3, 0);
    check(!sweep(previous, stepRequest, &wall, 1).clamped, "wall is not a walking surface");
    auto ceiling = ground; ceiling.normal[1] = -1;
    check(!sweep(previous, requested, &ceiling, 1).clamped, "downward ceiling normal is rejected");

    float jump[] = {2, 2, 3};
    check(!mb::prepare_ground_sweep(previous, jump, start, end), "jump requests do not cast a ground-stop ray");
    float level[] = {2, 1, 3};
    check(!mb::prepare_ground_sweep(previous, level, start, end), "level and horizontal movement are not held by a prior floor");
    // Leaving a ledge: a hit under the OLD X/Z is not a hit under the destination.
    auto oldLedge = floor(0, 0, 0);
    check(!sweep(previous, requested, &oldLedge, 1).clamped,
          "leaving a ledge does not hover because an old-column hit was reused");
    auto endpoint = floor(2, -8, 3);
    r = sweep(previous, requested, &endpoint, 1);
    check(r.clamped && near(r.feetY, -7.98f) && r.fraction == 1,
          "contact at the segment endpoint receives the landing margin");

    std::vector<mb::GroundSweepHit> floors = {floor(2, -3, 3), overhead, wall, floor(2, 0.5f, 3)};
    r = sweep(previous, requested, floors.data(), floors.size());
    check(r.clamped && near(r.feetY, 0.52f) && r.hitIndex == 3,
          "nearest walkable crossing wins over later floors, overhead floors and walls");
    std::reverse(floors.begin(), floors.end());
    r = sweep(previous, requested, floors.data(), floors.size());
    check(r.clamped && near(r.feetY, 0.52f) && r.hitIndex == 0,
          "candidate order cannot change which floor is selected");

    // Gravity can cross a floor in small increments before a large correction
    // threshold is reached. Keep the accepted pose at the floor on EVERY hit.
    float accepted[] = {2, 0.02f, 3};
    float smallDescent[] = {2, -0.004f, 3};
    r = sweep(accepted, smallDescent, &ground, 1);
    check(r.valid && r.clamped && near(r.feetY, 0.02f) &&
          accepted[1] - smallDescent[1] < 0.025f && r.feetY - smallDescent[1] < 0.10f,
          "even a sub-threshold descent must clamp its first floor crossing");
    accepted[1] = r.feetY;
    smallDescent[1] = -0.06f;
    r = sweep(accepted, smallDescent, &ground, 1);
    check(r.valid && r.clamped && near(r.feetY, 0.02f),
          "successive small falls cannot erode the last accepted floor height");
    accepted[1] = -0.004f;  // Simulate ignoring the first safety clamp.
    smallDescent[1] = -0.20f;
    r = sweep(accepted, smallDescent, &ground, 1);
    check(r.valid && !r.clamped,
          "an already accepted below-floor pose cannot be recovered by this no-upward policy");
}

static void test_invalid_observations() {
    float previous[] = {0, 1, 0}, requested[] = {2, -8, 3};
    float start[3], end[3];
    mb::prepare_ground_sweep(previous, requested, start, end);
    auto ground = floor(2, 0, 3);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (unsigned axis = 0; axis < 3; ++axis) {
        auto bad = ground; bad.pos[axis] = nan;
        check(!sweep(previous, requested, &bad, 1).clamped, "NaN hit coordinate is rejected");
        bad = ground; bad.normal[axis] = infinity;
        check(!sweep(previous, requested, &bad, 1).clamped, "infinite normal coordinate is rejected");
    }
    auto offSegment = ground; offSegment.pos[0] += 0.05f;
    check(!sweep(previous, requested, &offSegment, 1).clamped, "hit from another ray cannot become the destination floor");
    auto zeroNormal = ground; zeroNormal.normal[1] = 0;
    check(!sweep(previous, requested, &zeroNormal, 1).clamped, "missing normal is rejected");
    auto scaledNormal = ground; scaledNormal.normal[1] = 5;
    check(!sweep(previous, requested, &scaledNormal, 1).clamped, "non-unit malformed normal cannot pass the walkability threshold");
    check(!mb::select_ground_crossing(start, end, 1, nullptr, 1).valid,
          "nonempty candidate list needs storage");
    check(!mb::select_ground_crossing(start, end, nan, &ground, 1).valid, "invalid previous height fails closed");
    check(!mb::select_ground_crossing(start, end, 1, &ground, 1, -0.01f).valid,
          "negative clearance cannot push feet into a floor");
    check(!mb::select_ground_crossing(start, end, 1, &ground, 1, infinity).valid,
          "nonfinite clearance fails closed");
    check(!mb::select_ground_crossing(start, end, 1, &ground, 1, 0.02f, nan).valid,
          "nonfinite hit tolerance fails closed");
    float highStart[] = {2, 3, 3};
    check(!mb::select_ground_crossing(highStart, end, 1, &ground, 1).valid,
          "unbounded upward search is not a permitted descent sweep");
    float diagonalStart[] = {0, 1.10f, 0};
    check(!mb::select_ground_crossing(diagonalStart, end, 1, &ground, 1).valid,
          "a diagonal probe cannot use the inferred vertical-normal contract");
    float badRequest[] = {nan, -8, 3};
    check(!mb::prepare_ground_sweep(previous, badRequest, start, end), "invalid requested X/Z prevents the query");
    check(!mb::prepare_ground_sweep(nullptr, requested, start, end), "missing previous position prevents the query");
    float hugePrevious[] = {0, std::numeric_limits<float>::max(), 0};
    check(!mb::prepare_ground_sweep(hugePrevious, requested, start, end),
          "unrepresentable upward lift fails closed without arithmetic overflow");
}

static void test_placement_and_hold() {
    float feet[] = {2, 80, 3}, nearFeet[] = {3, 80, 3}, farFeet[] = {20, 80, 3};
    check(mb::movement_acquisition_near(feet, nearFeet), "near native takeover pose is accepted");
    check(!mb::movement_acquisition_near(feet, farFeet), "old far-away client pose cannot teleport a newly acquired body");
    check(mb::movement_recovery_near(feet, farFeet), "near supported anchors remain within the recovery radius");
    farFeet[0] = 67;
    check(!mb::movement_recovery_near(feet, farFeet), "recovery cannot reach an anchor over 64m away");
    farFeet[0] = std::numeric_limits<float>::infinity();
    check(!mb::movement_acquisition_near(feet, farFeet), "nonfinite acquisition fails closed");
    float start[3], end[3], hit[] = {2, 79.98f, 3};
    check(mb::prepare_placement_support(feet, start, end) && near(start[1], 80.1f) && near(end[1], 79),
          "spawn support probes only the current nearby vertical column");
    check(mb::placement_support_hit(feet, hit), "fresh collision just below feet proves nearby support");
    hit[1] = 78.9f;
    check(!mb::placement_support_hit(feet, hit), "distant floor does not validate an unsupported spawn");
    hit[1] = 80.05f;
    check(!mb::placement_support_hit(feet, hit), "overhead geometry does not support a below-floor spawn");
    hit[1] = 79.98f; hit[0] = 2.1f;
    check(!mb::placement_support_hit(feet, hit), "another column cannot validate placement support");
    mb::StandInHoldLease lease;
    check(!lease.expired(100, true) && !lease.expired(2099, true) && lease.expired(2100, true),
          "fresh continuous HOLD updates cannot renew the two-second deadline");
    check(!lease.expired(2200, false) && !lease.expired(2300, true), "real MOVE/release starts a new hold lease");
    check(lease.expired(2299, true), "clock regression cannot extend a hold lease");
}
int main() {
    test_movement_cases();
    test_invalid_observations();
    test_placement_and_hold();
    std::printf("Movement safety fixtures: %s (%d checks, %d failures). No engine collision validation.\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
