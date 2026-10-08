// Standalone MSVC fixture for the actual game.cpp services. All objects/mailboxes
// are test-owned; engine calls and clocks are substituted. No game, IPC, or UI.
// Compile with /O2 /Gy and link /OPT:REF. Unused callback services fail closed.
#include "../src/common.h"
#include <array>
#include <vector>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../src/memutil.cpp"

static LONGLONG fixtureTicks = 0, castTicks = 0;
static uint64_t workerMs = 0;
static uint32_t engineCalls = 0, lastFilter = 0;
static void* lastIgnore = nullptr;
static uintptr_t havokGlobal = 0, geomGlobal = 0, chrGlobal = 0;
static uint8_t* havokObject = nullptr;
static uint8_t* geomManager = nullptr;
static uint8_t* chrManager = nullptr;
static const void* countedObject = nullptr;
static const void* countedModules = nullptr;
static uint32_t objectReads = 0, moduleChecks = 0;

static BOOL WINAPI fixture_counter(LARGE_INTEGER* value) {
    value->QuadPart = fixtureTicks;
    return TRUE;
}
static BOOL WINAPI fixture_frequency(LARGE_INTEGER* value) {
    value->QuadPart = 1000000;  // microsecond fixture ticks
    return TRUE;
}
namespace mb {
static bool fixture_read(const void* source, void* out, size_t size) {
    const uintptr_t address = (uintptr_t)source;
    if (size == sizeof(void*)) {
        if (address == havokGlobal) { memcpy(out, &havokObject, size); return true; }
        if (address == geomGlobal) { memcpy(out, &geomManager, size); return true; }
        if (address == chrGlobal) { memcpy(out, &chrManager, size); return true; }
    }
    if (source == countedObject) { ++objectReads; ++workerMs; }
    return mem_read(source, out, size);
}
static bool fixture_readable(const void* source, size_t size) {
    if (source == countedModules) ++moduleChecks;
    return mem_readable(source, size);
}
}
#define mem_read fixture_read
#define mem_readable fixture_readable
#define QueryPerformanceCounter fixture_counter
#define QueryPerformanceFrequency fixture_frequency
#include "../src/game.cpp"
#undef QueryPerformanceFrequency
#undef QueryPerformanceCounter
#undef mem_readable
#undef mem_read

alignas(16) static std::array<uint8_t, ERMC_RAYS_OFF_HITS + ERMC_MAX_RAYS * sizeof(ErmcRayHit)> rayMailbox;
static ErmcEntityTable entityMailbox = {};
static ErmcGameState stateMailbox = {};
namespace mb {
uint64_t now_ms() { return workerMs; }
void log(const char*, ...) {}
ErmcRayHeader* shm_rays() { return (ErmcRayHeader*)rayMailbox.data(); }
ErmcEntityTable* shm_entities() { return &entityMailbox; }
ErmcGameState* shm_state() { return &stateMailbox; }
// These symbols occur in the real callback tables, but none of those callbacks
// may execute in this fixture. Abort if a test accidentally expands its scope.
ErmcHeader* shm_header() { std::abort(); }
ErmcPassageTable* shm_passages() { std::abort(); }
ErmcHunterEvents* shm_hunter() { std::abort(); }
ErmcDamageQueue* shm_damage() { std::abort(); }
bool control_snapshot(ErmcControl*) { std::abort(); }
void on_frame() { std::abort(); }
void frame_claim_source() { std::abort(); }
HWND game_hwnd() { std::abort(); }
void perf_add_tick(double) { std::abort(); }
void compositor_note_applied_pose(uint64_t) { std::abort(); }
bool compositor_active() { std::abort(); }
bool compositor_world_active(bool) { return true; }
volatile LONG g_inflight = 0;
}
static bool __fastcall fixture_cast(void*, uint32_t filter, const float* origin,
                                   const float* delta, float* hit, void* ignore) {
    ++engineCalls;
    fixtureTicks += castTicks;
    lastFilter = filter;
    lastIgnore = ignore;
    for (int i = 0; i < 4; ++i) hit[i] = origin[i] + delta[i];
    return true;
}
template<class T> static void put(uint8_t* object, size_t offset, T value) {
    memcpy(object + offset, &value, sizeof(value));
}
static int checks = 0, failures = 0;
static void check(bool pass, const char* what) {
    ++checks;
    if (!pass) { ++failures; std::printf("FAIL: %s\n", what); }
}

static void reset_rays(uint32_t count, LONGLONG cost) {
    mb::g_frame = {true, 0x01000000, {0, 0, 0}};
    rayMailbox.fill(0);
    auto* header = mb::shm_rays();
    header->reqSeq = 17;
    header->respSeq = 16;
    header->count = count;
    auto* rays = (ErmcRay*)(rayMailbox.data() + ERMC_RAYS_OFF_RAYS);
    for (uint32_t i = 0; i < ERMC_MAX_RAYS; ++i) {
        rays[i].start[0] = (float)i;
        rays[i].start[1] = 10;
        rays[i].end[0] = (float)i;
    }
    fixtureTicks = 0;
    castTicks = cost;
    engineCalls = 0;
    mb::g_dbgRayState = 0;
}
static void test_ray_slices() {
    using namespace mb;
    std::array<uint8_t, 0xA0> havok = {};
    int world = 0, ignoredPlayer = 0;
    havokObject = havok.data();
    put(havok.data(), kHavokPhysWorld, &world);
    reset_rays(35, 3000);
    check(service_rays(&ignoredPlayer, true) == 1 && engineCalls == 1 &&
          shm_rays()->processed == 1 && shm_rays()->respSeq == 16,
          "one slow cast yields immediately and leaves the batch pending");
    check(lastFilter == kTerrainRayFilter && lastIgnore == &ignoredPlayer,
          "terrain filter and player-ignore guard remain intact");
    auto* hits = (ErmcRayHit*)(rayMailbox.data() + ERMC_RAYS_OFF_HITS);
    check(hits[0].hit == 1 && hits[0].normal[1] == 1 && hits[1].hit == 0,
          "only processed answers are written, including downward landing convention");
    castTicks = 1000;
    check(service_rays(&ignoredPlayer, true) == 2 && shm_rays()->processed == 3,
          "resume obeys the same two-millisecond budget from a nonaligned index");
    while (shm_rays()->respSeq != shm_rays()->reqSeq)
        service_rays(&ignoredPlayer, true);
    check(engineCalls == 35 && shm_rays()->processed == 0 && shm_rays()->respSeq == 17,
          "all answers eventually complete exactly once before response publication");
    bool ordered = true;
    for (uint32_t i = 0; i < 35; ++i) ordered &= hits[i].hit == 1 && hits[i].pos[0] == (float)i;
    check(ordered && service_rays(&ignoredPlayer, true) == 0,
          "answers preserve request order and completed mailboxes are not recast");

    reset_rays(5, 3000);
    put(havok.data(), kHavokPhysWorld, (void*)nullptr);
    check(service_rays(&ignoredPlayer, true) == 0 && shm_rays()->respSeq == 16 &&
          shm_rays()->processed == 0 && hits[0].hit == 0,
          "missing physics world preserves the loading/floor-safety pending state");
    put(havok.data(), kHavokPhysWorld, &world);
    check(service_rays(&ignoredPlayer, false) == 0 && engineCalls == 0,
          "inactive consumer cannot drain a stale terrain request");
    g_dbgRayState = 1;
    g_dbgRayFlags = ERMC_RAYS_CUSTOM_FILTER | ERMC_RAYS_HIT_SELF;
    g_dbgFilter = 123;
    check(service_rays(&ignoredPlayer, false) == 1 && g_dbgRayState == 2 &&
          shm_rays()->processed == 0 && lastFilter == 123 && lastIgnore == nullptr,
          "explicit debug ray remains independent of consumer liveness");

    reset_rays(ERMC_MAX_RAYS + 1, 1000);
    shm_rays()->processed = ERMC_MAX_RAYS - 1;
    shm_rays()->flags = ERMC_RAYS_CUSTOM_FILTER;
    shm_rays()->filterA = 456;
    check(service_rays(nullptr, true) == 1 && shm_rays()->respSeq == 17 && lastFilter == 456,
          "oversized batch remains bounded by mailbox capacity and custom filter survives");
    reset_rays(3, 3000);
    shm_rays()->processed = 9;
    check(service_rays(nullptr, true) == 1 && shm_rays()->processed == 1,
          "invalid progress restarts inside the mailbox bounds");
    havokObject = nullptr;
}

struct Character {
    std::array<uint8_t, 0x580> instance = {};
    std::array<uint8_t, 0x70> modules = {};
    std::array<uint8_t, 0x1A0> data = {};
    std::array<uint8_t, 0x300> physics = {};
    void init(uint64_t handle, float distanceSq, bool player = false) {
        using namespace mb;
        put(instance.data(), 0, g_base + (player ? rva::kVtPlayerIns : rva::kVtEnemyIns));
        put(instance.data(), kChrModules, modules.data());
        put(instance.data(), kChrDistSq, distanceSq);
        put(instance.data(), kChrHandle, handle);
        put(instance.data(), kChrModelId, 2000);
        put(instance.data(), kChrTeamType, uint8_t(6));
        put(modules.data(), kModData, data.data());
        put(modules.data(), kModPhysics, physics.data());
        put(data.data(), 0, g_base + rva::kVtDataModule);
        put(data.data(), kDataHp, 100);
        put(data.data(), kDataMaxHp, 100);
        put(physics.data(), 0, g_base + rva::kVtPhysModule);
    }
};
static void test_entity_filter() {
    using namespace mb;
    Character candidate;
    candidate.init(1, 10000);
    countedModules = candidate.modules.data();
    moduleChecks = 0;
    Chr result = {};
    check(!get_chr(candidate.instance.data(), &result, true) && moduleChecks == 0,
          "distant entity does not validate or dereference its modules");
    check(get_chr(candidate.instance.data(), &result) && moduleChecks == 1,
          "queued combat/debug consumption keeps its original full character validation");
    put(candidate.instance.data(), kChrDistSq, std::numeric_limits<float>::quiet_NaN());
    check(!get_chr(candidate.instance.data(), &result, true) && moduleChecks == 1,
          "NaN distance is rejected before modules");
    put(candidate.instance.data(), kChrDistSq, -1.0f);
    check(!get_chr(candidate.instance.data(), &result, true) && moduleChecks == 1,
          "negative distance is rejected before modules");
    put(candidate.instance.data(), kChrDistSq, kPublishRadius * kPublishRadius);
    check(get_chr(candidate.instance.data(), &result, true) && moduleChecks == 2,
          "entity exactly on the existing publication radius retains full validation");
    put(candidate.instance.data(), kChrModules, (uint8_t*)nullptr);
    check(!get_chr(candidate.instance.data(), &result, true),
          "nearby entity with missing modules cannot be published");
    put(candidate.instance.data(), 0, uintptr_t(0));
    check(!get_chr(candidate.instance.data(), &result, true),
          "invalid character vtable remains rejected");
    countedModules = nullptr;

    Character enemy, player, distant;
    enemy.init(11, 100);
    player.init(22, 100, true);
    distant.init(33, 10000);
    std::vector<uint8_t> manager(kChrsByDistance + 0x18);
    std::array<uint8_t, 4 * kChrByDistanceEntry> entries = {};
    put(manager.data(), 0, g_base + rva::kVtWorldChrMan);
    put(manager.data(), kChrsByDistance + 8, entries.data());
    put(manager.data(), kChrsByDistance + 0x10, entries.data() + entries.size());
    put(entries.data(), 0, enemy.instance.data());
    put(entries.data(), kChrByDistanceEntry, player.instance.data());
    put(entries.data(), 2 * kChrByDistanceEntry, distant.instance.data());
    put(entries.data(), 3 * kChrByDistanceEntry, enemy.instance.data());
    chrManager = manager.data();
    Player host = {};
    g_combatOk = false;
    g_coopExcludePlayerIns = false;
    check(publish_entities(host) == 4 && entityMailbox.count == 2 &&
          entityMailbox.entities[0].id == 11 && entityMailbox.entities[1].id == 22 &&
          !(entityMailbox.seq & 1),
          "solo publication preserves near PlayerIns, deduplication, and even seqlock");
    g_coopExcludePlayerIns = true;
    check(!find_published(22, &result), "co-op consumption guard rejects an already-published PlayerIns");
    check(publish_entities(host) == 4 && entityMailbox.count == 1 &&
          entityMailbox.entities[0].id == 11 && !find_published(22, &result),
          "co-op publication still excludes PlayerIns and distant enemies");
    g_coopExcludePlayerIns = false;
    chrManager = nullptr;
}

static void test_object_deadline() {
    using namespace mb;
    std::array<uint8_t, 0x30> manager = {}, head = {}, node = {};
    std::array<uint8_t, 0x100> pool = {};
    std::array<uint8_t, 0x4E0> geometry = {};
    std::vector<uint64_t> pointers(16384, (uint64_t)geometry.data());
    put(manager.data(), 0, g_base + rva::kVtWorldGeomMan);
    put(manager.data(), 0x20, head.data());
    put(head.data(), 8, node.data());
    put(node.data(), 0, head.data());
    put(node.data(), 0x10, head.data());
    put(node.data(), 0x20, uint32_t(0x01000000));
    put(node.data(), 0x28, pool.data());
    put(pool.data(), 0xE8, pointers.data());
    put(pool.data(), 0xFC, uint32_t(pointers.size() - 2));
    put(geometry.data(), 0, g_base + rva::kVtGeomDynamicIns);
    geomManager = manager.data();
    countedObject = geometry.data();
    workerMs = 0;
    objectReads = 0;
    g_ok = true;
    g_reachBlock = 0x01000000;
    g_reachReady = -1;
    reach_scan();
    check(objectReads == 50 && workerMs == 50 && g_reachReady >= 0 &&
          g_reachLists[g_reachReady].n == 0,
          "16K-slot tile stops at the worker deadline and publishes a bounded partial list");
    g_reachBlock = 0xFFFFFFFFu;
    reach_scan();
    check(objectReads == 50, "free-flight disabled reach scan performs no object walk");
    geomManager = nullptr;
    countedObject = nullptr;
}
int main() {
    mb::g_base = (uintptr_t)&fixture_cast - mb::rva::kCastRay;
    havokGlobal = mb::g_base + mb::rva::kCSHavokMan;
    geomGlobal = mb::g_base + mb::rva::kWorldGeomMan;
    chrGlobal = mb::g_base + mb::rva::kWorldChrMan;
    test_ray_slices();
    test_entity_filter();
    test_object_deadline();
    std::printf("Native long-run service contract: %d checks, %d failures.\n", checks, failures);
    return failures ? 1 : 0;
}
