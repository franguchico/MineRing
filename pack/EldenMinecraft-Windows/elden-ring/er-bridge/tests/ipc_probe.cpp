// Integration test only: a synthetic host exercises the exact production mapping and ABI.
// This never reads, starts or hooks Elden Ring.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

static_assert(offsetof(ErmcHeader, hostHeartbeat) == 0x10, "Java heartbeat offset");
static_assert(offsetof(ErmcGameState, camPos) == 0x10, "Java camera offset");
static_assert(offsetof(ErmcControl, hunterPos) == 0x38, "Java player offset");

int main() {
    using namespace mb;
    if (!shm_open()) { fprintf(stderr, "map failed\n"); return 1; }
    ErmcHeader* h = shm_header();
    h->hostHeartbeat = 987654321;
    state_begin_write();
    auto s = shm_state();
    s->flags = ERMC_STATE_PLAYER_VALID | ERMC_STATE_CAMERA_VALID;
    s->frame = 12345;
    s->camPos[0] = 10.5f; s->camPos[1] = -2.25f; s->camPos[2] = 100.0f;
    s->unitsPerMeter = 1.0f;
    state_end_write();
    puts("HOST_READY"); fflush(stdout);
    auto until = now_ms() + 20000;
    ErmcControl c = {};
    while (now_ms() < until) {
        if (control_snapshot(&c) && c.mcFrame == 424242 && shm_damage()->write == 1 &&
            shm_rays()->reqSeq != shm_rays()->respSeq) break;
        Sleep(1);
    }
    auto damage = shm_damage();
    auto rays = shm_rays();
    auto ray = (ErmcRay*)((uint8_t*)rays + sizeof(ErmcRayHeader));
    if (c.mcFrame != 424242 || c.camPos[0] != 3.25f || c.hunterPos[2] != -7.5f ||
        damage->write != 1 || damage->ring[0].id != 0x1122334455667788ull ||
        damage->ring[0].amount != 9.5f || rays->count != 1 || ray[0].start[1] != 20.f) {
        fprintf(stderr, "Java payload/ABI mismatch or timeout\n"); shm_close(); return 2;
    }
    auto hits = (ErmcRayHit*)((uint8_t*)rays + sizeof(ErmcRayHeader) + ERMC_MAX_RAYS * sizeof(ErmcRay));
    hits[0].pos[0] = 4.5f; hits[0].pos[1] = 5.5f; hits[0].pos[2] = 6.5f;
    hits[0].normal[1] = 1.0f; hits[0].hit = 1; hits[0].attr = 42;
    compiler_barrier();
    rays->respSeq = rays->reqSeq;
    until = now_ms() + 10000;
    while (now_ms() < until && h->mcHeartbeat != 2) Sleep(1);
    bool ok = h->mcHeartbeat == 2;
    puts(ok ? "IPC_BIDIRECTIONAL_PASS" : "Java ray acknowledgement timed out");
    shm_close(); log_close();
    return ok ? 0 : 3;
}
