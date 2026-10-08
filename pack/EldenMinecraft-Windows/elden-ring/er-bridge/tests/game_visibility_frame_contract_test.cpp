// Exercises production game.cpp tick/lifecycle/frame code with test-owned game
// objects and mailboxes. No game, IPC files, window/input calls or engine calls.
// MSVC: /O2 /Gy /std:c++17 /I src /I include, link /OPT:REF user32.lib.
// Define ERMC_GAME_SOURCE and ERMC_BASELINE_TEST to run against a saved old core.
#include "../src/common.h"
#include <array>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <cstdarg>
#include "../src/memutil.cpp"

static uint64_t fixtureMs = 10000;
static uint64_t fixtureUnixMs = 1791158400000ull;
static uint8_t* worldChrMan = nullptr;
static uintptr_t worldChrGlobal = 0;
static uintptr_t fixtureImageBase = 0;
static HWND hostWindow = (HWND)1, foregroundWindow = (HWND)2;
static ErmcControl controlMailbox = {};
static bool snapshotAvailable = true;
static int focusCalls = 0;
static std::vector<std::string> fixtureLogs;
struct FixtureImageSpan { uintptr_t address; std::vector<uint8_t> bytes; };
static std::vector<FixtureImageSpan> fixtureImage;
static uintptr_t retiredPeer = 0;
static unsigned retiredPeerReads = 0;
static uintptr_t countedEnemy = 0;
static unsigned enemyIdentityReads = 0;
static bool fixtureWorldComposited = true;
static unsigned fixtureAppliedPoses = 0;
static bool fixtureF8 = false;
static bool fixtureFocusDenied = false;
static uintptr_t havokGlobal = 0;
static std::array<uint8_t, 0xA0> fixtureHavok = {};
static bool fixturePhysicsWorld = true, fixtureFloorHit = true;
static float fixtureFloorDelta = -0.02f;
static bool fixtureAbsoluteFloor = false;
static float fixtureFloorY = 80;
static bool fixtureNarrowFloor = false;
static unsigned fixtureCasts = 0;
static void* fixtureRayIgnore = nullptr;
static uint32_t fixtureRayFilter = 0;
static bool __fastcall fixture_cast(void*, uint32_t filter, const float* origin,
                                   const float* delta, float* hit, void* ignore) {
    ++fixtureCasts; fixtureRayFilter = filter; fixtureRayIgnore = ignore;
    if (!fixtureFloorHit) return false;
    if (fixtureNarrowFloor && std::fabs(origin[0] - 2.0f) > 0.2f) return false;
    hit[0] = origin[0];
    hit[1] = fixtureAbsoluteFloor ? fixtureFloorY : origin[1] - 0.1f + fixtureFloorDelta;
    hit[2] = origin[2]; hit[3] = 1;
    return delta[1] < 0 && hit[1] <= origin[1] && hit[1] >= origin[1] + delta[1];
}
static ULONGLONG WINAPI fixture_clock() { return fixtureMs; }
static void WINAPI fixture_filetime(LPFILETIME out) {
    uint64_t ticks = fixtureUnixMs * 10000 + 116444736000000000ull;
    out->dwLowDateTime = DWORD(ticks); out->dwHighDateTime = DWORD(ticks >> 32);
}
static BOOL WINAPI fixture_counter(LARGE_INTEGER* out) { out->QuadPart = fixtureMs * 1000; return TRUE; }
static BOOL WINAPI fixture_frequency(LARGE_INTEGER* out) { out->QuadPart = 1000000; return TRUE; }
static HWND WINAPI fixture_foreground() { return foregroundWindow; }
static SHORT WINAPI fixture_key(int key) { return key == VK_F8 && fixtureF8 ? SHORT(-32768) : 0; }
static BOOL WINAPI fixture_grant(DWORD) { return TRUE; }
static BOOL WINAPI fixture_show(HWND, int) { ++focusCalls; return TRUE; }
static BOOL WINAPI fixture_foreground_set(HWND h) {
    ++focusCalls; if (fixtureFocusDenied) return FALSE;
    foregroundWindow = h; return TRUE;
}
static HWND WINAPI fixture_focus(HWND h) { ++focusCalls; return h; }
namespace mb {
static bool fixture_read(const void* source, void* out, size_t size) {
    uintptr_t address = (uintptr_t)source;
    if (address == countedEnemy && countedEnemy) ++enemyIdentityReads;
    if (retiredPeer && address >= retiredPeer && address - retiredPeer < 0x700) {
        ++retiredPeerReads; return false;
    }
    for (const auto& span : fixtureImage) {
        if (address >= span.address && address - span.address <= span.bytes.size() &&
            size <= span.bytes.size() - (address - span.address)) {
            memcpy(out, span.bytes.data() + (address - span.address), size); return true;
        }
    }
    if ((uintptr_t)source == worldChrGlobal && size == sizeof(void*)) {
        memcpy(out, &worldChrMan, size); return true;
    }
    if (address == havokGlobal && size == sizeof(void*)) {
        void* p = fixturePhysicsWorld ? fixtureHavok.data() : nullptr;
        memcpy(out, &p, size); return true;
    }
    // Every other fake game global is absent, so no engine entry can be called.
    if (address >= fixtureImageBase && address - fixtureImageBase < 0x5000000) return false;
    return mem_read(source, out, size);
}
}
#define GetTickCount64 fixture_clock
#define GetSystemTimeAsFileTime fixture_filetime
#define QueryPerformanceCounter fixture_counter
#define QueryPerformanceFrequency fixture_frequency
#define GetForegroundWindow fixture_foreground
#define GetAsyncKeyState fixture_key
#define AllowSetForegroundWindow fixture_grant
#define ShowWindow fixture_show
#define SetForegroundWindow fixture_foreground_set
#define SetActiveWindow fixture_focus
#define SetFocus fixture_focus
#define mem_read fixture_read
#ifndef ERMC_GAME_SOURCE
#define ERMC_GAME_SOURCE "../src/game.cpp"
#endif
#include ERMC_GAME_SOURCE
#undef mem_read
#undef SetFocus
#undef SetActiveWindow
#undef SetForegroundWindow
#undef ShowWindow
#undef AllowSetForegroundWindow
#undef GetAsyncKeyState
#undef GetForegroundWindow
#undef QueryPerformanceFrequency
#undef QueryPerformanceCounter
#undef GetTickCount64
#undef GetSystemTimeAsFileTime

alignas(8) static std::array<uint8_t, 0xE00> fixtureSharedMemory = {};
static ErmcHeader& headerMailbox = *(ErmcHeader*)fixtureSharedMemory.data();
static ErmcGameState stateMailbox = {};
static ErmcEntityTable entitiesMailbox = {};
static ErmcPassageTable passagesMailbox = {};
static ErmcRayHeader raysMailbox = {};
static ErmcHunterEvents hunterMailbox = {};
static ErmcDamageQueue damageMailbox = {};
namespace mb {
uint64_t now_ms() { return fixtureMs; }
void log(const char* format, ...) {
    char line[2048]; va_list args; va_start(args, format);
    vsnprintf(line, sizeof(line), format, args); va_end(args);
    fixtureLogs.emplace_back(line);
}
ErmcHeader* shm_header() { return &headerMailbox; }
ErmcGameState* shm_state() { return &stateMailbox; }
ErmcEntityTable* shm_entities() { return &entitiesMailbox; }
ErmcPassageTable* shm_passages() { return &passagesMailbox; }
ErmcRayHeader* shm_rays() { return &raysMailbox; }
ErmcHunterEvents* shm_hunter() { return &hunterMailbox; }
ErmcDamageQueue* shm_damage() { return &damageMailbox; }
bool control_snapshot(ErmcControl* out) {
    if (!snapshotAvailable) return false;
    *out = controlMailbox; return true;
}
void on_frame() {
    stateMailbox = {};
    game_fill_state(&stateMailbox);
}
void frame_claim_source() {}
HWND game_hwnd() { return hostWindow; }
void perf_add_tick(double) {}
void compositor_note_applied_pose(uint64_t) { ++fixtureAppliedPoses; }
bool compositor_active() { return false; }
bool compositor_world_active(bool) { return fixtureWorldComposited; }
volatile LONG g_inflight = 0;
}
template<class T> static void put(uint8_t* object, size_t offset, T value) {
    memcpy(object + offset, &value, sizeof(value));
}
static int checks = 0, failures = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}
static bool close(float a, float b) { return fabsf(a - b) < 0.003f; }
static bool same(const float* a, const float* b) {
    return close(a[0], b[0]) && close(a[1], b[1]) && close(a[2], b[2]);
}
struct Character {
    std::array<uint8_t, 0x700> instance = {};
    std::array<uint8_t, 0x70> modules = {};
    std::array<uint8_t, 0x1A0> data = {};
    std::array<uint8_t, 0x300> physics = {};
    mb::Player player() { return {instance.data(), data.data(), physics.data()}; }
    void init(uint32_t block = 0x3c2a2400) {
        using namespace mb;
        instance.fill(0); modules.fill(0); data.fill(0); physics.fill(0);
        put(instance.data(), 0, g_base + rva::kVtPlayerIns);
        put(instance.data(), kChrModules, modules.data());
        put(instance.data(), kChrRenderFlags, uint8_t(0x28));
        put(modules.data(), kModData, data.data());
        put(modules.data(), kModPhysics, physics.data());
        put(data.data(), 0, g_base + rva::kVtDataModule);
        put(data.data(), kDataHp, 100);
        put(data.data(), kDataMaxHp, 100);
        put(physics.data(), 0, g_base + rva::kVtPhysModule);
        position(block, {2, 80, 3}, {2, 80, 3});
    }
    void position(uint32_t block, std::array<float, 3> raw, std::array<float, 3> havok) {
        using namespace mb;
        put(instance.data(), kPlayerBlockId, block);
        memcpy(instance.data() + kPlayerBlockPos, raw.data(), 12);
        memcpy(physics.data() + kPhysPos, havok.data(), 12);
    }
    bool rendered() { return (instance[mb::kChrRenderFlags] & 8) != 0; }
    bool pinned() {
        using namespace mb;
        return (*(uint32_t*)(instance.data() + kChrDebugFlags) & 32) ||
               physics[kPhysGravityOff] || (data[kDataFlags] & 1);
    }
};
static std::vector<uint8_t> manager(mb::kChrsByDistance + 0x18);
static void reset_frame(bool coop) {
    using namespace mb;
    g_coopExcludePlayerIns = coop;
    g_frame = {false, 0, {0, 0, 0}};
    g_rawZone = 0; g_framePlayer = nullptr; g_page = nullptr;
    g_bias[0] = g_bias[1] = g_bias[2] = 0;
    g_haveHmR = g_putHavokValid = false;
}
static void start_tick_fixture(Character& local, bool coop = true) {
    using namespace mb;
    reset_frame(coop);
    local.init();
    manager.assign(manager.size(), 0);
    put(manager.data(), 0, g_base + rva::kVtWorldChrMan);
    put(manager.data(), kMainPlayer, local.instance.data());
    worldChrMan = manager.data();
    fixtureSharedMemory.fill(0); stateMailbox = {}; entitiesMailbox = {};
    passagesMailbox = {}; raysMailbox = {}; hunterMailbox = {}; damageMailbox = {};
    headerMailbox.mcPid = 77; headerMailbox.mcStartMs = fixtureMs;
    foregroundWindow = (HWND)2; hostWindow = (HWND)1; focusCalls = 0;
    snapshotAvailable = true; controlMailbox = {};
    g_standing = g_havePut = g_movementCorrected = false; g_standPlayer = nullptr;
#ifdef ERMC_BASELINE_TEST
    g_hidden = false;
#else
    g_visibility.reset();
    g_holding = false;
    g_holdLease.reset(); InterlockedExchange(&g_controlRevoked, 0);
    InterlockedExchange(&g_bodyControlAccepted, 0);
    g_autoRecoverySeen = false;
    g_haveTerminalHandoff = false;
    g_explicitReset = {};
    g_peerCompositeLease.reset();
    g_peerImageSize = 0x4000000; fixtureWorldComposited = true;
    g_peerLocalSteam = 0; g_peerSteamQueryAt = ~0ull;
    g_peerModeLease.reset(); g_peerRenderOwners.reset();
#endif
    fixtureImage.clear(); retiredPeer = 0; retiredPeerReads = 0;
    fixtureF8 = false; fixtureFocusDenied = false; g_f8Down = false;
    fixturePhysicsWorld = fixtureFloorHit = true; fixtureFloorDelta = -0.02f;
    fixtureAbsoluteFloor = false; fixtureFloorY = 80;
    fixtureNarrowFloor = false;
    g_haveRecoveryAnchor = false; g_anchorProbeAt = 0;
    fixtureCasts = 0; fixtureHavok.fill(0);
    put(fixtureHavok.data(), kHavokPhysWorld, fixtureHavok.data());
    countedEnemy = 0; enemyIdentityReads = 0;
    fixtureLogs.clear();
    g_lastHp = -1; g_clampFrames = 0; g_combatOk = g_actionOk = false;
    g_nearestHostile.valid = false; g_reachReady = -1;
    g_reachOffset[0] = g_reachOffset[1] = g_reachOffset[2] = 0;
    g_focusSeenInit = false; g_mcDeathsInit = false; g_expectDeath = false;
    g_haveLastCtrl = false; g_lastCtrlSeq = 0; g_lastCtrlChangeMs = 0;
    auto p = local.player();
    update_frame(p, false);
    g_life = LIFE_ALIVE; g_lifeTicked = true; g_aliveZone = g_frame.zone;
    g_lastAliveMs = fixtureMs; g_lastStandMs = 0;
    // The real liveness watch must observe progress in this new session.
    task_tick_body(nullptr);
    ++fixtureMs; ++headerMailbox.mcHeartbeat;
    task_tick_body(nullptr);
}
static void tick(Character& local, uint64_t elapsed, uint32_t flags, bool beat = true) {
    fixtureMs += elapsed;
    if (beat) ++headerMailbox.mcHeartbeat;
    controlMailbox.seq += 2;
    controlMailbox.flags = flags;
    float raw[3]; mb::raw_stable_pos(local.player(), raw);
    memcpy(controlMailbox.hunterPos, raw, 12);
    mb::task_tick_body(nullptr);
}
static constexpr uint32_t drive = ERMC_CTRL_MOVE_HUNTER | ERMC_CTRL_HIDE_HUNTER | ERMC_CTRL_FREE_FLIGHT;
static void test_visibility_tick_lifecycle() {
    using namespace mb;
    Character local, remote;
    remote.init();
    const auto remoteBefore = remote.instance;
    start_tick_fixture(local);
    // A remote PlayerIns is even present in the production distance list.
    std::array<uint8_t, kChrByDistanceEntry> entries = {};
    put(entries.data(), 0, remote.instance.data());
    put(manager.data(), kChrsByDistance + 8, entries.data());
    put(manager.data(), kChrsByDistance + 0x10, entries.data() + entries.size());
    tick(local, 1, drive);
    check(!local.rendered() && local.pinned(), "a live local hide request pins and hides the main player");
    local.instance[kChrRenderFlags] |= 8;
    tick(local, 16, drive);
    check(!local.rendered() && (local.instance[kChrRenderFlags] & 0x20),
          "engine reasserting render is corrected each tick without changing unrelated bits");
    const uint64_t lastRequest = fixtureMs;
    tick(local, 100, 0);
    check(!local.rendered() && !local.pinned() && !g_standing && !g_havePut && !g_putHavokValid,
          "co-op zero-control readiness gap holds only visibility and immediately releases physics");
    tick(local, 149, 0);
    check(!local.rendered() && fixtureMs - lastRequest == 249,
          "repeated zero-control updates cannot renew the original visibility deadline");
    tick(local, 1, 0);
    check(local.rendered() && !local.pinned(), "visibility restores exactly at the 250ms bound");
    check(remote.instance == remoteBefore && entitiesMailbox.count == 0,
          "a remote PlayerIns remains untouched and excluded from co-op proxies");
    tick(local, 1, drive);
    tick(local, 100, 0);
    tick(local, 30, drive);
    check(!local.rendered() && local.pinned(), "resuming within the grace has no visible intermediate tick");
    tick(local, 1, ERMC_CTRL_OVERRIDE_CAMERA);
    check(local.rendered() && !local.pinned(), "explicit camera-only control restores the body immediately");

    start_tick_fixture(local, false);
    tick(local, 1, drive); tick(local, 1, 0);
    check(local.rendered(), "solo zero-control release remains immediate");

    start_tick_fixture(local);
    tick(local, 1, drive);
    foregroundWindow = hostWindow;
    tick(local, 1, 0);
    check(local.rendered() && !local.pinned(), "actual host focus bypasses co-op visibility grace");

    start_tick_fixture(local);
    tick(local, 1, drive);
    ++headerMailbox.hostFocusReq;
    tick(local, 1, 0);
    check(local.rendered() && focusCalls > 0, "explicit host handoff restores before the grace expires");

    start_tick_fixture(local);
    tick(local, 1, drive);
    ++headerMailbox.hostFocusReq;
    tick(local, 1, drive);
    check(local.rendered() && !local.pinned(), "host handoff overrides the cached moving/hiding pose for that tick");

    start_tick_fixture(local);
    tick(local, 1, drive); tick(local, 100, 0);
    headerMailbox.mcPid = 78; headerMailbox.mcStartMs = fixtureMs + 1;
    tick(local, 1, 0);
    check(local.rendered() && !local.pinned(), "consumer-session change cannot inherit an old visibility lease");

    start_tick_fixture(local);
    tick(local, 1, drive);
    tick(local, 1001, 0, false);
    check(local.rendered() && !local.pinned(), "stale consumer heartbeat restores and releases physics");

    start_tick_fixture(local);
    tick(local, 1, drive); tick(local, 100, 0);
    game_shutdown();
    check(local.rendered() && !local.pinned(), "shutdown restores a hidden body even after motion was released");

    start_tick_fixture(local);
    local.instance[kChrRenderFlags] &= (uint8_t)~8;
    tick(local, 1, drive); tick(local, 250, 0);
    check(!local.rendered(), "release does not enable rendering that was already disabled before ownership");

    start_tick_fixture(local);
    tick(local, 1, drive);
    put(local.data.data(), kDataHp, 0);
    tick(local, 1, 0);
    check(local.rendered() && g_life == LIFE_DEAD && !local.pinned(), "death restores immediately and releases the local body");

    start_tick_fixture(local);
    tick(local, 1, drive);
    put(local.instance.data(), kPlayerBlockId, uint32_t(0xFFFFFFFF));
    tick(local, 1, 0);
    check(local.rendered() && !g_frame.valid && !local.pinned(), "loading releases a validated body even with an invalid block id");

    start_tick_fixture(local);
    tick(local, 1, drive);
    auto p = local.player();
    controlMailbox.hunterPos[0] = std::numeric_limits<float>::quiet_NaN();
    const auto beforePhysics = local.physics;
    stand_in(p, &controlMailbox, true);
    check(local.rendered() && !local.pinned() &&
          !memcmp(local.physics.data() + kPhysPos, beforePhysics.data() + kPhysPos, 16),
          "nonfinite motion releases visibility and cannot write a physics pose");

    start_tick_fixture(local);
    tick(local, 1, drive); tick(local, 100, 0);
    remote.init(); remote.instance[kChrRenderFlags] = 0x20;
    put(manager.data(), kMainPlayer, remote.instance.data());
    worldChrMan = manager.data();
    controlMailbox.seq += 2; controlMailbox.flags = 0;
    ++fixtureMs; ++headerMailbox.mcHeartbeat;
    task_tick_body(nullptr);
    check(remote.instance[kChrRenderFlags] == 0x20 && !remote.pinned(),
          "replacement main player never inherits render ownership from the old instance");

    start_tick_fixture(local);
    tick(local, 1, drive); tick(local, 100, 0);
    worldChrMan = nullptr;
    tick(local, 1, 0);
    check(!g_standing && !g_havePut, "missing validated main player abandons remembered state without writes");
    worldChrMan = nullptr;
}

#ifndef ERMC_BASELINE_TEST
static bool logged(const char* text) {
    for (const auto& line : fixtureLogs) if (line.find(text) != std::string::npos) return true;
    return false;
}
static void image_span(uintptr_t address, const void* data, size_t n) {
    const auto* bytes = (const uint8_t*)data;
    fixtureImage.push_back({address, std::vector<uint8_t>(bytes, bytes + n)});
}
static void fixture_rtti(uint32_t vt, uint32_t col, uint32_t desc, const char* name) {
    uintptr_t table[2] = {mb::g_base + col, 0};
    image_span(mb::g_base + vt - 8, table, sizeof(table));
    uint32_t locator[6] = {1, 0, 0, desc, 0, col};
    image_span(mb::g_base + col, locator, sizeof(locator));
    image_span(mb::g_base + desc + 16, name, strlen(name) + 1);
}
struct PeerCharacter : Character {
    alignas(8) std::array<uint8_t, 16> session = {};
    alignas(8) std::array<uint8_t, 24> entry = {};
    void peer(uint64_t steam, uint64_t handle, int32_t role = 1) {
        init();
        put(instance.data(), 8, handle); put(instance.data(), 0x68, role);
        put(instance.data(), 0x5B0, session.data()); put(instance.data(), 0x6B8, entry.data());
        put(session.data(), 0, mb::g_base + 0x1000); put(session.data(), 8, steam);
        put(entry.data(), 0x10, steam);
    }
};
static void publish_mode(uint64_t steam, uint32_t mode = ERMC_PEER_MINECRAFT, bool rosterProgress = true) {
    auto* p = mb::peer_mode();
    p->seq = p->seq + 2;
    p->version = ERMC_PEER_VERSION; p->mcStartMs = headerMailbox.mcStartMs;
    p->remoteSteamId = steam; p->mode = mode; p->canRenderPeer = 1;
    if (!p->peerEpoch) p->peerEpoch = 10;
    if (rosterProgress) ++p->rosterSequence;
}
static void test_peer_sidecar_tick() {
    using namespace mb;
    constexpr uint64_t localId = 76561198000000001ull, peerId = localId + 1;
    constexpr uint32_t composite = drive | ERMC_CTRL_COMPOSITE;
    Character local;
    PeerCharacter peer, npc, other;
    start_tick_fixture(local);
    g_peerLocalSteam = localId;
    headerMailbox.hostStartMs = 12345;
    peer.peer(peerId, 123); npc.peer(peerId + 1, 456, 5); other.peer(peerId + 2, 789);
    fixture_rtti((uint32_t)rva::kVtPlayerIns, 0x4000, 0x5000, ".?AVPlayerIns@CS@@");
    fixture_rtti(0x1000, 0x2000, 0x3000, ".?AVPlayerNetworkSession@CS@@");
    std::array<uint8_t, 3 * kChrByDistanceEntry> entries = {};
    auto set_list = [&](size_t count) {
        put(manager.data(), kPlayerChrSet + 0x10, (uint32_t)count);
        put(manager.data(), kPlayerChrSet + 0x18, entries.data());
        for (size_t i = 0; i < count; ++i) {
            auto ins = *(uint8_t**)(entries.data() + i * kChrByDistanceEntry);
            put(ins, 0x10, entries.data() + i * kChrByDistanceEntry);
        }
    };
    put(entries.data(), 0, peer.instance.data());
    put(entries.data(), kChrByDistanceEntry, npc.instance.data());
    set_list(2);
    const auto npcBefore = npc.instance;
    publish_mode(peerId); tick(local, 1, composite);
    publish_mode(peerId); tick(local, 1, composite);
    check(peer.rendered() && peer_info()->remoteSteamId == 0 &&
          peer_info()->flags == ERMC_PEER_LOCAL_VALID,
          "an unvalidated player set cannot hide or advertise a remote player");
    check(peer_info()->version == ERMC_PEER_VERSION && peer_info()->hostStartMs == 12345 &&
          peer_info()->localSteamId == localId && !(peer_info()->seq & 1),
          "native sidecar publishes the frozen layout with a stable even sequence and host identity");

    // Exercise per-object verification, with no global enable bypass.
    put(manager.data(), kPlayerChrSet, g_base + rva::kVtChrSet);
    publish_mode(peerId); tick(local, 1, composite);
    check(!peer.rendered() && peer_info()->remoteSteamId == peerId &&
          (peer_info()->flags & ERMC_PEER_REMOTE_HIDDEN),
          "matching verified peer MC mode plus observer composition hides the native render bit");
    check(npc.instance == npcBefore && !peer.pinned(),
          "NPC PlayerIns and remote gameplay flags remain untouched");
    peer.instance[kChrRenderFlags] |= 8;
    publish_mode(peerId); tick(local, 16, ERMC_CTRL_HOLD_HUNTER | ERMC_CTRL_COMPOSITE);
    check(g_holding && !peer.rendered(), "fresh composited HOLD keeps a drawable MC peer's native body hidden");
    publish_mode(peerId, ERMC_PEER_ELDEN_RING); tick(local, 1, composite);
    check(peer.rendered() && !local.rendered() && !(peer_info()->flags & ERMC_PEER_REMOTE_HIDDEN),
          "the remote peer's own ER mode restores its native body while observer remains MC");
    publish_mode(peerId); tick(local, 1, composite);
    foregroundWindow = hostWindow;
    tick(local, 1, composite);
    check(peer.rendered(), "observer host focus restores native even while peer mode says MC");
    constexpr uint32_t passive = ERMC_CTRL_COMPOSITE | ERMC_CTRL_PASSIVE_COMPOSITE;
    controlMailbox.mcFrame = 25;
    publish_mode(peerId); tick(local, 1, passive);
    check(!peer.rendered() && local.rendered() && !local.pinned(),
          "ER observer plus MC peer keeps only peer native body hidden; local controls and body restored");
    publish_mode(peerId, ERMC_PEER_ELDEN_RING); tick(local, 1, passive);
    check(peer.rendered() && local.rendered() && !local.pinned(),
          "ER observer plus ER peer restores both native bodies without a local stand-in");
    publish_mode(peerId); tick(local, 1, passive | drive);
    check(peer.rendered() && local.rendered() && !local.pinned(),
          "contradictory passive plus body ownership flags cannot take over the local body or hide the peer");
    publish_mode(peerId); tick(local, 1, passive);
    fixtureWorldComposited = false;
    publish_mode(peerId); tick(local, 1, passive);
    check(!peer.rendered(), "a brief missing world frame preserves the already composed MC peer");
    for (int i = 0; i < 3; ++i) { publish_mode(peerId); tick(local, 80, passive); }
    check(!peer.rendered(), "missing frames cannot renew the original 250ms visibility deadline");
    publish_mode(peerId); tick(local, 9, passive);
    check(peer.rendered(), "a sustained missing submitted world restores native at the deadline");
    fixtureWorldComposited = true;
    foregroundWindow = (HWND)2;
    publish_mode(peerId); tick(local, 1, drive);
    check(peer.rendered(), "observer without COMPOSITE never makes a peer invisible");
    InterlockedExchange(&g_controlRevoked, 1);
    controlMailbox.mcFrame = 30;
    publish_mode(peerId); tick(local, 1, passive);
    check(!peer.rendered() && local.rendered() && !local.pinned(),
          "revoked automatic body control still allows safe passive MC peer composition in native mode");
    InterlockedExchange(&g_controlRevoked, 0);
    publish_mode(peerId); peer_mode()->canRenderPeer = 0; tick(local, 1, composite);
    check(peer.rendered(), "canRenderPeer false restores native despite observer composition");

    publish_mode(peerId); tick(local, 1, composite);
    publish_mode(peerId, ERMC_PEER_MINECRAFT, false); tick(local, 1501, composite);
    check(peer.rendered(), "locally republished IPC cannot renew a stale network roster lease");
    publish_mode(peerId); tick(local, 1, composite);
    check(!peer.rendered(), "a fresh roster can resume visibility after an ordinary readiness lapse");
    tick(local, 1501, composite);
    check(peer.rendered(), "unchanged IPC publication also expires after 1500ms");
    publish_mode(peerId); tick(local, 1, composite);
    ++peer_mode()->seq; tick(local, 1, composite);
    check(!peer.rendered(), "a brief torn publication reuses the last verified same-session peer mode");
    tick(local, 249, composite);
    check(peer.rendered(), "an odd publication cannot renew the bounded cached-mode deadline");
    ++peer_mode()->seq;
    publish_mode(peerId); tick(local, 1, composite);
    check(peer.rendered(), "first observation after a torn/reset stream requires fresh roster progress");
    publish_mode(peerId); tick(local, 1, composite);
    check(!peer.rendered(), "two consistent advancing publications reestablish the peer lease");

    ++peer_mode()->peerEpoch; publish_mode(peerId); tick(local, 1, composite);
    check(peer.rendered(), "new authenticated peer epoch cannot inherit the previous visibility lease");
    publish_mode(peerId); tick(local, 1, composite);
    ++headerMailbox.mcPid; ++headerMailbox.mcStartMs;
    publish_mode(peerId); tick(local, 1, composite);
    check(peer.rendered(), "Minecraft process/session replacement releases remote visibility immediately");
    publish_mode(peerId); tick(local, 1, composite);
    publish_mode(peerId); tick(local, 1, composite);
    check(!peer.rendered(), "reconnected consumer can establish a fresh exact-identity lease");

    publish_mode(peerId + 99); tick(local, 1, composite);
    publish_mode(peerId + 99); tick(local, 1, composite);
    check(peer.rendered(), "a lone native peer is never associated with a different network Steam identity");
    publish_mode(peerId); tick(local, 1, composite);
    publish_mode(peerId); tick(local, 1, composite);
    put(entries.data(), 2 * kChrByDistanceEntry, other.instance.data()); set_list(3);
    publish_mode(peerId); tick(local, 1, composite);
    check(!peer.rendered() && other.rendered() && peer_info()->remoteSteamId == peerId,
          "additional human cannot disable or inherit visibility for the exact requested Steam identity");
    other.peer(peerId, 790);
    set_list(3);
    publish_mode(peerId); tick(local, 1, composite);
    check(peer.rendered() && other.rendered() && !peer_info()->remoteSteamId,
          "duplicate live Steam identity is ambiguous and restores both native bodies");
    set_list(2); publish_mode(peerId); tick(local, 1, composite);
    check(!peer.rendered(), "single verified human can resume after ambiguity ends");

    // Invalid observations cannot dereference/restore a remembered object. When
    // it returns with the same identity, pending ownership allows safe restore.
    set_list(0); retiredPeer = (uintptr_t)peer.instance.data();
    publish_mode(peerId, ERMC_PEER_ELDEN_RING); tick(local, 1, composite);
    check(retiredPeerReads == 0 && !peer_info()->remoteSteamId,
          "despawn performs no reads or writes through a remembered remote pointer");
    retiredPeer = 0; set_list(2);
    publish_mode(peerId, ERMC_PEER_ELDEN_RING); tick(local, 1, composite);
    check(peer.rendered(), "same validated identity reappearing permits a pending owned-bit restore");

    publish_mode(peerId); tick(local, 1, composite);
    put(peer.instance.data(), kChrHandle, uint64_t(124)); peer.instance[kChrRenderFlags] = 0x20;
    publish_mode(peerId, ERMC_PEER_ELDEN_RING); tick(local, 1, composite);
    check(peer.instance[kChrRenderFlags] == 0x20,
          "reused instance address with changed handle cannot inherit or restore previous ownership");
    publish_mode(peerId); tick(local, 1, composite);
    publish_mode(peerId, ERMC_PEER_ELDEN_RING); tick(local, 1, composite);
    check(peer.instance[kChrRenderFlags] == 0x20, "release preserves a native render bit already clear before ownership");
    check(peer_mode()->mode == ERMC_PEER_ELDEN_RING && (peer_info()->flags & ERMC_PEER_REMOTE_VALID) &&
          (peer_info()->flags & ERMC_PEER_REMOTE_HIDDEN),
          "peer ER mode reports engine-owned hidden render state so Java cannot cancel its only visible avatar");
    peer.instance[kChrRenderFlags] = 0x28;
    publish_mode(peerId); tick(local, 1, composite);
    game_shutdown();
    check(peer.rendered() && peer_info()->flags == 0, "shutdown restores a currently validated peer and clears sidecar identity");

    NativePeerIdentity identity;
    check(native_peer_identity((uintptr_t)peer.instance.data(), (uintptr_t)local.instance.data(), &identity),
          "source-backed candidate accepts the complete synthetic RTTI and matching identity chain");
    put(peer.entry.data(), 0x10, peerId + 9);
    check(!native_peer_identity((uintptr_t)peer.instance.data(), 0, &identity), "identity mismatch between session paths is rejected");
    put(peer.entry.data(), 0x10, peerId);
    check(valid_peer_steam(0x0110000100000001ull) && valid_peer_steam(0x01100001ffffffffull) &&
          !valid_peer_steam(0x0110000100000000ull) && !valid_peer_steam(0x0110000200000000ull),
          "native and Java accept exactly the same public individual desktop Steam identity range");
    put(peer.session.data(), 8, uint64_t(123)); put(peer.entry.data(), 0x10, uint64_t(123));
    check(!native_peer_identity((uintptr_t)peer.instance.data(), 0, &identity),
          "matching invalid Steam identities cannot classify a peer");
    put(peer.session.data(), 8, peerId); put(peer.entry.data(), 0x10, peerId);
    put(peer.instance.data(), 8, uint64_t(0x12000000ffffffffull));
    check(!native_peer_identity((uintptr_t)peer.instance.data(), 0, &identity),
          "empty handle selector is rejected even with a nonempty block identifier");
    put(peer.instance.data(), 8, uint64_t(124));
    bool rolesRejected = true;
    for (int32_t role : {3, 4, 5, 6, 7, 9, 10, 11, 12, 14, 19, 20, 21, 22}) {
        put(peer.instance.data(), 0x68, role);
        rolesRejected &= !native_peer_identity((uintptr_t)peer.instance.data(), 0, &identity);
    }
    check(rolesRejected, "NPC, summon/invader NPC, ghost and unknown roles fail closed even with matching IDs");
    put(peer.instance.data(), 0x68, int32_t(1));
    fixtureImage.back().bytes[4] = 'X';
    check(!native_peer_identity((uintptr_t)peer.instance.data(), 0, &identity), "a different network-session RTTI name is rejected");
    worldChrMan = nullptr;
}
static void test_peer_rebind_tick() {
    using namespace mb;
    constexpr uint64_t localId = 76561198000000001ull, peerId = localId + 1;
    constexpr uint32_t composite = drive | ERMC_CTRL_COMPOSITE;
    Character local;
    PeerCharacter peer;
    start_tick_fixture(local);
    g_peerLocalSteam = localId;
    peer.peer(peerId, 123);
    fixture_rtti((uint32_t)rva::kVtPlayerIns, 0x4000, 0x5000, ".?AVPlayerIns@CS@@");
    fixture_rtti(0x1000, 0x2000, 0x3000, ".?AVPlayerNetworkSession@CS@@");
    alignas(8) std::array<uint8_t, 16> reboundSession = peer.session;
    alignas(8) std::array<uint8_t, 24> reboundEntry = peer.entry;
    alignas(8) std::array<uint8_t, 32> players = {};
    put(manager.data(), kPlayerChrSet, g_base + rva::kVtChrSet);
    put(manager.data(), kPlayerChrSet + 0x10, uint32_t(2));
    put(manager.data(), kPlayerChrSet + 0x18, players.data());
    auto rebind = [&](bool alternate) {
        players.fill(0);
        size_t offset = alternate ? 16 : 0;
        put(players.data(), offset, peer.instance.data());
        put(peer.instance.data(), 0x10, players.data() + offset);
        put(peer.instance.data(), 0x5B0, alternate ? reboundSession.data() : peer.session.data());
        put(peer.instance.data(), 0x6B8, alternate ? reboundEntry.data() : peer.entry.data());
    };
    rebind(false);
    publish_mode(peerId); tick(local, 1, composite);
    publish_mode(peerId); tick(local, 1, composite);
    check(!peer.rendered(), "rebind fixture starts with an owned native peer render bit");
    bool rebindsHidden = true;
    for (int i = 0; i < 12; ++i) {
        rebind((i & 1) == 0);
        publish_mode(peerId); tick(local, 1, composite);
        rebindsHidden &= !peer.rendered() && (peer_info()->flags & ERMC_PEER_REMOTE_HIDDEN);
    }
    check(rebindsHidden, "more than eight verified session/entry/slot rebinds retain peer render ownership");
    rebind(true);
    publish_mode(peerId, ERMC_PEER_ELDEN_RING); tick(local, 1, composite);
    check(peer.rendered() && peer.instance[kChrRenderFlags] == 0x28,
          "ER mode immediately restores the owned bit across another freshly verified rebind");
    publish_mode(peerId); tick(local, 1, composite);
    rebind(false);
    game_shutdown();
    check(peer.rendered(), "shutdown restores a live same-body peer after its slot and session change");
    worldChrMan = nullptr;
}
static size_t owner_count(const mb::PeerRenderOwners& owners) {
    size_t n = 0;
    for (const auto& s : owners.slots) if (s.used) ++n;
    return n;
}
static mb::NativePeerIdentity owner_identity(size_t serial, uint64_t steam = 0) {
    mb::NativePeerIdentity id;
    // Keys deliberately point at no fixture object: ownership operations must
    // never attempt to read through remembered instance/session/slot addresses.
    id.ins = 0x400000 + serial * 0x1000;
    id.session = 0x800000 + serial * 0x1000;
    id.entry = 0xc00000 + serial * 0x1000;
    id.slot = 0x1000000 + serial * 16;
    id.handle = 0x3c00000000000000ull | uint64_t(serial + 1);
    id.steam = steam ? steam : 76561198000000001ull + serial;
    return id;
}
static void test_peer_owner_replacement_and_capacity() {
    using namespace mb;
    PeerRenderOwners owners;
    auto original = owner_identity(1);
    uint8_t flags = 0xa8;
    check(owners.apply(original, flags, true) && flags == 0xa0,
          "peer ownership removes only the native render bit");
    auto rebound = original;
    rebound.session += 8; rebound.entry += 8; rebound.slot += 16;
    owners.apply(rebound, flags, false);
    check(flags == 0xa8 && owner_count(owners) == 0,
          "fresh apply alone restores the same body across session/entry/slot rebind");
    flags = 0xa0;
    owners.apply(original, flags, true);
    owners.observe(&rebound, 1, true);
    owners.apply(rebound, flags, false);
    check(flags == 0xa0, "rebind does not take ownership of an engine-hidden render bit");

    flags = 0x28;
    owners.apply(original, flags, true);
    auto reused = rebound; ++reused.handle;
    flags = 0x20;
    owners.observe(&reused, 1, false);
    owners.apply(reused, flags, false);
    check(flags == 0x20 && owner_count(owners) == 0,
          "verified handle replacement retires the old key even in a partial observation and never inherits");
    flags = 0x28;
    owners.apply(original, flags, true);
    reused = rebound; ++reused.steam;
    flags = 0x20;
    owners.apply(reused, flags, false);
    check(flags == 0x20 && owner_count(owners) == 0,
          "another Steam account at the same address cannot restore a previous owned bit");

    flags = 0x28;
    owners.apply(original, flags, true);
    for (int i = 0; i < 20; ++i) owners.observe(nullptr, 0, true);
    check(owner_count(owners) == 1 && flags == 0x20,
          "repeated complete absence keeps a pending restore while capacity is available");
    owners.observe(&rebound, 1, true);
    owners.apply(rebound, flags, false);
    check(flags == 0x28 && owner_count(owners) == 0,
          "same body reappearing after prolonged despawn still restores through its current verified key");

    bool reconnectsHidden = true, replacementsBounded = true;
    for (size_t i = 0; i < 24; ++i) {
        auto current = owner_identity(10 + i, original.steam);
        owners.observe(&current, 1, true);
        flags = 0x28;
        reconnectsHidden &= owners.apply(current, flags, true) && flags == 0x20;
        replacementsBounded &= owner_count(owners) == 1;
    }
    check(reconnectsHidden && replacementsBounded,
          "24 reconnects with the same Steam account and new bodies cannot exhaust the eight ownership records");
    auto last = owner_identity(33, original.steam);
    owners.apply(last, flags, false);
    check(flags == 0x28, "latest reconnect restores its own original render bit");

    owners.reset(); flags = 0x28;
    owners.apply(original, flags, true);
    auto replacement = owner_identity(100, original.steam);
    NativePeerIdentity ambiguous[2] = {replacement, owner_identity(101, original.steam)};
    for (int i = 0; i < 5; ++i) owners.observe(ambiguous, 2, true);
    check(owner_count(owners) == 1,
          "ambiguous duplicate Steam bodies do not prove an immediate pending-owner replacement");
    NativePeerIdentity coexist[2] = {original, replacement};
    owners.observe(coexist, 2, true);
    owners.apply(original, flags, false);
    check(flags == 0x28, "a still enumerated old body retains restoration when another live body shares its Steam ID");

    owners.reset();
    std::array<NativePeerIdentity, 8> present;
    std::array<uint8_t, 8> pendingFlags;
    auto fillOwners = [&]() {
        owners.reset();
        for (size_t i = 0; i < present.size(); ++i) {
            present[i] = owner_identity(200 + i);
            pendingFlags[i] = 0x28;
            owners.apply(present[i], pendingFlags[i], true);
        }
    };
    fillOwners();
    auto incoming = owner_identity(300);
    owners.observe(present.data(), present.size(), true);
    flags = 0x28;
    check(!owners.apply(incoming, flags, true) && flags == 0x28 && owner_count(owners) == 8,
          "capacity pressure cannot evict any freshly observed live body");
    for (int i = 0; i < 10; ++i) owners.observe(&incoming, 1, false);
    check(!owners.apply(incoming, flags, true) && flags == 0x28,
          "partial or unreadable observations cannot retire pending owners under capacity pressure");
    owners.observe(&incoming, 1, true);
    owners.observe(&incoming, 1, true);
    check(!owners.apply(incoming, flags, true), "two complete absences are insufficient to evict a pending owner");
    owners.observe(nullptr, 0, false);
    owners.observe(&incoming, 1, true);
    check(!owners.apply(incoming, flags, true), "an incomplete observation breaks consecutive absence proof");
    owners.observe(&incoming, 1, true);
    check(!owners.apply(incoming, flags, true), "absence proof restarts after an incomplete observation");
    owners.observe(&incoming, 1, true);
    check(owners.apply(incoming, flags, true) && flags == 0x20 && owner_count(owners) == 8,
          "three complete absences free only the record needed to hide an incoming ninth body");
    bool churnHidden = true;
    for (size_t i = 1; i < 16; ++i) {
        incoming = owner_identity(300 + i);
        for (unsigned j = 0; j < PeerRenderOwners::kAbsentObservationsBeforeEviction; ++j)
            owners.observe(&incoming, 1, true);
        flags = 0x28;
        churnHidden &= owners.apply(incoming, flags, true) && flags == 0x20 && owner_count(owners) == 8;
    }
    bool retiredFlagsUntouched = true;
    for (auto retiredFlags : pendingFlags) retiredFlagsUntouched &= retiredFlags == 0x20;
    check(churnHidden && retiredFlagsUntouched,
          "more than eight distinct reconnects reclaim bounded absent keys without accessing or restoring retired objects");
    owners.apply(incoming, flags, false);
    check(flags == 0x28 && owner_count(owners) == 7, "capacity eviction preserves restoration of the newly owned live body");

    fillOwners();
    for (unsigned i = 0; i < PeerRenderOwners::kAbsentObservationsBeforeEviction; ++i)
        owners.observe(nullptr, 0, true);
    owners.observe(present.data(), present.size(), true);
    flags = 0x28;
    check(!owners.apply(incoming, flags, true), "reappearing bodies clear stale absence eligibility before capacity is requested");
    bool liveRestores = true;
    for (size_t i = 0; i < present.size(); ++i) {
        owners.apply(present[i], pendingFlags[i], false);
        liveRestores &= pendingFlags[i] == 0x28;
    }
    check(liveRestores && owner_count(owners) == 0, "all eight reappearing bodies retain their original owned-bit restores");
}
static void test_hold_tick_lifecycle() {
    using namespace mb;
    constexpr uint32_t hold = ERMC_CTRL_HOLD_HUNTER;
    static_assert(hold == (1u << 9), "HOLD uses the coordinated native/Java control bit");
    Character local, other;
    start_tick_fixture(local);
    tick(local, 1, hold);
    check(!g_standing && !g_holding && !local.pinned() && local.rendered(),
          "HOLD cannot acquire a local body before a valid MOVE");

    tick(local, 1, drive);
    const auto acceptedPhysics = local.physics;
    std::array<float, 3> accepted; memcpy(accepted.data(), g_lastPut, 12);
    // Grounded HOLD flags deliberately omit HIDE/FREE_FLIGHT and supply garbage
    // pose/yaw. Only the already accepted body pose may reach physics/events.
    controlMailbox.hunterPos[0] = std::numeric_limits<float>::quiet_NaN();
    controlMailbox.hunterYawDeg = std::numeric_limits<float>::infinity();
    controlMailbox.flags = hold; controlMailbox.seq += 2;
    fixtureMs += 760; ++headerMailbox.mcHeartbeat;
    put(local.data.data(), kDataHp, 75);
    task_tick_body(nullptr);
    check(g_holding && g_standing && local.pinned() && !local.rendered(),
          "760ms readiness HOLD retains all stand-in protection and visibility");
    check(same(g_lastPut, accepted.data()) && local.physics == acceptedPhysics,
          "HOLD ignores incoming position and yaw and pins only the accepted pose");
    check(hunterMailbox.hitCount == 1 && hunterMailbox.lastDamage == 25 &&
          *(int*)(local.data.data() + kDataHp) == 100 && same(hunterMailbox.lastHitFrom, accepted.data()),
          "HOLD forwards real damage and refills HP through the existing stand-in path");
    check(logged("HOLD started"), "HOLD acquisition is logged once as a transition");
    // Reasserting render/protection is independent of incoming HIDE flags.
    local.instance[kChrRenderFlags] |= 8;
    local.physics[kPhysGravityOff] = 0; local.data[kDataFlags] &= 0xfeu;
    g_movementCorrected = true;
    g_correctedFeet[0] = -999; g_correctedFeet[1] = -999; g_correctedFeet[2] = -999;
    g_reachOffset[0] = 0.25f; g_reachOffset[1] = 0; g_reachOffset[2] = 0.5f;
    tick(local, 16, hold);
    check(local.pinned() && !local.rendered() && same(g_lastPut, accepted.data()) &&
          g_movementCorrected && close(g_reachOffset[0], 0.25f),
          "HOLD reapplies protection without movement correction or reach repositioning");
    g_movementCorrected = false; g_reachOffset[0] = g_reachOffset[2] = 0;
    controlMailbox.hunterYawDeg = 0;  // a valid MOVE replaces the intentionally malformed HOLD yaw
    tick(local, 1, drive | hold);
    check(g_standing && !g_holding && logged("HOLD ended"), "MOVE takes precedence over simultaneous HOLD");
    tick(local, 1, hold); tick(local, 1, 0);
    check(!g_standing && !local.pinned(), "zero control still releases HOLD protection immediately");

    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, hold);
    ++headerMailbox.hostFocusReq;
    tick(local, 1, hold);
    check(!g_holding && !local.pinned() && local.rendered(), "F8 host handoff overrides a cached HOLD");
    foregroundWindow = (HWND)2;
    tick(local, 1, hold);
    check(!g_standing, "HOLD after F8 cannot reacquire without a new MOVE");

    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, hold);
    foregroundWindow = hostWindow;
    tick(local, 1, hold);
    check(!g_holding && !local.pinned(), "actual host focus releases HOLD without a handoff counter");

    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, hold);
    fixtureMs += 1001; ++headerMailbox.mcHeartbeat;  // same control seq, fresh heartbeat
    task_tick_body(nullptr);
    check(!g_holding && !local.pinned() && local.rendered(), "unchanged HOLD control expires at the existing 1s lease");

    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, hold);
    tick(local, 1001, hold, false);
    check(!g_holding && !local.pinned(), "fresh HOLD seq cannot keep a stale consumer heartbeat alive");

    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, hold);
    ++headerMailbox.mcPid; ++headerMailbox.mcStartMs;
    tick(local, 1, hold);
    check(!g_holding && !local.pinned(), "consumer reconnect cannot inherit HOLD");
    tick(local, 1, hold);
    check(!g_standing, "a newly live consumer must establish MOVE before HOLD");

    start_tick_fixture(local); tick(local, 1, drive); ++headerMailbox.hostLife;
    tick(local, 1, hold);
    check(!g_holding && !local.pinned(), "a new hostLife cannot inherit the old accepted pose");

    start_tick_fixture(local); tick(local, 1, drive);
    local.position(0x0a010000, {2, 80, 3}, {2, 80, 3});
    tick(local, 1, hold);
    check(!g_holding && !local.pinned(), "raw map transition releases HOLD before using another frame");

    start_tick_fixture(local); tick(local, 1, drive);
    put(local.instance.data(), kPlayerBlockId, uint32_t(0xFFFFFFFF));
    tick(local, 1, hold);
    check(!g_holding && !local.pinned(), "invalid loading block cannot retain HOLD");

    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, hold);
    put(local.data.data(), kDataHp, 0);
    tick(local, 1, hold);
    check(g_life == LIFE_DEAD && headerMailbox.hostDeaths == 1 && !g_holding && !local.pinned() &&
          *(int*)(local.data.data() + kDataHp) == 0,
          "genuine native death during HOLD is forwarded, never refilled or suppressed");
    check(logged("native death observed hp 0 max 100") && logged("standing 1 hold 1 clamp 0") &&
          logged("recentControl 1 control 0x200") && logged("freeFlight 1"),
          "native death diagnostic records pre-release HP flags standing hold clamp and recent mode");

    start_tick_fixture(local); tick(local, 1, drive); other.init();
    put(manager.data(), kMainPlayer, other.instance.data());
    tick(local, 1, hold);
    check(!g_holding && !other.pinned() && other.rendered(), "replacement main PlayerIns never inherits HOLD");

    start_tick_fixture(local); tick(local, 1, drive);
    put(local.instance.data(), kChrHandle, uint64_t(999));
    tick(local, 1, hold);
    check(!g_holding && !local.pinned(), "reused address with another FieldInsHandle cannot inherit HOLD");

    start_tick_fixture(local); tick(local, 1, drive); other.init();
    put(local.modules.data(), kModPhysics, other.physics.data());
    tick(local, 1, hold);
    check(!g_holding && !g_standing, "replacement physics module invalidates HOLD ownership");

    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, hold);
    worldChrMan = nullptr;
    const auto retired = local.instance;
    task_tick_body(nullptr);
    check(!g_holding && !g_standing && local.instance == retired,
          "despawn drops HOLD without dereferencing a remembered body");

    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, hold);
    game_shutdown();
    check(!g_holding && !local.pinned() && local.rendered(), "shutdown restores an existing held body");

    start_tick_fixture(local, false); tick(local, 1, drive); tick(local, 1, hold);
    check(!g_holding && !local.pinned(), "solo cannot opt into the coop HOLD extension");

    start_tick_fixture(local); other.init();
    const auto npcInstance = other.instance;
    const auto npcData = other.data;
    const auto npcPhysics = other.physics;
    std::array<uint8_t, kChrByDistanceEntry> entries = {};
    put(entries.data(), 0, other.instance.data());
    put(manager.data(), kChrsByDistance + 8, entries.data());
    put(manager.data(), kChrsByDistance + 0x10, entries.data() + entries.size());
    tick(local, 1, drive); tick(local, 760, hold);
    check(other.instance == npcInstance && other.data == npcData && other.physics == npcPhysics,
          "local HOLD never changes another human-or-NPC PlayerIns");
    worldChrMan = nullptr;
}
static void test_bounded_hold_recovery() {
    using namespace mb;
    Character local;
    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, ERMC_CTRL_HOLD_HUNTER);
    const auto accepted = local.physics;
    for (int i = 0; i < 19; ++i) tick(local, 100, ERMC_CTRL_HOLD_HUNTER);
    check(g_holding && local.pinned(), "fresh HOLD remains bounded but protected before its two-second deadline");
    put(local.data.data(), kDataHp, 73);
    tick(local, 50, ERMC_CTRL_HOLD_HUNTER);
    check(hunterMailbox.hitCount == 1 && hunterMailbox.lastDamage == 27 &&
          *(int*)(local.data.data() + kDataHp) == 100,
          "real damage is still forwarded and refilled during the short HOLD recovery window");
    tick(local, 50, ERMC_CTRL_HOLD_HUNTER);
    check(!g_holding && !g_standing && !local.pinned() && local.rendered() &&
          foregroundWindow == hostWindow && g_controlRevoked && g_lastStandMs == 0,
          "continuously fresh HOLD releases protections, rendering, death authority and focus at 2000ms");
    check(!memcmp(local.physics.data() + kPhysPos, accepted.data() + kPhysPos, 16) &&
          logged("HOLD timed out"), "timeout releases in place without an unverified rescue teleport");
    int calls = focusCalls;
    for (int i = 0; i < 5; ++i) tick(local, 100, drive | ERMC_CTRL_OVERRIDE_CAMERA);
    check(!g_standing && local.rendered() && focusCalls == calls,
          "old producer MOVE/camera packets cannot reacquire or repeatedly steal focus after timeout");
    ++headerMailbox.mcDeaths;
    tick(local, 1, 0);
    check(!g_expectDeath && *(int*)(local.data.data() + kDataHp) == 100,
          "a deferred inactive Minecraft death cannot kill the returned native player");
    fixtureF8 = true; tick(local, 1, 0); fixtureF8 = false;
    check(!g_controlRevoked && headerMailbox.mcSwitchReq == 1,
          "explicit native F8 re-arms the revoked input owner");
    foregroundWindow = (HWND)2;
    tick(local, 1, drive);
    check(g_standing && local.pinned(), "a synchronized new MOVE may resume after explicit native retry");

    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, ERMC_CTRL_HOLD_HUNTER);
    fixtureFocusDenied = true;
    tick(local, 2000, ERMC_CTRL_HOLD_HUNTER);
    check(g_controlRevoked && !local.pinned() && local.rendered() && foregroundWindow != hostWindow,
          "Windows foreground denial cannot prolong native HOLD or retain body protection");
    calls = focusCalls;
    tick(local, 20, drive);
    check(!local.pinned() && focusCalls == calls, "focus denial still revokes stale ownership without a retry loop");
    worldChrMan = nullptr;
}
static void test_placement_runtime() {
    using namespace mb;
    Character local;
    start_tick_fixture(local);
    g_life = LIFE_NONE; g_lifeTicked = true; g_quickSettle = false;
    fixtureFloorHit = false;
    for (int i = 0; i < 120; ++i) tick(local, 16, drive);
    check(g_life == LIFE_SETTLING && !local.pinned() && local.rendered() &&
          (stateMailbox.flags & ERMC_STATE_HOST_BUSY) && !(stateMailbox.flags & ERMC_STATE_PLAYER_VALID),
          "stable late-join coordinates without collision support never become a playable MC body");
    fixtureFloorHit = true; fixtureFloorDelta = 0.05f;
    tick(local, 16, drive);
    check(g_life == LIFE_SETTLING, "near overhead geometry cannot make a spawn inside rock usable");
    fixtureFloorDelta = -0.02f;
    tick(local, 16, 0);
    check(g_life == LIFE_ALIVE && (stateMailbox.flags & ERMC_STATE_PLAYER_VALID) &&
          fixtureRayFilter == kTerrainRayFilter && fixtureRayIgnore == local.instance.data(),
          "fresh support at the current spawn completes settlement using terrain-only player-ignored rays");

    start_tick_fixture(local);
    controlMailbox.seq += 2; controlMailbox.flags = drive;
    raw_stable_pos(local.player(), controlMailbox.hunterPos); controlMailbox.hunterPos[0] += 40;
    ++headerMailbox.mcHeartbeat; ++fixtureMs;
    const auto before = local.physics;
    task_tick_body(nullptr);
    check(!local.pinned() && local.rendered() &&
          !memcmp(local.physics.data() + kPhysPos, before.data() + kPhysPos, 16) && !g_bodyControlAccepted,
          "first MOVE cannot teleport to a stale pre-load pose even with fresh control publication");

    start_tick_fixture(local); tick(local, 1, drive);
    auto p = local.player();
    fixtureFloorDelta = 0;
    controlMailbox.hunterPos[1] = 79.996f;
    stand_in(p, &controlMailbox, true);
    check(close(*(float*)(p.phys + kPhysPos + 4), 80) && g_movementCorrected,
          "production ground sweep applies a sub-threshold floor clamp before writing physics");
    controlMailbox.hunterPos[1] = 80;
    stand_in(p, &controlMailbox, true);  // client acknowledges the landing
    fixturePhysicsWorld = false;
    controlMailbox.hunterPos[1] = 70;
    stand_in(p, &controlMailbox, true);
    check(!local.pinned() && local.rendered() && close(*(float*)(p.phys + kPhysPos + 4), 80),
          "missing collision world cannot be misclassified as a floor miss and accept an unchecked descent");

    start_tick_fixture(local); tick(local, 1, drive);
    g_putHavokValid = false;
    put(local.physics.data(), kPhysPos, 500.0f); p = local.player();
    stand_in(p, &controlMailbox, true);
    check(!local.pinned() && close(*(float*)(p.phys + kPhysPos), 500),
          "an engine warp cannot be undone by the old MOVE pose after pinned-pose validity is lost");
    start_tick_fixture(local); tick(local, 1, drive);
    controlMailbox.hunterYawDeg = (std::numeric_limits<float>::max)();
    p = local.player(); stand_in(p, &controlMailbox, true);
    const float* quat = (const float*)(p.phys + kPhysQuat);
    check(std::isfinite(quat[0]) && std::isfinite(quat[1]) && std::isfinite(quat[2]) && std::isfinite(quat[3]),
          "a huge finite client yaw cannot overflow the native body quaternion");
    worldChrMan = nullptr;
}
static void test_auto_air_recovery() {
    using namespace mb;
    Character local;
    auto fly = [&]() {
        fixtureAbsoluteFloor = true; fixtureFloorY = 80;
        tick(local, 1, drive);  // acquire on a genuine supported floor
        controlMailbox.seq += 2; controlMailbox.flags = drive;
        raw_stable_pos(local.player(), controlMailbox.hunterPos);
        controlMailbox.hunterPos[1] = 110;
        fixtureMs += 300; ++headerMailbox.mcHeartbeat;
        task_tick_body(nullptr);
        check(g_haveRecoveryAnchor && close(g_recoveryAnchor[1], 80) &&
              close(*(float*)(local.physics.data() + kPhysPos + 4), 110),
              "airborne flight never replaces the last collision-supported ground anchor");
        tick(local, 1, ERMC_CTRL_HOLD_HUNTER);
    };
    start_tick_fixture(local); fly();
    put(local.data.data(), kDataHp, 82);
    tick(local, 2000, ERMC_CTRL_HOLD_HUNTER);
    check(!local.pinned() && local.rendered() && close(*(float*)(local.physics.data() + kPhysPos + 4), 80) &&
          logged("returned to fresh same-life/map anchor") && *(int*)(local.data.data() + kDataHp) == 82,
          "automatic native HOLD expiry returns an airborne body to its freshly revalidated supported anchor");
    float recovered[3]; to_stable((const float*)(local.physics.data() + kPhysPos), recovered);
    tick(local, 16, 0);
    check(same(stateMailbox.playerPos, recovered), "automatic relocation cannot become a false Havok grid recentre next tick");

    start_tick_fixture(local); fly();
    ++headerMailbox.hostFocusReq;
    tick(local, 100, 0);
    check(!local.pinned() && close(*(float*)(local.physics.data() + kPhysPos + 4), 110),
          "intentional manual F8 handoff preserves the chosen airborne native position");

    start_tick_fixture(local); fly();
    fixtureFloorHit = false;
    tick(local, 2000, ERMC_CTRL_HOLD_HUNTER);
    check(!local.pinned() && close(*(float*)(local.physics.data() + kPhysPos + 4), 110) &&
          logged("no nearby freshly supported anchor") && logged("fall risk"),
          "unloaded/removed anchor collision never authorizes a stale teleport or an indefinite damage shield");

    for (bool lifeChange : {true, false}) {
        start_tick_fixture(local); fly();
        if (lifeChange) ++headerMailbox.hostLife;
        else ++g_frame.zone;
        auto p = local.player(); const auto before = local.physics;
        check(!recover_auto_handoff(p, "test context changed") && local.physics == before,
              "another map or life cannot inherit the stored ground anchor");
    }
    start_tick_fixture(local); fly();
    fixtureNarrowFloor = true;
    tick(local, 2000, ERMC_CTRL_HOLD_HUNTER);
    check(!local.pinned() && close(*(float*)(local.physics.data() + kPhysPos + 4), 110),
          "a cached anchor with only center support cannot recover onto an unsupported footprint");

    start_tick_fixture(local); fly();
    g_putHavokValid = false;
    auto p = local.player(); const auto before = local.physics;
    check(!recover_auto_handoff(p, "test engine warp") && local.physics == before,
          "a lost pinned-pose lease cannot recover through an old anchor even while its floor is loaded");
    for (bool handleChange : {true, false}) {
        start_tick_fixture(local); fly();
        if (handleChange) put(local.instance.data(), kChrHandle, uint64_t(999));
        else g_standPhys = nullptr;
        p = local.player(); const auto unchanged = local.physics;
        check(!recover_auto_handoff(p, "test owner replaced") && local.physics == unchanged,
              "replacement handle or physics ownership cannot inherit a supported anchor");
    }
#ifdef ERMC_CTRL_AUTO_RECOVERY
    start_tick_fixture(local); fly();
    // Reproduce a native tick between the reason control and the separate focus
    // counter. This must not release at the airborne pose and lose the anchor.
    tick(local, 750, ERMC_CTRL_AUTO_RECOVERY);
    check(!local.pinned() && close(*(float*)(local.physics.data() + kPhysPos + 4), 80) &&
          logged("client automatic handoff") && g_controlRevoked,
          "coordinated client automatic handoff recovers at 750ms without waiting for the native two-second fallback");
    ++headerMailbox.hostFocusReq;
    tick(local, 1, ERMC_CTRL_AUTO_RECOVERY);
    check(close(*(float*)(local.physics.data() + kPhysPos + 4), 80),
          "later focus-counter observation cannot repeat or undo the already completed automatic recovery");
    foregroundWindow = (HWND)2;
    tick(local, 1, drive);
    check(!local.pinned(), "automatic client handoff cannot be undone by resumed old MOVE packets");

    start_tick_fixture(local); fly();
    tick(local, 750, ERMC_CTRL_AUTO_RECOVERY);
    fixtureF8 = true;
    tick(local, 1, ERMC_CTRL_AUTO_RECOVERY);  // reason retained until Java observes acknowledgement
    fixtureF8 = false;
    check(!g_controlRevoked && headerMailbox.mcSwitchReq == 1,
          "retaining the automatic reason through ack cannot revoke an intentional native F8 retry");
#endif
    worldChrMan = nullptr;
}
static void test_peer_discovery_budget() {
    using namespace mb;
    Character local;
    PeerCharacter peer;
    start_tick_fixture(local);
    constexpr uint64_t id = 76561198000000002ull;
    g_peerLocalSteam = id - 1;
    peer.peer(id, 123);
    fixture_rtti((uint32_t)rva::kVtPlayerIns, 0x4000, 0x5000, ".?AVPlayerIns@CS@@");
    fixture_rtti(0x1000, 0x2000, 0x3000, ".?AVPlayerNetworkSession@CS@@");
    alignas(8) uintptr_t enemyVtable = g_base + rva::kVtEnemyIns;
    countedEnemy = (uintptr_t)&enemyVtable;
    std::vector<uint8_t> entries(400 * kChrByDistanceEntry);
    put(entries.data(), 0, peer.instance.data());
    for (size_t i = 1; i < 400; ++i) put(entries.data(), i * kChrByDistanceEntry, &enemyVtable);
    put(manager.data(), kChrsByDistance + 8, entries.data());
    put(manager.data(), kChrsByDistance + 0x10, entries.data() + entries.size());
    std::array<uint8_t, 16> players = {};
    put(players.data(), 0, peer.instance.data());
    put(peer.instance.data(), 0x10, players.data());
    put(manager.data(), kPlayerChrSet, g_base + rva::kVtChrSet);
    put(manager.data(), kPlayerChrSet + 0x10, uint32_t(1));
    put(manager.data(), kPlayerChrSet + 0x18, players.data());
    publish_mode(id);
    auto initialPlayer = local.player(); update_peer_visibility(&initialPlayer, true, true);
    unsigned initial = enemyIdentityReads;
    for (int i = 0; i < 5; ++i) {
        fixtureMs += 16; publish_mode(id);
        auto p = local.player(); update_peer_visibility(&p, true, true);
    }
    check(initial == 0 && enemyIdentityReads == 0,
          "dedicated player discovery never scans any of the 399 nearby enemy objects");
    fixtureMs += 20; publish_mode(id);
    auto p = local.player(); update_peer_visibility(&p, true, true);
    check(enemyIdentityReads == 0 && !peer.rendered(),
          "dedicated set keeps exact peer visibility without periodic NPC rescans");
    put(peer.instance.data(), 0x10, uintptr_t(0));
    publish_mode(id); update_peer_visibility(&p, true, true);
    check(!peer_info()->remoteSteamId, "broken reciprocal slot identity is rejected before a render write");
    put(peer.instance.data(), 0x10, players.data());
    game_shutdown(); countedEnemy = 0; worldChrMan = nullptr;
}
static void test_death_after_handoff() {
    using namespace mb;
    Character local;
    start_tick_fixture(local); tick(local, 1, drive);
    ++headerMailbox.hostFocusReq;
    ++headerMailbox.mcDeaths;
    tick(local, 1, 0);
    check(!g_expectDeath && !g_standing && local.rendered() && g_mcDeathsSeen == headerMailbox.mcDeaths,
          "F8 handoff consumes a queued MC death without killing the now native-controlled player");
    start_tick_fixture(local); tick(local, 1, drive);
    ++headerMailbox.mcDeaths;
    controlMailbox.mcFrame = 50;
    tick(local, 1, ERMC_CTRL_PASSIVE_COMPOSITE | ERMC_CTRL_COMPOSITE);
    check(!g_expectDeath && !g_standing && local.rendered() && g_mcDeathsSeen == headerMailbox.mcDeaths,
          "passive native control ignores an offscreen MC death during the old stand-in grace");
    worldChrMan = nullptr;
}
#endif

static std::array<float, 3> route_to_outdoors(bool coop, uint32_t startBlock) {
    using namespace mb;
    reset_frame(coop);
    Character local; local.init(startBlock);
    auto p = local.player();
    update_frame(p, false);
    local.position(0x3c2a2400, {2, 80, 3}, {34, 48, 179});
    update_frame(p, true);
    std::array<float, 3> published = {};
    to_stable((const float*)(p.phys + kPhysPos), published.data());
    float blockStable[3];
    const uint32_t zone = stable_pos(p, blockStable);
    if (coop) check(zone == 0x3c000000 && g_frame.zone == zone && same(published.data(), blockStable),
                    "co-op transition uses the same canonical raw zone for block and published physics coordinates");
    else check(zone == startBlock && g_frame.zone == startBlock,
               "solo transition preserves the original stitched zone");
    return published;
}
static void test_canonical_routes_and_reload() {
    using namespace mb;
    const float canonical[3] = {10754, 80, 9219};
    auto cave = route_to_outdoors(true, 0x0a010000);
    auto dungeon = route_to_outdoors(true, 0x12000000);
    check(same(cave.data(), canonical) && same(dungeon.data(), canonical) && same(cave.data(), dungeon.data()),
          "two unrelated routes into the same raw zone converge to identical canonical coordinates");
    reset_frame(true);
    Character outdoors; outdoors.init();
    auto p = outdoors.player();
    outdoors.position(0x3c2a2400, {2, 80, 3}, {-14, -8, 59});
    update_frame(p, false);
    float straight[3]; to_stable((const float*)(p.phys + kPhysPos), straight);
    check(g_frame.zone == 0x3c000000 && same(straight, cave.data()),
          "a PC starting outdoors agrees despite an unrelated Havok origin");
    auto solo = route_to_outdoors(false, 0x0a010000);
    const float stitched[3] = {34, 48, 179};
    check(same(solo.data(), stitched) && !same(solo.data(), canonical),
          "solo keeps the previous offset and stitched coordinates exactly");

    reset_frame(true); outdoors.init(); p = outdoors.player(); update_frame(p, false);
    outdoors.position(0x3c2b2400, {-254, 80, 3}, {2, 80, 3});
    update_frame(p, true);
    float tileStable[3]; stable_pos(p, tileStable);
    check(g_frame.zone == 0x3c000000 && same(tileStable, canonical),
          "open-world tile changes normalize to the same area frame without a zone reset");

    TaskPage persisted = {};
    persisted.frameMagic = kFrameMagic; persisted.frameRawZone = 0x3c000000;
    persisted.frameZone = 0x0a010000;
    persisted.frameBias[0] = -10720; persisted.frameBias[1] = -32; persisted.frameBias[2] = -9040;
    reset_frame(true); outdoors.init(); p = outdoors.player(); g_page = &persisted;
    update_frame(p, false);
    float reloadStable[3]; stable_pos(p, reloadStable);
    check(g_frame.zone == 0x3c000000 && same(reloadStable, canonical) && persisted.frameMagic != kFrameMagic,
          "co-op ignores a solo/old-core stitched hot-reload record and saves a distinct frame marker");
    reset_frame(true); g_page = &persisted;
    update_frame(p, false);
    stable_pos(p, reloadStable);
    check(g_frame.zone == 0x3c000000 && same(reloadStable, canonical),
          "co-op hot reload remains canonical when its own record is present");

    persisted.frameMagic = kFrameMagic; persisted.frameRawZone = 0x3c000000;
    persisted.frameZone = 0x0a010000;
    persisted.frameBias[0] = -10720; persisted.frameBias[1] = -32; persisted.frameBias[2] = -9040;
    reset_frame(false); g_page = &persisted; update_frame(p, false);
    stable_pos(p, reloadStable);
    check(g_frame.zone == 0x0a010000 && same(reloadStable, stitched) && persisted.frameMagic == kFrameMagic,
          "solo hot reload still restores existing stitching in the original TaskPage layout");
    g_page = nullptr;
}
static void test_pinned_recentre_and_lag() {
    using namespace mb;
    for (bool coop : {false, true}) {
        reset_frame(coop);
        Character local; local.init(); auto p = local.player(); update_frame(p, false);
        float before[3]; to_stable((const float*)(p.phys + kPhysPos), before);
        float oldOffset[3]; memcpy(oldOffset, g_frame.offset, 12);
        memcpy(g_putHavok, p.phys + kPhysPos, 12); g_putHavokValid = true;
        local.position(0x3c2a2400, {2, 80, 3}, {10, 112, -13});
        update_frame(p, true);
        float after[3]; to_stable((const float*)(p.phys + kPhysPos), after);
        check(same(before, after) && g_putHavokValid && close(g_frame.offset[0], oldOffset[0] + 8) &&
              close(g_frame.offset[1], oldOffset[1] + 32) && close(g_frame.offset[2], oldOffset[2] - 16),
              coop ? "co-op pinned recentre preserves stable coordinates and trusted flight pose" :
                     "solo pinned recentre preserves stable coordinates and trusted flight pose");
        memcpy(g_putHavok, p.phys + kPhysPos, 12);
        memcpy(oldOffset, g_frame.offset, 12);
        bool stableFlight = true;
        bool livingFlight = true;
        g_life = LIFE_ALIVE; g_lifeTicked = true; g_aliveZone = g_frame.zone;
        g_mcDeathsInit = false; g_expectDeath = false; g_clampFrames = 0;
        g_standing = g_havePut = true;
        for (int tickIndex = 0; tickIndex < 120; ++tickIndex) {
            // Physics advances, block coordinates lag by 0.75m. Do not turn
            // this feedback into a frame shift or a remembered flight floor.
            const float height = 112 + (tickIndex + 1) * 0.75f;
            put(local.physics.data(), kPhysPos + 4, height);
            memcpy(g_putHavok, p.phys + kPhysPos, 12); g_putHavokValid = true;
            update_frame(p, true);
            to_stable((const float*)(p.phys + kPhysPos), g_lastPut);
            float lifeStable[3]; stable_pos(p, lifeStable);
            update_life(&p);
            livingFlight &= g_life == LIFE_ALIVE && same(lifeStable, g_lastPut);
            stableFlight &= same(oldOffset, g_frame.offset) && g_putHavokValid;
            if (coop) stableFlight &= g_bias[0] == 0 && g_bias[1] == 0 && g_bias[2] == 0;
        }
        check(stableFlight, coop ? "co-op lagging block coordinates cannot accumulate bias or flight offset" :
                                 "solo flight offset stabilization remains unchanged");
        check(livingFlight, "pinned flight uses the published physical frame without false warps/settling");
        put(local.physics.data(), kPhysPos, 21.25f);
        update_frame(p, true);
        check(!g_putHavokValid && same(oldOffset, g_frame.offset),
              "a non-grid engine warp invalidates the pinned write without changing its frame offset");
    }
    g_standing = g_havePut = false;
}
static void test_canonical_zone_tick() {
    using namespace mb;
    Character local;
    start_tick_fixture(local);
    tick(local, 1, drive);
    local.position(0x12000000, {20, 80, 5}, {52, 48, 181});
    tick(local, 1, drive);
    check(g_frame.zone == 0x12000000 && g_life == LIFE_SETTLING && local.rendered() &&
          !local.pinned() && (stateMailbox.flags & ERMC_STATE_HOST_BUSY) &&
          !(stateMailbox.flags & ERMC_STATE_PLAYER_VALID),
          "canonical raw-zone transition releases the body and publishes busy while coordinates settle");
    const uint32_t hostLifeBefore = headerMailbox.hostLife;
    for (int i = 0; i < 10; ++i) tick(local, 16, drive);
    const float canonical[3] = {20, 80, 5};
    check(g_life == LIFE_ALIVE && stateMailbox.stageId == 0x12000000 &&
          (stateMailbox.flags & ERMC_STATE_PLAYER_VALID) &&
          !(stateMailbox.flags & ERMC_STATE_HOST_BUSY) && same(stateMailbox.playerPos, canonical),
          "production state publication becomes valid in the destination's canonical map frame");
    check(headerMailbox.hostLife == hostLifeBefore,
          "a seamless raw-zone transition uses the existing short settle without inventing a respawn");
    worldChrMan = nullptr;
}
int main() {
    mb::g_base = (uintptr_t)&fixture_cast - mb::rva::kCastRay;
    fixtureImageBase = mb::g_base;
    worldChrGlobal = mb::g_base + mb::rva::kWorldChrMan;
    havokGlobal = mb::g_base + mb::rva::kCSHavokMan;
    mb::g_ok = true;
    test_visibility_tick_lifecycle();
#ifndef ERMC_BASELINE_TEST
    ErmcControl passive = {};
    passive.mcFrame = 400;
    passive.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_PASSIVE_COMPOSITE;
    check(mb::passive_composite_control(passive) && mb::composite_frame_target(passive, 200) == 400,
          "passive frame selection uses the published native-camera frame, not the last overridden pose");
    bool ownershipRejected = true;
    for (uint32_t bit : {ERMC_CTRL_OVERRIDE_CAMERA, ERMC_CTRL_MOVE_HUNTER, ERMC_CTRL_HIDE_HUNTER,
                         ERMC_CTRL_HOLD_HUNTER, ERMC_CTRL_FREE_FLIGHT}) {
        auto malformed = passive; malformed.flags |= bit;
        ownershipRejected &= !mb::composite_control_valid(malformed) && mb::composite_frame_target(malformed, 200) == 0;
    }
    check(ownershipRejected, "passive composition rejects every camera/body ownership bit");
    passive.mcFrame = 0;
    check(!mb::passive_composite_control(passive), "passive rendering requires a completed nonzero frame");
    passive.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_OVERRIDE_CAMERA;
    check(mb::composite_frame_target(passive, 200) == 200, "active rendering preserves native applied-pose history");
    test_hold_tick_lifecycle();
    test_bounded_hold_recovery();
    test_placement_runtime();
    test_auto_air_recovery();
    test_peer_sidecar_tick();
    test_peer_rebind_tick();
    test_peer_owner_replacement_and_capacity();
    test_peer_discovery_budget();
    test_death_after_handoff();
#endif
    test_canonical_routes_and_reload();
    test_pinned_recentre_and_lag();
    test_canonical_zone_tick();
    std::printf("Native visibility/frame contract: %d checks, %d failures.\n", checks, failures);
    return failures ? 1 : 0;
}
