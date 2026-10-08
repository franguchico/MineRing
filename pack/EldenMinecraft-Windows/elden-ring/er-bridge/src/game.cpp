// Elden Ring-specific memory access and hooks (eldenring.exe 2.7.1.0 = App Ver. 1.17.1).
// Addresses and layouts were found by reverse engineering this build and verified live; each is
// checked against its byte signature at startup.
//
// No inline hooks: per-frame code runs as tasks registered with the game's own task system
// (CSTaskImp::RegisterTask), so Arxan's code restoration can't undo anything. The task
// objects and their tiny forwarding stubs live in one RWX page that survives core hot
// reloads (its address is kept in the shared-memory header); the core only swaps the
// function pointers the stubs jump to.
//
// Coordinates: Elden Ring's physics ("Havok") space moves as the world shifts, so everything
// published to Minecraft is in a stable frame per zone:
//   - open world (areas 60/61): global = block-relative + 256 * (gridX, 0, gridZ), zone = area << 24
//   - everything else (legacy dungeons, caves...): block-relative, zone = full block id
// offset = player Havok position - player stable position, re-derived every tick.
//
// Life: one life shared by both games. Minecraft's health is the player's health (the
// standing-in Tarnished never dies of damage: NoDead, HP refilled, hits reported). A death
// on either side kills the other one too, and Elden Ring's own death and respawn at the last
// Site of Grace is the respawn for both: Minecraft draws nothing while the Tarnished is dead
// or loading, and moves its player to the Tarnished once it is usable again (hostLife).
#include "common.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "consumer_liveness.h"
#include "movement_safety.h"
#include "action_detour_contract.h"
#include "local_player_visibility.h"
#include "peer_visibility.h"
#include "composite_mode.h"
#ifndef ERMC_NATIVE_WINDOWS
#if defined(_MSC_VER)
#define ERMC_NATIVE_WINDOWS 1
#else
#define ERMC_NATIVE_WINDOWS 0
#endif
#endif

namespace mb {

// A queried region can be unmapped by streaming before a later direct access.
// Keep SEH in a separate thunk so the caller's inflight/readability RAII always
// runs normally. Catch memory faults only, never unrelated game exceptions.
static bool guarded_game_call(void (*body)(void*), void* context) {
#if defined(_MSC_VER) && ERMC_NATIVE_WINDOWS
    __try {
        body(context);
        return true;
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                 GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
                 GetExceptionCode() == EXCEPTION_GUARD_PAGE)
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        mem_readability_invalidate();
        return false;
    }
#else
    body(context);
    return true;
#endif
}

namespace rva {
constexpr uintptr_t kWorldChrMan = 0x3D69FF8;   // static WorldChrManImp*
constexpr uintptr_t kCSCamera = 0x3D669C8;      // static CSCamera*
constexpr uintptr_t kCSHavokMan = 0x3D7A0D0;    // static CSHavokMan*
constexpr uintptr_t kCSTaskImp = 0x458FE88;     // static CSTaskImp*
constexpr uintptr_t kRegisterTask = 0xEB3E50;   // bool (CSTaskImp*, u32 group, FD4TaskBase*)
constexpr uintptr_t kCastRay = 0xC71E00;        // bool (CSPhysWorld*, u32 filter, vec4* origin, vec4* delta, vec4* hit, ChrIns* ignore)
constexpr uintptr_t kKill = 0x3EE730;           // void ChrIns::Kill(ChrIns*, u8) (the game's debug "kill main player" calls it with 0)
constexpr uintptr_t kCSActionButtonMan = 0x3D72478;  // static CSActionButtonManImp*
constexpr uintptr_t kMsgRepository = 0x3D81568;      // static MsgRepositoryImp*
constexpr uintptr_t kWorldGeomMan = 0x3D6DC18;       // static CSWorldGeomManImp*: map assets per block
// vtables (RTTI) used to validate pointers before touching them
constexpr uintptr_t kVtWorldChrMan = 0x2A4E980;
constexpr uintptr_t kVtPlayerIns = 0x2A7FBB0;
constexpr uintptr_t kVtChrSet = 0x2A41348;
constexpr uintptr_t kVtEnemyIns = 0x2A47090;
constexpr uintptr_t kVtPersCam = 0x2AA0A40;
constexpr uintptr_t kVtTaskImp = 0x2C04B00;
constexpr uintptr_t kVtDataModule = 0x2A380B8;
constexpr uintptr_t kVtPhysModule = 0x2A3C890;
constexpr uintptr_t kVtActionButtonMan = 0x2B48140;
constexpr uintptr_t kVtMsgRepository = 0x2BAD530;
constexpr uintptr_t kVtWorldGeomMan = 0x2A89130;
constexpr uintptr_t kVtGeomDynamicIns = 0x2A87278;        // CSWorldGeomDynamicIns (movable/usable assets)
constexpr uintptr_t kVtAnimSkeletonModifier = 0x2B6F1A0;  // CSFD4LocationAnimSkeletonToMdlObjIdxModifier
}  // namespace rva

// WorldChrMan
constexpr size_t kMainPlayer = 0x1E508;
constexpr size_t kPlayerChrSet = 0x10EE0;  // embedded ChrSet; capacity +0x10, slots +0x18
constexpr size_t kChrsByDistance = 0x1F1D0;  // vector<{ChrIns*, f32 distance, u32 kind}>: begin +8, end +0x10, sorted
constexpr size_t kChrByDistanceEntry = 0x10;
// ChrIns / PlayerIns
constexpr size_t kChrModules = 0x190;
constexpr size_t kChrRenderFlags = 0x1C5;   // bit3 enable_render, bit7 death_flag
constexpr size_t kChrDebugFlags = 0x538;    // bit5 NoMove
constexpr size_t kPlayerBlockPos = 0x6C0;   // vec3 block-relative + yaw
constexpr size_t kPlayerBlockId = 0x6D0;
constexpr size_t kChrNpcParamId = 0x60;
constexpr size_t kChrModelId = 0x64;          // c#### character model number
constexpr size_t kChrTeamType = 0x6C;         // 6 enemy, 7 strong enemy/boss (TEAM_TYPE)
// modules
constexpr size_t kModData = 0x00;
constexpr size_t kModPhysics = 0x68;
// CSChrDataModule
constexpr size_t kDataHp = 0x138;
constexpr size_t kDataMaxHp = 0x13C;
constexpr size_t kDataFlags = 0x19B;        // bit0 NoDead
// CSChrPhysicsModule
constexpr size_t kPhysQuat = 0x50;
constexpr size_t kPhysQuatInterp = 0x60;
constexpr size_t kPhysPos = 0x70;
constexpr size_t kPhysPosLast = 0x80;
constexpr size_t kPhysProxyUpdate = 0x91;
constexpr size_t kPhysGravityOff = 0x1D3;
constexpr size_t kPhysHitHeight = 0x2E0;      // character capsule (the player's: 1.5 x 0.4)
constexpr size_t kPhysHitRadius = 0x2E4;
// CSCamera / CSPersCam
constexpr size_t kCamPers1 = 0x08;
constexpr size_t kCamMatrix = 0x10;         // rows: right, up, forward, position (vec4 each)
constexpr size_t kCamFov = 0x50;            // vertical, radians
constexpr size_t kCamAspect = 0x54;
constexpr size_t kCamNear = 0x58;
constexpr size_t kCamFar = 0x5C;
// CSHavokMan
constexpr size_t kHavokPhysWorld = 0x98;

// Task groups (fromsoftware-rs CSTaskGroupIndex)
constexpr uint32_t kGroupDrawParamUpdate = 107;       // right after CameraStep (106)
constexpr uint32_t kGroupWorldChrManPostPhysics = 117;

// Default ray filter: the one the game's only CastRay caller uses (hits map and characters;
// our own player is ignored through the "ignore" argument).
constexpr uint32_t kDefaultRayFilter = 0x2000058;
// Terrain rays for Minecraft: map geometry and props, but not characters (verified live
// In the chapel: 0x5D/0x63 hit benches and walls like 0x2000058 but pass through
// the Tarnished; the camera's 0x5B also passes through props). Enemies are Minecraft entities,
// not terrain, so they must not leave block pillars behind.
constexpr uint32_t kTerrainRayFilter = 0x5D;

static uintptr_t g_base = 0;
static bool g_ok = false;
static bool g_coopExcludePlayerIns = false;

struct CodeSig {
    uintptr_t rva;
    const char* sig;
    const char* name;
};

static const CodeSig kSigs[] = {
    {rva::kRegisterTask, "48 89 5C 24 08 57 48 83 EC 40 48 8D 4C 24 20 49 8B D8 8B FA E8", "CSTaskImp::RegisterTask"},
    {rva::kCastRay, "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00", "CSPhysWorld::CastRay"},
    {rva::kKill, "48 89 5C 24 08 57 48 83 EC 30 F3 0F 10 05 ?? ?? ?? ?? 48 8B D9 48 8B 89 90 01 00 00 0F B6 FA C6 44 24 28 01", "ChrIns::Kill"},
};

static bool bytes_match(uintptr_t addr, const char* sig) {
    const uint8_t* p = (const uint8_t*)addr;
    size_t i = 0;
    for (const char* s = sig; *s;) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') {
            s += (s[1] == '?') ? 2 : 1;
        } else {
            char hex[3] = {s[0], s[1], 0};
            if (!mem_readable(p + i, 1) || p[i] != (uint8_t)strtol(hex, nullptr, 16)) return false;
            s += 2;
        }
        i++;
    }
    return true;
}

// Pointer with the expected vtable (rva), or null.
static uint8_t* typed(void* p, uintptr_t vtRva) {
    if (!p || !mem_readable(p, 8)) return nullptr;
    return (*(uintptr_t*)p == g_base + vtRva) ? (uint8_t*)p : nullptr;
}

static uint8_t* global_ptr(uintptr_t rvaAddr, uintptr_t vtRva) {
    void* p = nullptr;
    if (!mem_read((void*)(g_base + rvaAddr), &p, 8)) return nullptr;
    return vtRva ? typed(p, vtRva) : (uint8_t*)p;
}

// ---------------------------------------------------------------------------------------
// Player and coordinate frame

struct Player {
    uint8_t* ins;
    uint8_t* data;
    uint8_t* phys;
};

static bool get_player(Player* out) {
    uint8_t* wcm = global_ptr(rva::kWorldChrMan, rva::kVtWorldChrMan);
    if (!wcm) return false;
    uint8_t* pl = typed(*(void**)(wcm + kMainPlayer), rva::kVtPlayerIns);
    if (!pl || !mem_readable(pl + kChrModules, 8)) return false;
    uint8_t* mods = *(uint8_t**)(pl + kChrModules);
    if (!mods || !mem_readable(mods, 0x70)) return false;
    out->ins = pl;
    out->data = typed(*(void**)(mods + kModData), rva::kVtDataModule);
    out->phys = typed(*(void**)(mods + kModPhysics), rva::kVtPhysModule);
    return out->data && out->phys;
}

struct TaskPage {
    uint64_t magic;
    void* target[2];            // core functions (null = no-op)
    void* vtbl[2][3];           // get_runtime_class, destructor, execute
    struct {
        void** vtbl;
        uint64_t unk8;
        uint64_t pad[6];
    } task[2];
    uint8_t stubRet0[4];        // xor eax,eax; ret
    uint8_t stubRet[4];         // ret
    uint8_t stubExec[2][16];    // mov rax,[rip+target[i]]; test rax,rax; jz +2; jmp rax; ret
    // Frame continuity across core reloads (see g_bias). Pages made by older cores read 0 here.
    uint32_t frameMagic;
    uint32_t frameZone;
    uint32_t frameRawZone;
    float frameBias[3];
};
static TaskPage* g_page = nullptr;
constexpr uint32_t kFrameMagic = 0x314D5246;  // "FRM1"
constexpr uint32_t kCoopFrameMagic = 0x31435246;  // "FRC1": canonical, never stitched

struct Frame {
    bool valid;
    uint32_t zone;
    float offset[3];  // Havok = stable + offset
};

static Frame g_frame = {false, 0, {0, 0, 0}};

// Loading screens report block id -1 (and the character may still exist).
static bool block_valid(const Player& p) {
    uint32_t blk = *(uint32_t*)(p.ins + kPlayerBlockId);
    return blk != 0xFFFFFFFFu && blk != 0;
}

// Seamless block changes. Where maps meet, Elden Ring moves the player to another map block
// without a loading screen (the Stranded Graveyard's cave belongs partly to open-world tile
// m60_42_36; legacy dungeons join the open world). Each block has its own coordinate frame, so
// Minecraft saw a new zone and sent Steve to another region 32768 blocks away. Now
// such a change keeps the zone: the new block's coordinates are shifted by g_bias to continue the
// old ones in solo play. Co-op uses the raw map frame so different routes on
// different PCs agree on both the zone and coordinates. A
// loading screen (no frame in between) or a new character starts a fresh zone.
static float g_bias[3] = {0, 0, 0};
static uint32_t g_rawZone = 0;         // the frame of the player's block coordinates
static uint8_t* g_framePlayer = nullptr;
// The Havok position the stand-in wrote last tick (valid while it pins the Tarnished).
static float g_putHavok[3];
static bool g_putHavokValid = false;

// The player's position in its block's own frame (open world: global), and that frame's zone.
static uint32_t raw_stable_pos(const Player& p, float* stable) {
    uint32_t blk = *(uint32_t*)(p.ins + kPlayerBlockId);
    const float* bpos = (const float*)(p.ins + kPlayerBlockPos);
    uint32_t area = blk >> 24;
    stable[0] = bpos[0], stable[1] = bpos[1], stable[2] = bpos[2];
    if (area == 60 || area == 61) {
        stable[0] += 256.0f * (float)((blk >> 16) & 0xFF);
        stable[2] += 256.0f * (float)((blk >> 8) & 0xFF);
        return area << 24;
    }
    return blk;
}

// The player's position in the published (stable) frame, and the published zone (see g_bias).
static uint32_t stable_pos(const Player& p, float* stable) {
    uint32_t raw = raw_stable_pos(p, stable);
    if (!g_frame.valid || raw != g_rawZone) return raw;  // update_frame() hasn't seen this block yet
    if (g_coopExcludePlayerIns) {
        // Use the same canonical physical frame as state publication. The block
        // position can lag a pinned/airborne body; interpreting that lag as a
        // game warp would repeatedly enter settling and clear entity proxies.
        const float* hpos = (const float*)(p.phys + kPhysPos);
        for (int i = 0; i < 3; i++) stable[i] = hpos[i] - g_frame.offset[i];
        return raw;
    }
    for (int i = 0; i < 3; i++) stable[i] += g_bias[i];
    return g_frame.zone;
}

static void save_frame() {
    if (!g_page) return;
    g_page->frameZone = g_frame.zone;
    g_page->frameRawZone = g_rawZone;
    memcpy(g_page->frameBias, g_bias, 12);
    g_page->frameMagic = g_coopExcludePlayerIns ? kCoopFrameMagic : kFrameMagic;
}

// Last tick's Havok minus block position, to see Havok re-centring while the game moves the
// character (a re-centre moves Havok coordinates, never block coordinates).
static float g_lastHmR[3];
static bool g_haveHmR = false;

// Whole multiples of 8 m per axis, each within tol of d; false if an axis is off by more.
static bool recentre_shift(const float* d, float tol, float* shift) {
    for (int i = 0; i < 3; i++) {
        float r = roundf(d[i] / 8.0f) * 8.0f;
        if (fabsf(d[i] - r) > tol) return false;
        shift[i] = r;
    }
    return true;
}

// The frame. Within a zone the offset only ever changes by Havok re-centring, whole multiples of
// 8 m: the block position is used to start a zone, never to track it. (In the Stranded Graveyard
// it lags the position the stand-in writes; re-deriving the offset from it each tick fed the lag
// back, and a runaway lifted Steve 13 m and threw the Tarnished 900 m up.)
// - Pinned by the stand-in: a re-centre moves the Tarnished's Havok position away from what we
//   wrote by exactly the shift, while its block position stays. Any other jump is the game moving
//   it (update_life sees that as a warp); the stand-in then stops trusting its last write.
// - Moved by the game: Havok minus block position jumps by the shift.
// seamlessOk: the Tarnished is alive and in use, so a block change is a walk across a map
// border (while settling after a load it may still be at a temporary spot).
static void update_frame(const Player& p, bool seamlessOk) {
    uint32_t blk = *(uint32_t*)(p.ins + kPlayerBlockId);
    const float* hpos = (const float*)(p.phys + kPhysPos);
    float raw[3];
    uint32_t rawZone = raw_stable_pos(p, raw);
    float hmr[3] = {hpos[0] - raw[0], hpos[1] - raw[1], hpos[2] - raw[2]};
    bool sameChar = g_frame.valid && p.ins == g_framePlayer;
    bool sameBlock = sameChar && rawZone == g_rawZone;
    Frame f = g_frame;
    f.valid = true;
    if (sameChar && (sameBlock || (seamlessOk && !g_coopExcludePlayerIns))) {
        float shift[3] = {0, 0, 0};
        bool warp = false;
        float dh[3] = {0, 0, 0};
        if (g_putHavokValid) {
            for (int i = 0; i < 3; i++) dh[i] = hpos[i] - g_putHavok[i];
            float len = fabsf(dh[0]) + fabsf(dh[1]) + fabsf(dh[2]);
            if (len > 0.5f) {
                // A re-centre: whole 8 m steps, and the block position didn't move with it.
                float dhmr[3], s2[3];
                for (int i = 0; i < 3; i++) dhmr[i] = hmr[i] - g_lastHmR[i];
                bool blockStayed = !sameBlock || !g_haveHmR || recentre_shift(dhmr, 1.5f, s2);
                if (!recentre_shift(dh, 1.5f, shift) || !blockStayed) {
                    shift[0] = shift[1] = shift[2] = 0;
                    warp = true;
                }
            }
        } else if (sameBlock && !seamlessOk) {
            // Settling or dying: the game places the character itself, so follow its block
            // position (the settle check waits for this offset to hold still).
            for (int i = 0; i < 3; i++) shift[i] = hpos[i] - raw[i] - g_bias[i] - g_frame.offset[i];
            if (fabsf(shift[0]) + fabsf(shift[1]) + fabsf(shift[2]) < 0.01f) shift[0] = shift[1] = shift[2] = 0;
        } else if (sameBlock && g_haveHmR) {
            float d[3];
            for (int i = 0; i < 3; i++) d[i] = hmr[i] - g_lastHmR[i];
            if (!recentre_shift(d, 1.5f, shift)) shift[0] = shift[1] = shift[2] = 0;
        }
        for (int i = 0; i < 3; i++) f.offset[i] = g_frame.offset[i] + shift[i];
        if (shift[0] != 0 || shift[1] != 0 || shift[2] != 0)
            log("frame: world shift in zone %08x (offset %.1f %.1f %.1f)", f.zone, f.offset[0], f.offset[1], f.offset[2]);
        if (warp) g_putHavokValid = false;  // the game moved it: don't put it back at Steve's feet
        if (!sameBlock)
            log("frame: block %08x -> %08x without a loading screen: zone %08x continues", g_rawZone, rawZone, f.zone);
        // Block coordinates of this block -> the published frame (stable_pos, the reach list).
        // In co-op block coordinates stay canonical. Keep the existing offset
        // / recentre detection within this raw zone: following lagging block
        // coordinates every tick would reintroduce the free-flight runaway.
        for (int i = 0; i < 3; i++)
            g_bias[i] = g_coopExcludePlayerIns ? 0.0f : hpos[i] - f.offset[i] - raw[i];
    } else {
        g_putHavokValid = false;
        if (!g_coopExcludePlayerIns && !g_framePlayer && g_page &&
            g_page->frameMagic == kFrameMagic && g_page->frameRawZone == rawZone) {
            // First frame of a hot-reloaded core: continue the old core's frame, or a zone kept
            // across a map border would restart as the block's own, sending Steve to another region.
            memcpy(g_bias, g_page->frameBias, 12);
            f.zone = g_page->frameZone;
            log("frame: continuing zone %08x after a core reload", f.zone);
        } else {
            g_bias[0] = g_bias[1] = g_bias[2] = 0;
            f.zone = rawZone;
        }
        for (int i = 0; i < 3; i++) f.offset[i] = hpos[i] - raw[i] - g_bias[i];
        log("frame: zone %08x (block %08x), Havok offset %.1f %.1f %.1f", f.zone, blk, f.offset[0], f.offset[1], f.offset[2]);
    }
    memcpy(g_lastHmR, hmr, 12);
    g_haveHmR = true;
    g_rawZone = rawZone;
    g_framePlayer = p.ins;
    g_frame = f;
    save_frame();
}

static void to_havok(const float* s, float* h) {
    for (int i = 0; i < 3; i++) h[i] = s[i] + g_frame.offset[i];
}
static void to_stable(const float* h, float* s) {
    for (int i = 0; i < 3; i++) s[i] = h[i] - g_frame.offset[i];
}

// ---------------------------------------------------------------------------------------
// Control from Minecraft

// The tick and camera tasks can run on different game threads. Their snapshots and
// heartbeat observations must not overwrite each other's cache.
static thread_local uint32_t g_lastCtrlSeq = 0;
static thread_local uint64_t g_lastCtrlChangeMs = 0;
static thread_local ErmcControl g_lastCtrl;
static thread_local bool g_haveLastCtrl = false;

static bool mc_consumer_active() {
#if ERMC_NATIVE_WINDOWS
    static thread_local McHeartbeatWatch consumer;
    ErmcHeader* h = shm_header();
    return consumer.update(h->mcPid, h->mcStartMs, h->mcHeartbeat, GetTickCount64());
#else
    return true;  // retain the existing CrossOver behavior
#endif
}

static bool control_active(ErmcControl* out) {
    if (!mc_consumer_active()) {
        g_haveLastCtrl = false;
        g_lastCtrlSeq = 0;
        g_lastCtrlChangeMs = 0;
        return false;
    }
    ErmcControl c;
    memset(&c, 0, sizeof(c));
    uint64_t now = now_ms();
    if (control_snapshot(&c)) {
        g_lastCtrl = c;
        g_haveLastCtrl = true;
        if (c.seq != g_lastCtrlSeq) {
            g_lastCtrlSeq = c.seq;
            g_lastCtrlChangeMs = now;
        }
    } else if (!g_haveLastCtrl) {
        return false;
    }
    // A torn read (Minecraft paused mid-write, e.g. by the JVM) keeps the last good pose
    // instead of dropping control for a frame, which would flash the Tarnished into view.
    if (now - g_lastCtrlChangeMs > 1000) return false;  // Minecraft stopped updating
    *out = g_lastCtrl;
    return true;
}

// ---------------------------------------------------------------------------------------
// Camera

static volatile LONG g_overrideApplied = 0;
// HOLD timeout revokes both tasks until an explicit native F8 retry. Camera and
// tick tasks may run on different threads, so publish the revocation atomically.
static volatile LONG g_controlRevoked = 0;
static volatile LONG g_bodyControlAccepted = 0;
static bool g_autoRecoverySeen = false;  // tick-owned reason edge, not the separate focus counter
static uint32_t g_handoffSeqSeen = 0, g_handoffPid = 0;
static uint64_t g_handoffStart = 0, g_handoffReadAt = 0;
static bool g_handoffReading = false, g_suppressMcDeaths = false;
static bool g_haveTerminalHandoff = false;
static uint32_t g_terminalFocusReq = 0, g_terminalResumeReq = 0;
static uint64_t g_terminalHostStart = 0;
static bool handoff_snapshot(ErmcHandoff* out);
static constexpr uint64_t kHandoffReadLimitMs = 100;

// Tick-owned, process-bound explicit command ledger. Connection epochs are
// identities, not counters: only a committed binding may replace one.
struct ExplicitResetLedger {
    uint32_t pid = 0, stamp = 0;
    uint64_t mcStart = 0, hostStart = 0, connection = 0, event = 0;
    uint64_t retired[256] = {};
    unsigned retiredCount = 0;
    bool exhausted = false;
    bool is_retired(uint64_t id) const {
        for (unsigned i = 0; i < retiredCount; ++i) if (retired[i] == id) return true;
        return false;
    }
    bool retire_current() {
        if (!connection) return true;
        if (retiredCount == 256) {
            exhausted = true; connection = event = 0;
            return false;
        }
        retired[retiredCount++] = connection;
        connection = event = 0;
        return true;
    }
};
static ExplicitResetLedger g_explicitReset;

static void normalize(float* v) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}
static void cross(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static uint8_t* final_camera() {
    uint8_t* cs = global_ptr(rva::kCSCamera, 0);
    if (!cs || !mem_readable(cs, 0x30)) return nullptr;
    return typed(*(void**)(cs + kCamPers1), rva::kVtPersCam);
}

// Runs in DrawParamUpdate, right after the game resolved its camera for this frame.
static void apply_camera(const ErmcControl& c) {
    if (InterlockedCompareExchange(&g_controlRevoked, 0, 0) || !g_frame.valid ||
        !(c.flags & ERMC_CTRL_OVERRIDE_CAMERA) || (c.flags & ERMC_CTRL_PASSIVE_COMPOSITE)) return;
    if ((c.flags & (ERMC_CTRL_MOVE_HUNTER | ERMC_CTRL_HOLD_HUNTER)) &&
        !InterlockedCompareExchange(&g_bodyControlAccepted, 0, 0)) return;
    uint8_t* cam = final_camera();
    if (!cam) return;
    float pos[3], tgt[3];
    to_havok(c.camPos, pos);
    to_havok(c.camTarget, tgt);
    float fwd[3] = {tgt[0] - pos[0], tgt[1] - pos[1], tgt[2] - pos[2]};
    normalize(fwd);
    float up[3] = {c.camUp[0], c.camUp[1], c.camUp[2]};
    float right[3];
    cross(up, fwd, right);  // left-handed: right = up x forward
    normalize(right);
    cross(fwd, right, up);
    float m[16] = {right[0], right[1], right[2], 0, up[0], up[1], up[2], 0,
                   fwd[0], fwd[1], fwd[2], 0, pos[0], pos[1], pos[2], 1};
    memcpy(cam + kCamMatrix, m, sizeof(m));
    if (c.fovYDeg > 5.0f && c.fovYDeg < 170.0f) *(float*)(cam + kCamFov) = c.fovYDeg * 3.14159265f / 180.0f;
    compositor_note_applied_pose(c.mcFrame);
    InterlockedExchange(&g_overrideApplied, 1);
}

// ---------------------------------------------------------------------------------------
// The player character stands in for Steve: hidden, pinned to his feet, never dies; its HP
// loss is reported to Minecraft as damage and refilled.

// Nearest hostile character published this tick (see publish_entities), for blaming hits.
static struct {
    bool valid;
    float pos[3];
    uint32_t kind;
    uint64_t handle;
} g_nearestHostile = {false, {0, 0, 0}, 0, 0};

// The attacker's position and kind from last tick's entity table (defined with the entities).
static bool published_attacker(uint64_t handle, float* pos, uint32_t* kind);
// Removes our debug test enemy, if any (defined with the entities).
static void remove_test_enemy(const char* why);

static bool g_standing = false;
static bool g_holding = false;
static StandInHoldLease g_holdLease;
static LocalPlayerVisibility g_visibility;
static uint8_t* g_standPlayer = nullptr;
// HOLD can continue only the exact body/session/map last accepted by MOVE.
static uint8_t* g_standData = nullptr;
static uint8_t* g_standPhys = nullptr;
static uint64_t g_standHandle = 0;
static uint32_t g_standZone = 0, g_standLife = 0;
static bool g_lastDriveFreeFlight = false;
static int g_lastHp = -1;
static uint64_t g_lastStandMs = 0;   // last tick the Tarnished stood in (Minecraft in control)
static float g_lastPut[3];           // stable position it was pinned to last tick
static bool g_havePut = false;
static bool g_movementCorrected = false;
static float g_correctedFeet[3];
static int g_clampFrames = 0;        // consecutive ticks the game knocked HP down to 1 (see stand_in)
static void return_to_host(const char* why);
static bool recover_auto_handoff(const Player& p, const char* why);
// Runtime observation only. No saved position, no stale-pointer capability.
// Ownership keys are the existing stand-in keys and must match again at use.
static bool g_haveRecoveryAnchor = false;
static float g_recoveryAnchor[3] = {};
static uint64_t g_anchorProbeAt = 0;

static void release_player(uint8_t* ins, uint8_t* data, uint8_t* phys, bool restoreVisibility = true) {
    if (g_holding) log("stand-in: HOLD ended (released)");
    g_holding = false;
    InterlockedExchange(&g_bodyControlAccepted, 0);
    g_holdLease.reset();
    g_haveRecoveryAnchor = false;
    g_anchorProbeAt = 0;
    g_putHavokValid = false;
    g_movementCorrected = false;
    if (ins && mem_readable(ins + kChrDebugFlags, 4)) {
        *(uint32_t*)(ins + kChrDebugFlags) &= ~(1u << 5);          // NoMove off
    }
    if (restoreVisibility) {
        if (ins == g_standPlayer && ins && mem_readable(ins + kChrRenderFlags, 1) &&
            mem_readable(ins + 8, 8) && g_visibility.identity.same({(uintptr_t)ins,
                (uintptr_t)data, (uintptr_t)phys, *(uint64_t*)(ins + 8)}))
            g_visibility.release(ins[kChrRenderFlags]);
        else g_visibility.reset();
    }
    if (phys && mem_readable(phys + kPhysGravityOff, 1)) phys[kPhysGravityOff] = 0;
    if (data && mem_readable(data + kDataFlags, 1)) data[kDataFlags] &= ~1u;  // NoDead off
    g_standing = false;
    g_havePut = false;
    g_clampFrames = 0;
}

typedef void(__fastcall* Kill_t)(void* chr, uint8_t arg);

// The game's own death: the same call its debug "kill main player" makes. Death animation,
// "YOU DIED", runes left behind and the respawn at the last Site of Grace all follow.
static void kill_player(const Player& p) {
    release_player(p.ins, p.data, p.phys);
    ((Kill_t)(g_base + rva::kKill))(p.ins, 0);
}

// ---------------------------------------------------------------------------------------
// Reach: doors, levers, chests
//
// Elden Ring offers "Open", "Pull lever"... only about a metre from the object, facing it.
// Minecraft's block collision keeps Steve up to a block or two from Elden Ring's walls (worst
// for doors at an angle to the block grid), so the hidden Tarnished, standing exactly where
// Steve stands, never got close enough to the chapel's side door. When Steve
// faces an animated map object (a door, lever, chest, lift: a dynamic asset with an animation
// skeleton) within kReachMax, the Tarnished stands kReachGap short of it instead, as close as
// Elden Ring's own collision lets a player. Only for those: messages and corpses lie at
// Steve's feet, and their prompts must stay where they are.
//
// The objects' positions come from CSWorldGeomMan (block tree -> CSGrowableNodePool of
// CSWorldGeomIns*; +0x48 MSB part, +0x20 position in the block frame). Walking ~800 pointers
// through VirtualQuery takes milliseconds, so the worker thread does it once a second and
// the game thread only casts one ray per tick.

static bool raycast(const float* s, const float* e, uint32_t filter, void* ignore, ErmcRayHit* out);
static void* ray_world();

static bool fresh_support(const Player& p, const float* feet, bool footprint) {
    static constexpr float columns[][2] = {{0, 0}, {0.25f, 0}, {-0.25f, 0}, {0, 0.25f}, {0, -0.25f}};
    for (size_t i = 0; i < (footprint ? 5u : 1u); ++i) {
        float column[3] = {feet[0] + columns[i][0], feet[1], feet[2] + columns[i][1]};
        float start[3], end[3];
        ErmcRayHit hit = {};
        if (!prepare_placement_support(column, start, end) ||
            !raycast(start, end, kTerrainRayFilter, p.ins, &hit) || !hit.hit ||
            !placement_support_hit(column, hit.pos)) return false;
    }
    return true;
}

static bool recover_auto_handoff(const Player& p, const char* why) {
    // The game may have warped/replaced the body meanwhile. Never return such a
    // body to a previous pose, even if an old map still has loaded collision.
    bool owned = g_standing && g_havePut && g_putHavokValid && g_frame.valid &&
        p.ins == g_standPlayer && p.data == g_standData && p.phys == g_standPhys &&
        *(uint64_t*)(p.ins + 0x08) == g_standHandle && g_frame.zone == g_standZone &&
        shm_header()->hostLife == g_standLife;
    if (!owned) {
        log("recovery: %s; no current same-body/map/life owner, release without teleport", why);
        return false;
    }
    float current[3];
    to_stable((const float*)(p.phys + kPhysPos), current);
    if (!movement_acquisition_near(current, g_lastPut)) {
        log("recovery: %s; body moved since last accepted pose, release without teleport", why);
        return false;
    }
    if (fresh_support(p, current, true)) {
        log("recovery: %s; current position has fresh collision support", why);
        return true;
    }
    if (!g_haveRecoveryAnchor || !movement_recovery_near(current, g_recoveryAnchor) ||
        !fresh_support(p, g_recoveryAnchor, true)) {
        log("recovery: %s; no nearby freshly supported anchor, native release in place (fall risk)", why);
        return false;
    }
    float h[4]; to_havok(g_recoveryAnchor, h); h[3] = 1;
    if (!movement_position_finite(h)) return false;
    memcpy(p.phys + kPhysPos, h, 16);
    memcpy(p.phys + kPhysPosLast, h, 16);
    p.phys[kPhysProxyUpdate] = 1;
    // Lifecycle runs after the committed handoff now. This explicit relocation
    // must not look like an engine warp relative to the former airborne pose.
    memcpy(g_lastPut, g_recoveryAnchor, sizeof(g_lastPut));
    // This is our explicit relocation, not a Havok origin shift. Block position
    // may lag it for a frame; do not interpret that delta as a grid recentre.
    g_haveHmR = false;
    g_putHavokValid = false;
    log("recovery: %s; returned to fresh same-life/map anchor %.2f %.2f %.2f",
        why, g_recoveryAnchor[0], g_recoveryAnchor[1], g_recoveryAnchor[2]);
    return true;
}

constexpr float kReachMax = 2.0f;          // look this far ahead (m, horizontal, from the chest)
constexpr float kReachGap = 0.45f;         // stop this far short of the surface (capsule + margin)
constexpr float kReachChest = 1.0f;        // ray height above the feet
constexpr float kReachObjRadius = 2.0f;    // the surface hit must be this close to an object's origin
constexpr float kReachScanRadius = 40.0f;  // objects listed around the player
constexpr int kReachMaxObjs = 96;

struct ReachList {
    uint32_t block;  // the player's block when scanned
    int n;
    float pos[kReachMaxObjs][3];  // stable frame
    float yaw[kReachMaxObjs];     // MSB rotation about +Y, radians: facing (sin, 0, cos)
    uint32_t id[kReachMaxObjs];   // from the object's address: stable while it stays loaded
    char name[kReachMaxObjs][24];
};
static ReachList g_reachLists[2];
static volatile LONG g_reachReady = -1;              // published list (worker -> game thread)
static volatile uint32_t g_reachBlock = 0xFFFFFFFFu;  // game thread -> worker: the player's block
static volatile float g_reachAt[3];                  //   and where Steve stands (stable)
static float g_reachOffset[3];                       // put - Steve this tick (not published)
static int g_reachTarget = -1;                       // object index stepped up to, for the log

// Worker-thread reads of game memory the game may free meanwhile (open-world streaming):
// Native reads keep the actual copy guarded against a streaming unmap. Wine retains
// its existing ReadProcessMemory path.
static bool safe_read(const void* p, void* out, size_t n) {
#if defined(_MSC_VER) && ERMC_NATIVE_WINDOWS
    return mem_read(p, out, n);
#else
    SIZE_T got = 0;
    return (uintptr_t)p >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
#endif
}

static uint64_t rd_u64(const void* p) {
    uint64_t v = 0;
    return safe_read(p, &v, 8) ? v : 0;
}

static bool has_vtable(const void* p, uintptr_t vtRva) {
    return p && rd_u64(p) == g_base + vtRva;
}

// Worker thread. Collects animated dynamic assets near the player in its stable frame. The
// game may reshuffle these objects meanwhile (area loads): every pointer is re-checked, and a
// bad list only costs a missing or needless step-up for a second.
static void reach_scan() {
    uint32_t blk = g_reachBlock;
    if (!g_ok || blk == 0xFFFFFFFFu) return;
    float at[3] = {g_reachAt[0], g_reachAt[1], g_reachAt[2]};
    float bias[3] = {g_bias[0], g_bias[1], g_bias[2]};
    uint8_t* man = (uint8_t*)rd_u64((void*)(g_base + rva::kWorldGeomMan));
    if (!has_vtable(man, rva::kVtWorldGeomMan)) return;
    uint64_t t0 = now_ms();
    int w = g_reachReady == 0 ? 1 : 0;
    ReachList& L = g_reachLists[w];
    L.block = blk;
    L.n = 0;
    bool openWorld = (blk >> 24) == 60 || (blk >> 24) == 61;
    uintptr_t head = (uintptr_t)rd_u64(man + 0x20);
    uintptr_t stack[64];
    int sp = 0, visited = 0;
    if (head) stack[sp++] = (uintptr_t)rd_u64((void*)(head + 0x08));  // root
    // At most ~50 ms: the core's shutdown waits 2 s for this thread.
    while (sp > 0 && visited < 256 && now_ms() - t0 < 50) {
        uintptr_t node = stack[--sp];
        uint8_t nb[0x30];
        if (!node || node == head || !safe_read((void*)node, nb, sizeof(nb)) || nb[0x19]) continue;
        visited++;
        if (sp < 62) {
            stack[sp++] = *(uintptr_t*)(nb + 0x00);
            stack[sp++] = *(uintptr_t*)(nb + 0x10);
        }
        uint32_t id = *(uint32_t*)(nb + 0x20);
        uint8_t* data = *(uint8_t**)(nb + 0x28);
        // Legacy dungeons: only the player's own block shares its frame. Open world: detailed
        // (LOD 0) tiles of the same area, 256 m apart.
        float ox = 0, oz = 0;
        if (openWorld) {
            if ((id >> 24) != (blk >> 24) || (id & 0xFF) != 0) continue;
            ox = 256.0f * (float)((id >> 16) & 0xFF);
            oz = 256.0f * (float)((id >> 8) & 0xFF);
        } else if (id != blk) {
            continue;
        }
        // The pool's one contiguous array starts at +0xE8; the high half of +0xF8 is its
        // capacity (1024, grown to 3072 in the Stranded Graveyard), plus two slots. +0xF0 is
        // not its end once it has grown.
        uint8_t hdr[0x18];
        if (!data || !safe_read(data + 0xE8, hdr, sizeof(hdr))) continue;
        uint64_t first = *(uint64_t*)hdr;
        size_t slots = (size_t)*(uint32_t*)(hdr + 0x14) + 2;
        if (!first || slots > 16384) continue;
        static uint64_t ptrs[16384];
        if (!safe_read((void*)first, ptrs, slots * 8)) continue;
        for (size_t i = 0; i < slots && L.n < kReachMaxObjs; i++) {
            // A streamed tile can contain 16K slots. The outer tree deadline
            // alone does not bound the pointer/module walk within that tile.
            if (now_ms() - t0 >= 50) break;
            uint8_t* g = (uint8_t*)ptrs[i];
            if ((uintptr_t)g < 0x10000 || ((uintptr_t)g & 7) || !has_vtable(g, rva::kVtGeomDynamicIns)) continue;
            uint32_t gid = (uint32_t)((uintptr_t)g >> 3) ^ (uint32_t)((uintptr_t)g >> 35);
            bool dup = false;
            for (int k = 0; k < L.n && !dup; k++) dup = L.id[k] == gid;  // the pool can list one twice
            if (dup) continue;
            if (!has_vtable((void*)rd_u64(g + 0x4D8), rva::kVtAnimSkeletonModifier)) continue;
            uint8_t* part = (uint8_t*)rd_u64(g + 0x48);
            float pos[3];
            if (!part || !safe_read(part + 0x20, pos, 12)) continue;
            pos[0] += ox + bias[0], pos[1] += bias[1], pos[2] += oz + bias[2];  // published frame
            float dx = pos[0] - at[0], dy = pos[1] - at[1], dz = pos[2] - at[2];
            if (dx * dx + dy * dy + dz * dz > kReachScanRadius * kReachScanRadius) continue;
            memcpy(L.pos[L.n], pos, 12);
            float yaw = 0;
            safe_read(part + 0x30, &yaw, 4);
            L.yaw[L.n] = yaw;
            L.id[L.n] = gid;
            char* nm = L.name[L.n];
            nm[0] = 0;
            uintptr_t wname = (uintptr_t)rd_u64(part);
            wchar_t wn[23];
            if (wname && safe_read((void*)wname, wn, sizeof(wn))) {
                int k = 0;
                for (; k < 23 && wn[k] && wn[k] < 128; k++) nm[k] = (char)wn[k];
                nm[k] = 0;
            }
            L.n++;
        }
    }
    InterlockedExchange(&g_reachReady, w);
}

void game_worker_poll() {
    static uint64_t last = 0;
    uint64_t now = now_ms();
    if (now - last < 1000) return;
    last = now;
    if (!mc_consumer_active()) return;
    MemReadabilityScope memory;
    reach_scan();
}

constexpr float kReachLateral = 1.2f;  // Steve at most this far aside from the object's centre line
constexpr float kReachKnee = 0.4f;     // clearance ray height (steps, a dais, a railing)

// Game thread, standing in. When Steve faces an animated object within reach, `put` (Steve's
// position) moves in front of it: out from the object's origin along its facing, by the surface
// Steve sees plus kReachGap, at Steve's offset to the side. *faceTheta then faces the object
// squarely, and true is returned: a slanted look used to leave the Tarnished beside a door's
// small prompt zone, or at a slant to it. Only with a clear path at knee height and floor under
// the spot, so the Tarnished never stands inside a step or over a drop.
static bool reach_assist(float* put, float yawDeg, uint32_t block, void* ignore, float* faceTheta) {
    g_reachOffset[0] = g_reachOffset[1] = g_reachOffset[2] = 0;
    g_reachBlock = block;
    g_reachAt[0] = put[0], g_reachAt[1] = put[1], g_reachAt[2] = put[2];
    LONG r = g_reachReady;
    const ReachList* L = r >= 0 ? &g_reachLists[r] : nullptr;
    int target = -1;
    bool squared = false;
    if (L && L->block == block && L->n > 0) {
        // Anything animated within reach at all? (Most ticks: no, and no ray is cast.)
        bool nearby = false;
        for (int i = 0; i < L->n && !nearby; i++) {
            float dx = L->pos[i][0] - put[0], dz = L->pos[i][2] - put[2];
            nearby = dx * dx + dz * dz < (kReachMax + kReachObjRadius) * (kReachMax + kReachObjRadius) &&
                     fabsf(L->pos[i][1] - put[1]) < 3.0f;
        }
        float y = yawDeg * 3.14159265f / 180.0f;
        float fx = -sinf(y), fz = -cosf(y);  // Elden Ring forward (see stand_in)
        ErmcRayHit hit;
        float s[3] = {put[0], put[1] + kReachChest, put[2]};
        float e[3] = {s[0] + fx * kReachMax, s[1], s[2] + fz * kReachMax};
        if (nearby && raycast(s, e, kTerrainRayFilter, ignore, &hit) && hit.hit) {
            float best = kReachObjRadius * kReachObjRadius;
            for (int i = 0; i < L->n; i++) {
                float dx = L->pos[i][0] - hit.pos[0], dz = L->pos[i][2] - hit.pos[2];
                float dy = hit.pos[1] - L->pos[i][1];
                if (dx * dx + dz * dz < best && dy > -0.5f && dy < 3.5f) {
                    best = dx * dx + dz * dz;
                    target = i;
                }
            }
        }
        if (target >= 0) {
            const float* o = L->pos[target];
            float nx = sinf(L->yaw[target]), nz = cosf(L->yaw[target]);
            float wx = nz, wz = -nx;
            float rx = put[0] - o[0], rz = put[2] - o[2];
            float along = rx * nx + rz * nz;
            float side = along >= 0 ? 1.0f : -1.0f;
            float lateral = rx * wx + rz * wz;
            // The surface Steve sees, measured out from the object's plane toward him.
            float surf = side * ((hit.pos[0] - o[0]) * nx + (hit.pos[2] - o[2]) * nz);
            float stand = (surf > 0 ? surf : 0) + kReachGap;
            if (fabsf(lateral) <= kReachLateral && side * along > stand + 0.05f) {
                float dst[3] = {o[0] + nx * side * stand + wx * lateral, put[1], o[2] + nz * side * stand + wz * lateral};
                ErmcRayHit k, g;
                float ks[3] = {put[0], put[1] + kReachKnee, put[2]};
                float ke[3] = {dst[0], put[1] + kReachKnee, dst[2]};
                raycast(ks, ke, kTerrainRayFilter, ignore, &k);
                float gs[3] = {dst[0], put[1] + 0.5f, dst[2]};
                float ge[3] = {dst[0], put[1] - 1.0f, dst[2]};
                raycast(gs, ge, kTerrainRayFilter, ignore, &g);
                if (!k.hit && g.hit) {
                    for (int i = 0; i < 3; i++) g_reachOffset[i] = dst[i] - put[i];
                    memcpy(put, dst, 12);
                    *faceTheta = atan2f(-side * nx, -side * nz);
                    squared = true;
                }
            }
        }
    }
    if (!squared) target = -1;
    if (target != g_reachTarget) {
        if (target >= 0)
            log("reach: facing %s; the Tarnished stands square to it, %.2f m from Steve", L->name[target],
                sqrtf(g_reachOffset[0] * g_reachOffset[0] + g_reachOffset[2] * g_reachOffset[2]));
        else
            log("reach: back at Steve's feet");
        g_reachTarget = target;
    }
    return squared;
}

// Open doorways for Minecraft (ErmcPassageTable). Through each animated object near Steve: the
// centre line at chest height must be clear in Elden Ring's collision (an open door, not a
// closed one), and lines kPassageSide to either side must hit (a door in a wall, not a chest in
// the open). Checked every 30 ticks while standing in.
constexpr float kPassageHalfWidth = 0.75f;  // over half the 1.37 m a 60-degree strip needs for a 4-connected block path
constexpr float kPassageHalfDepth = 1.8f;
constexpr float kPassageSide = 1.4f;
constexpr float kPassageRange = 16.0f;

static void update_passages(bool standing, void* ignore) {
    static uint32_t tick = 0;
    static uint32_t shown[ERMC_MAX_PASSAGES];
    static uint32_t shownCount = 0;
    ErmcPassageTable* t = shm_passages();
    ErmcPassage out[ERMC_MAX_PASSAGES];
    uint32_t n = 0;
    char names[160] = "";
    if (standing) {
        if (++tick % 30 != 0) return;
        LONG r = g_reachReady;
        const ReachList* L = r >= 0 ? &g_reachLists[r] : nullptr;
        for (int i = 0; L && L->block == g_reachBlock && i < L->n && n < ERMC_MAX_PASSAGES; i++) {
            const float* o = L->pos[i];
            float dx = o[0] - g_reachAt[0], dz = o[2] - g_reachAt[2];
            if (dx * dx + dz * dz > kPassageRange * kPassageRange || fabsf(o[1] - g_reachAt[1]) > 4.0f) continue;
            float nx = sinf(L->yaw[i]), nz = cosf(L->yaw[i]);
            float wx = nz, wz = -nx;
            float y = o[1] + 1.0f;
            ErmcRayHit h;
            float s[3] = {o[0] - nx * kPassageHalfDepth, y, o[2] - nz * kPassageHalfDepth};
            float e[3] = {o[0] + nx * kPassageHalfDepth, y, o[2] + nz * kPassageHalfDepth};
            // raycast() returns whether it hit (false also when the physics world is missing: then
            // the side rays "miss" too, and nothing is published).
            raycast(s, e, kTerrainRayFilter, ignore, &h);
            if (h.hit) continue;
            bool walls = true;
            for (int side = -1; side <= 1 && walls; side += 2) {
                float s2[3] = {s[0] + side * wx * kPassageSide, y, s[2] + side * wz * kPassageSide};
                float e2[3] = {e[0] + side * wx * kPassageSide, y, e[2] + side * wz * kPassageSide};
                raycast(s2, e2, kTerrainRayFilter, ignore, &h);
                walls = h.hit != 0;
            }
            if (!walls) continue;
            ErmcPassage& P = out[n++];
            memcpy(P.pos, o, 12);
            P.yaw = L->yaw[i];
            P.halfWidth = kPassageHalfWidth;
            P.halfDepth = kPassageHalfDepth;
            P.height = 3.0f;
            P.id = L->id[i];
            if (strlen(names) + strlen(L->name[i]) + 2 < sizeof(names)) {
                if (names[0]) strcat(names, " ");
                strcat(names, L->name[i]);
            }
        }
    } else if (t->count == 0 && shownCount == 0) {
        return;
    }
    bool same = n == shownCount;
    for (uint32_t i = 0; same && i < n; i++) same = out[i].id == shown[i];
    t->seq = t->seq + 1;  // odd: being written
    compiler_barrier();
    memcpy(t->p, out, n * sizeof(ErmcPassage));
    t->count = n;
    t->zone = g_frame.zone;
    compiler_barrier();
    t->seq = t->seq + 1;
    if (!same) {
        if (n) log("passages: %u open doorway(s) for Minecraft: %s", n, names);
        else log("passages: none");
        for (uint32_t i = 0; i < n; i++) shown[i] = out[i].id;
        shownCount = n;
    }
}

static void stand_in(const Player& p, const ErmcControl* c, bool active, bool visibilityGrace = false,
                      bool holdRequested = false) {
    if (g_standPlayer && g_standPlayer != p.ins) {
        if (g_holding) log("stand-in: HOLD ended (player replaced)");
        g_holding = false;
        g_holdLease.reset();
        g_haveRecoveryAnchor = false;
        g_anchorProbeAt = 0;
        g_putHavokValid = false;
        g_standing = false;  // new character instance (loading screen): nothing to restore
        InterlockedExchange(&g_bodyControlAccepted, 0);
        g_visibility.reset();
        g_havePut = false;
        g_movementCorrected = false;
    }
    g_standPlayer = p.ins;
    g_visibility.observe({(uintptr_t)p.ins, (uintptr_t)p.data, (uintptr_t)p.phys,
        *(uint64_t*)(p.ins + 8)});
    // A HOLD never acquires a body or accepts coordinates. MOVE wins ambiguous
    // flags; a malformed MOVE must release rather than falling back to HOLD.
    bool hold = !active && holdRequested && g_coopExcludePlayerIns && g_standing &&
        g_havePut && g_putHavokValid && g_frame.valid && g_standData == p.data &&
        g_standPhys == p.phys && g_standHandle == *(uint64_t*)(p.ins + 0x08) &&
        g_standZone == g_frame.zone && g_standLife == shm_header()->hostLife &&
        movement_position_finite(g_lastPut);
    const bool sameOwner = g_standing && g_standData == p.data && g_standPhys == p.phys &&
        g_standHandle == *(uint64_t*)(p.ins + 0x08) && g_standZone == g_frame.zone &&
        g_standLife == shm_header()->hostLife && g_putHavokValid;
    float currentFeet[3];
    to_stable((const float*)(p.phys + kPhysPos), currentFeet);
    float requestedHavok[3] = {};
    if (active) to_havok(c->hunterPos, requestedHavok);
    if (active && (!movement_position_finite(c->hunterPos) ||
        !movement_position_finite(requestedHavok) || !std::isfinite(c->hunterYawDeg) ||
        (!sameOwner && !movement_acquisition_near(currentFeet, c->hunterPos)))) {
        // Invalid client coordinates must never reach the game's physics body.
        active = false;
        visibilityGrace = false;
        g_putHavokValid = false;  // never undo an engine warp with the old reach pose
        static uint64_t lastRejectedLog = 0;
        uint64_t now = now_ms();
        if (!lastRejectedLog || now < lastRejectedLog || now - lastRejectedLog >= 1000) {
            lastRejectedLog = now;
            log("movement: rejected invalid or unsynchronized MOVE; native pose %.2f %.2f %.2f",
                currentFeet[0], currentFeet[1], currentFeet[2]);
        }
    }
    if (active && !sameOwner) {
        g_movementCorrected = false;
        g_haveRecoveryAnchor = false;
        g_anchorProbeAt = 0;
    }
    if (active && !g_movementCorrected) {
        float previous[3], start[3], end[3];
        for (int i = 0; i < 3; ++i) previous[i] = sameOwner
            ? g_lastPut[i] - g_reachOffset[i] : currentFeet[i];
        if (prepare_ground_sweep(previous, c->hunterPos, start, end) && !ray_world()) {
            // Missing streaming physics is not a genuine ray miss. Hand back in
            // place instead of accepting an unchecked step through the floor.
            active = false;
            visibilityGrace = false;
            g_putHavokValid = false;
            log("movement: descent released without a loaded collision world");
        }
    }
    if (g_holdLease.expired(now_ms(), hold)) {
        log("stand-in: HOLD timed out after %llu ms; native control requires F8 retry",
            (unsigned long long)StandInHoldLease::limitMs);
        recover_auto_handoff(p, "bounded HOLD expired");
        InterlockedExchange(&g_controlRevoked, 1);
        g_lastStandMs = 0;  // deferred Minecraft deaths no longer own this life
        hold = false;
        visibilityGrace = false;
        g_putHavokValid = false;  // release in place; no saved-pose teleport
        return_to_host("bounded HOLD expired");
    }

    if (hold != g_holding) {
        log("stand-in: HOLD %s (last accepted position %.2f %.2f %.2f)",
            hold ? "started" : "ended", g_lastPut[0], g_lastPut[1], g_lastPut[2]);
        g_holding = hold;
    }
    bool hide = hold || (active && (c->flags & ERMC_CTRL_HIDE_HUNTER));
    g_visibility.apply(p.ins[kChrRenderFlags], now_ms(), hide, visibilityGrace && !active && !hold);

    if (!active && !hold) {
        InterlockedExchange(&g_bodyControlAccepted, 0);
        g_movementCorrected = false;
        // Only while our last write still stands (not across a loading screen or a game warp).
        bool trusted = g_putHavokValid;
        g_putHavokValid = false;
        if (trusted && g_standing && g_havePut && (g_reachOffset[0] != 0 || g_reachOffset[2] != 0)) {
            // Back to Steve's feet first: the reach step stands it against a door or a lever,
            // where the game's own physics would take over.
            float s0[3] = {g_lastPut[0] - g_reachOffset[0], g_lastPut[1] - g_reachOffset[1], g_lastPut[2] - g_reachOffset[2]};
            float h0[4];
            to_havok(s0, h0);
            h0[3] = 1.0f;
            memcpy(p.phys + kPhysPos, h0, 16);
            memcpy(p.phys + kPhysPosLast, h0, 16);
            p.phys[kPhysProxyUpdate] = 1;
        }
        g_reachBlock = 0xFFFFFFFFu;
        g_reachOffset[0] = g_reachOffset[1] = g_reachOffset[2] = 0;
        g_reachTarget = -1;
        if (g_standing) {
            release_player(p.ins, p.data, p.phys, false);  // visibility has its own bounded lease
            log("stand-in: released the player");
            // An aggroed test soldier would now fight the real, unprotected Tarnished.
            remove_test_enemy("the stand-in ended");
        }
        g_lastHp = *(int*)(p.data + kDataHp);
        return;
    }
    if (!g_standing) {
        g_standing = true;
        g_lastHp = -1;
        g_clampFrames = 0;
        log("stand-in: the player now follows Steve");
    }
    g_lastStandMs = now_ms();
    *(uint32_t*)(p.ins + kChrDebugFlags) |= (1u << 5);  // NoMove: ignore the game's own input
    p.phys[kPhysGravityOff] = 1;
    p.data[kDataFlags] |= 1u;                            // NoDead

    if (!hold) {
        g_standData = p.data; g_standPhys = p.phys;
        g_standHandle = *(uint64_t*)(p.ins + 0x08);  // FieldInsHandle, as in entity publication
        g_standZone = g_frame.zone; g_standLife = shm_header()->hostLife;
        g_lastDriveFreeFlight = (c->flags & ERMC_CTRL_FREE_FLIGHT) != 0;
    }
    float put[3];
    memcpy(put, hold ? g_lastPut : c->hunterPos, sizeof(put));
    if (!hold && g_movementCorrected) {
        float dx = put[0] - g_correctedFeet[0], dz = put[2] - g_correctedFeet[2];
        if (put[1] >= g_correctedFeet[1] - 0.03f && dx * dx + dz * dz < 0.25f) {
            g_movementCorrected = false;  // the client's teleport/landing reached this pose
        } else {
            memcpy(put, g_correctedFeet, sizeof(put));
        }
    }
    if (!hold && !g_movementCorrected) {
        // Probe the destination column, from just above the last accepted feet.
        // Looking down from head height could mistake an overhead floor for a
        // landing. A miss is a real drop; no remembered flat floor is invented.
        float previousFeet[3];
        for (int i = 0; i < 3; ++i) previousFeet[i] = sameOwner && g_havePut && g_putHavokValid
            ? g_lastPut[i] - g_reachOffset[i] : currentFeet[i];
        float start[3], end[3];
        if (prepare_ground_sweep(previousFeet, put, start, end)) {
            ErmcRayHit hit = {};
            if (raycast(start, end, kTerrainRayFilter, p.ins, &hit) && hit.hit) {
                // This engine entry point does not expose surface normals.
                // For this strictly vertical down-ray only, use its landing
                // candidate convention, not an inferred normal from a side ray.
                GroundSweepHit candidate = {};
                candidate.hit = true;
                memcpy(candidate.pos, hit.pos, sizeof(candidate.pos));
                candidate.normal[1] = 1.0f;
                auto landing = select_ground_crossing(start, end, previousFeet[1], &candidate, 1);
                if (landing.valid && landing.clamped) {
                    float requestedY = put[1];
                    put[1] = landing.feetY;
                    memcpy(g_correctedFeet, put, sizeof(put));
                    g_movementCorrected = true;
                    static uint64_t lastCorrectionLog = 0;
                    uint64_t now = now_ms();
                    if (now - lastCorrectionLog > 1000) {
                        lastCorrectionLog = now;
                        log("movement: stopped floor crossing at %.2f %.2f %.2f (Minecraft requested Y %.2f)",
                            put[0], put[1], put[2], requestedY);
                    }
                }
            }
        }
    }
    float faceTheta = 0;
    bool squared = false;
    if (!hold && (c->flags & ERMC_CTRL_FREE_FLIGHT)) {
        // Door/lever reach is a grounded interaction aid. Keeping its old offset
        // in free flight can drag the hidden host body sideways against a door.
        g_reachBlock = 0xFFFFFFFFu;
        g_reachOffset[0] = g_reachOffset[1] = g_reachOffset[2] = 0;
        g_reachTarget = -1;
    } else if (!hold) {
        squared = reach_assist(put, c->hunterYawDeg, *(uint32_t*)(p.ins + kPlayerBlockId), p.ins, &faceTheta);
    }
    float h[4];
    to_havok(put, h);
    h[3] = 1.0f;
    memcpy(p.phys + kPhysPos, h, 16);
    memcpy(p.phys + kPhysPosLast, h, 16);
    p.phys[kPhysProxyUpdate] = 1;
    memcpy(g_lastPut, put, 12);
    memcpy(g_putHavok, h, 12);
    g_putHavokValid = true;
    g_havePut = true;
    InterlockedExchange(&g_bodyControlAccepted, 1);
    if (!hold && (!g_anchorProbeAt || now_ms() < g_anchorProbeAt || now_ms() - g_anchorProbeAt >= 250)) {
        g_anchorProbeAt = now_ms();
        // Remember only an accepted native pose with a fresh support observation.
        // Airborne poses never overwrite the last supported one.
        if (fresh_support(p, put, false)) {
            memcpy(g_recoveryAnchor, put, 12);
            g_haveRecoveryAnchor = true;
        }
    }
    // Minecraft yaw (0 = +Z south, clockwise) -> Elden Ring yaw about +Y, with Minecraft's
    // Z flipped (Elden Ring is left-handed): forward_er = (-sin y, 0, -cos y).
    if (!hold) {
        float y = fmodf(c->hunterYawDeg, 360.0f) * (3.14159265f / 180.0f);
        float theta = squared ? faceTheta : atan2f(-sinf(y), -cosf(y));
        float q[4] = {0.0f, sinf(theta * 0.5f), 0.0f, cosf(theta * 0.5f)};
        memcpy(p.phys + kPhysQuat, q, 16);
        memcpy(p.phys + kPhysQuatInterp, q, 16);
    }

    int hp = *(int*)(p.data + kDataHp), mx = *(int*)(p.data + kDataMaxHp);
    // NoDead turns lethal damage into "1 HP left". Once, that is a big hit (Minecraft decides
    // whether it kills). Every tick in a row, it is the game killing the Tarnished (a kill
    // plane under the map): report nothing more, update_life lets it die after 3 ticks.
    bool clamped = hp == 1 && g_lastHp > 1;
    g_clampFrames = clamped ? g_clampFrames + 1 : 0;
    if (g_lastHp > 0 && hp < g_lastHp && g_clampFrames <= 1) {
        float dmg = (float)(g_lastHp - hp);
        // Blame the attacker the game recorded on the Tarnished (last_hit_by, reset after each
        // hit so falls and poison aren't blamed on it), else the nearest hostile character. Its
        // position gives Minecraft the knockback direction and the "slain by" name.
        uint64_t by = *(uint64_t*)(p.ins + 0x180);
        *(uint64_t*)(p.ins + 0x180) = ~0ull;
        float apos[3];
        uint32_t akind = 0;
        if ((uint32_t)by != 0xFFFFFFFFu && published_attacker(by, apos, &akind)) {
        } else if (g_nearestHostile.valid) {
            memcpy(apos, g_nearestHostile.pos, 12);
            akind = g_nearestHostile.kind;
        } else {
            memcpy(apos, hold ? g_lastPut : c->hunterPos, 12);
            akind = ERMC_ENT_LARGE_MONSTER;
        }
        // All reads/writes of streamed game objects finish before opening this
        // publication. A guarded fault must never strand its seqlock odd.
        ErmcHunterEvents* ev = shm_hunter();
        ev->seq = ev->seq + 1;
        compiler_barrier();
        ev->hitCount = ev->hitCount + 1;
        ev->totalDamage = ev->totalDamage + dmg;
        ev->lastDamage = dmg;
        memcpy(ev->lastHitFrom, apos, 12);
        ev->lastHitKind = akind;
        ev->hunterMaxHp = (float)mx;
        ev->lastHitFrame = shm_state()->frame;
        compiler_barrier();
        ev->seq = ev->seq + 1;
        log("stand-in: the player took %.0f damage -> Minecraft", dmg);
    }
    if (mx > 0) *(int*)(p.data + kDataHp) = mx;  // never faint while standing in
    g_lastHp = mx;
}

// ---------------------------------------------------------------------------------------
// Life: when the Tarnished can be used, and deaths shared with Minecraft.
//
//   NONE      no character, or a loading screen (block id -1)
//   SETTLING  back after a loading screen, a death or a game warp: wait until the game has
//             put it down for good (right after loading it sits at a temporary spot first)
//   ALIVE     published to Minecraft (PLAYER_VALID); may stand in for Steve
//   DEAD      dying or dead: hands off, so the game's death camera, "YOU DIED" and respawn
//             run untouched

enum Life { LIFE_NONE, LIFE_SETTLING, LIFE_ALIVE, LIFE_DEAD };
static const char* const kLifeNames[] = {"none", "settling", "alive", "dead"};
static Life g_life = LIFE_NONE;
static bool g_lifeTicked = false;     // false until this core's first tick
static bool g_quickSettle = false;    // hot reload into a running game: no loading screen to wait out
static uint32_t g_settleTicks = 0, g_stillTicks = 0, g_settleMin = 0, g_stillMin = 0;
static float g_settlePos[3], g_settleOff[3];
static uint32_t g_settleZone = 0, g_aliveZone = 0;
static uint64_t g_lastAliveMs = 0;
static bool g_expectDeath = false;    // we killed it because Minecraft's player died: don't echo back
static uint32_t g_mcDeathsSeen = 0;
static bool g_mcDeathsInit = false;

static void set_life(Life l, const char* why) {
    if (l == g_life) return;
    log("life: %s -> %s (%s)", kLifeNames[g_life], kLifeNames[l], why);
    g_life = l;
}

static void start_settling(const Player& p, uint32_t minTicks, uint32_t stillTicks, const char* why) {
    g_settleTicks = 0;
    g_stillTicks = 0;
    g_settleMin = minTicks;
    g_stillMin = stillTicks;
    g_settleZone = stable_pos(p, g_settlePos);
    memcpy(g_settleOff, g_frame.offset, 12);
    set_life(LIFE_SETTLING, why);
}

// Settled: at least g_settleMin ticks since settling began, and the position, zone and
// Havok offset unchanged for the last g_stillMin ticks.
static bool settled(const Player& p) {
    float s[3];
    uint32_t zone = stable_pos(p, s);
    float d = fabsf(s[0] - g_settlePos[0]) + fabsf(s[1] - g_settlePos[1]) + fabsf(s[2] - g_settlePos[2]);
    float o = fabsf(g_frame.offset[0] - g_settleOff[0]) + fabsf(g_frame.offset[1] - g_settleOff[1]) +
              fabsf(g_frame.offset[2] - g_settleOff[2]);
    if (zone == g_settleZone && d < 0.15f && o < 0.01f) {
        g_stillTicks++;
    } else {
        g_stillTicks = 0;
        g_settleZone = zone;
        memcpy(g_settlePos, s, 12);
        memcpy(g_settleOff, g_frame.offset, 12);
    }
    g_settleTicks++;
    if (g_settleTicks < g_settleMin || g_stillTicks < g_stillMin) return false;
    if (g_coopExcludePlayerIns) {
        // Coordinate stability does not prove a late Seamless spawn is playable.
        // Require a CURRENT nearby terrain collision; distant visual geometry,
        // a missing physics world, and cached Java terrain are not support.
        float start[3], end[3];
        ErmcRayHit hit = {};
        if (!prepare_placement_support(s, start, end) ||
            !raycast(start, end, kTerrainRayFilter, p.ins, &hit) || !hit.hit ||
            !placement_support_hit(s, hit.pos)) {
            static uint64_t lastSupportLog = 0;
            uint64_t now = now_ms();
            if (!lastSupportLog || now < lastSupportLog || now - lastSupportLog >= 5000) {
                lastSupportLog = now;
                log("life: waiting for fresh collision support in zone %08x at %.2f %.2f %.2f",
                    g_frame.zone, s[0], s[1], s[2]);
            }
            return false;
        }
    }
    return true;
}

static bool is_dead(const Player& p) {
    return *(int*)(p.data + kDataHp) <= 0 || (p.ins[kChrRenderFlags] & 0x80);
}

static void log_death_state(const Player& p, const char* reason) {
    uint64_t now = now_ms();
    bool recent = g_haveLastCtrl && now >= g_lastCtrlChangeMs && now - g_lastCtrlChangeMs <= 1000;
    log("life diagnostic: %s hp %d max %d dataFlags 0x%02x renderFlags 0x%02x standing %d hold %d clamp %d recentControl %d control 0x%x controlAgeMs %llu freeFlight %d",
        reason, *(int*)(p.data + kDataHp), *(int*)(p.data + kDataMaxHp),
        (unsigned)p.data[kDataFlags], (unsigned)p.ins[kChrRenderFlags],
        (int)g_standing, (int)g_holding, g_clampFrames, (int)recent,
        g_haveLastCtrl ? (unsigned)g_lastCtrl.flags : 0u,
        (unsigned long long)(g_haveLastCtrl && now >= g_lastCtrlChangeMs ? now - g_lastCtrlChangeMs : 0),
        (int)g_lastDriveFreeFlight);
}

// p: the main character, or null (none, or a loading screen). Runs before stand_in().
static void update_life(const Player* p) {
    uint64_t now = now_ms();
    bool first = !g_lifeTicked;
    g_lifeTicked = true;
    if (!p) {
        if (g_life != LIFE_NONE) set_life(LIFE_NONE, "no character or loading screen");
        return;
    }
    if (first && !is_dead(*p)) g_quickSettle = true;

    // Minecraft's player died while the Tarnished stood in for him: it dies too.
    uint32_t mcDeaths = shm_header()->mcDeaths;
    if (!g_mcDeathsInit) {
        g_mcDeathsSeen = mcDeaths;
        g_mcDeathsInit = true;
    }
    if (mcDeaths != g_mcDeathsSeen) {
        g_mcDeathsSeen = mcDeaths;
        ErmcControl currentMode = {};
        bool passive = control_snapshot(&currentMode) && (currentMode.flags & ERMC_CTRL_PASSIVE_COMPOSITE);
        const bool owned = g_standing && g_putHavokValid && g_frame.valid &&
            p->ins == g_standPlayer && p->data == g_standData && p->phys == g_standPhys &&
            *(uint64_t*)(p->ins + 8) == g_standHandle && g_frame.zone == g_standZone &&
            shm_header()->hostLife == g_standLife;
        if (!g_suppressMcDeaths && !passive && owned && mc_consumer_active() &&
            g_life == LIFE_ALIVE && g_lastStandMs && now >= g_lastStandMs &&
            now - g_lastStandMs < 2000 && !is_dead(*p)) {
            log_death_state(*p, "Minecraft death requested");
            log("life: Minecraft's player died -> the Tarnished dies too");
            g_expectDeath = true;
            kill_player(*p);
        }
    }

    bool dead = is_dead(*p);
    if (dead && g_life != LIFE_DEAD) log_death_state(*p, "native death observed");
    switch (g_life) {
    case LIFE_NONE:
    case LIFE_DEAD:
        if (!dead) {
            if (g_quickSettle) start_settling(*p, 10, 0, "character present");
            else start_settling(*p, 90, 45, g_life == LIFE_DEAD ? "respawned" : "loaded");
            g_quickSettle = false;
        }
        break;
    case LIFE_SETTLING:
        if (dead) {
            set_life(LIFE_DEAD, "died while settling");
        } else if (settled(*p)) {
            set_life(LIFE_ALIVE, "settled");
            g_aliveZone = g_frame.zone;
            // After a load, death or warp, Minecraft must put its player here. Not after a core
            // reload or a block change: the Tarnished is still where Minecraft put it.
            if (g_settleMin >= 30) shm_header()->hostLife = shm_header()->hostLife + 1;
        }
        break;
    case LIFE_ALIVE: {
        bool standing = g_standing;  // before kill_player() releases it
        if (!dead && g_standing && g_clampFrames >= 3) {
            log_death_state(*p, "repeated lethal clamp");
            // The game keeps killing it through NoDead (fell below the map): let it.
            log("life: the game is killing the Tarnished (below the map?) -> letting it die");
            kill_player(*p);
            dead = true;
        }
        if (dead) {
            if (standing && !g_expectDeath) {
                shm_header()->hostDeaths = shm_header()->hostDeaths + 1;
                log("life: the Tarnished died -> Minecraft's player dies too");
            }
            g_expectDeath = false;
            set_life(LIFE_DEAD, "died");
            break;
        }
        float s[3];
        uint32_t zone = stable_pos(*p, s);
        if (zone != g_aliveZone) {
            start_settling(*p, 10, 0, "zone changed");
        } else if (g_standing && g_havePut) {
            float d = fabsf(s[0] - g_lastPut[0]) + fabsf(s[1] - g_lastPut[1]) + fabsf(s[2] - g_lastPut[2]);
            if (d > 10.0f) start_settling(*p, 30, 15, "the game moved the Tarnished");
        }
        break;
    }
    }
    // Alive or dying: a death sequence (the Grafted Scion's ran 2.5 minutes) and the loading
    // screen after it are one busy stretch for Minecraft, however long the first part takes.
    if (g_life == LIFE_ALIVE || g_life == LIFE_DEAD) g_lastAliveMs = now;
}

// ---------------------------------------------------------------------------------------
// Characters near the player -> Minecraft (entity table), and Minecraft's hits -> them.
// Hostility is the game's own CanTarget(player, chr);
// damage goes through ModifyHp (respects "can't die"), credits the player (last_hit_by), and
// a player-owned Ruin Fragment "poke" bullet gives the hit reaction and aggro; Kill ends it
// (death animation, drops, runes to the player).

namespace combat {
constexpr uintptr_t kModifyHp = 0x436A40;      // void (CSChrDataModule*, int delta, u8, u8, f32, f32, u8)
constexpr uintptr_t kSpawnBullet = 0x3A2CB0;   // u32* (CSBulletManager*, u32* outHandle, const BulletSpawnData*, u32* outErr)
constexpr uintptr_t kCanTarget = 0x51B610;     // u32 (ChrIns* self, ChrIns* other)
constexpr uintptr_t kForceDeath = 0x5EE570;    // void (ChrIns*, bool awardRunes): Kill without drops when false
constexpr uintptr_t kCSBulletManager = 0x3D667A8;
constexpr uintptr_t kSoloParamRepository = 0x3D85F58;
constexpr uintptr_t kVtDebugChrCreator = 0x2A51220;
}  // namespace combat

static const CodeSig kCombatSigs[] = {
    {combat::kModifyHp, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 41 0F B6 F1 41 0F B6 E8 8B DA 48 8B F9 85 D2 79 1D", "CSChrDataModule::ModifyHp"},
    {combat::kSpawnBullet, "40 53 55 56 57 48 81 EC 98 07 00 00", "CSBulletManager::SpawnBullet"},
    {combat::kCanTarget, "40 56 48 83 EC 30 48 8B F2 48 85 C9 74 71", "CanTarget"},
    {combat::kForceDeath, "48 85 C9 74 12 84 D2 75 07 80 89 C6 01 00 00 03", "ForceDeath"},
};
static bool g_combatOk = false;

constexpr float kPublishRadius = 80.0f;        // metres
constexpr size_t kChrHandle = 0x08;            // FieldInsHandle (u64): the stable id
constexpr size_t kChrLastHitBy = 0x180;        // FieldInsHandle of the last attacker, -1 = none
constexpr size_t kChrRewards = 0x1C6;          // bit0 item lot dropped, bit1 runes given
constexpr size_t kChrDistSq = 0x3FC;           // squared distance to the main player

typedef void(__fastcall* ModifyHp_t)(void* data, int delta, uint8_t a3, uint8_t a4, float a5, float a6, uint8_t a7);
typedef uint32_t(__fastcall* CanTarget_t)(void* self, void* other);
typedef void(__fastcall* ForceDeath_t)(void* chr, bool awardRunes);

struct Chr {
    uint8_t* ins;
    uint8_t* data;
    uint8_t* phys;
};

static bool get_chr(uint8_t* ins, Chr* out, bool withinPublishRadius = false) {
    if (!typed(ins, rva::kVtEnemyIns) && !typed(ins, rva::kVtPlayerIns)) return false;
    if (!mem_readable(ins, 0x580)) return false;
    if (withinPublishRadius) {
        // Streaming can leave many distant candidates in the active list.
        // Reject them before validating their separate data/physics modules.
        // Keep the full instance check before reading its distance field.
        float d2 = *(float*)(ins + kChrDistSq);
        if (!(d2 >= 0.0f) || d2 > kPublishRadius * kPublishRadius) return false;
    }
    uint8_t* mods = *(uint8_t**)(ins + kChrModules);
    if (!mods || !mem_readable(mods, 0x70)) return false;
    out->ins = ins;
    out->data = typed(*(void**)(mods + kModData), rva::kVtDataModule);
    out->phys = typed(*(void**)(mods + kModPhysics), rva::kVtPhysModule);
    return out->data && out->phys;
}

// Conservative opt-in guard, not a network-peer classifier: PlayerIns NPCs
// are excluded too. Keep get_player/stand-in and the solo scan unchanged.
static bool coop_excludes_proxy(const Chr& c) {
    return g_coopExcludePlayerIns && typed(c.ins, rva::kVtPlayerIns);
}

// Enumerate the dedicated player set, then validate reciprocal membership,
// exact runtime types and both Steam identity paths before any render-bit write.
static size_t g_peerImageSize = 0;
static uint64_t g_peerLocalSteam = 0, g_peerSteamQueryAt = 0;
static PeerModeLease g_peerModeLease;
static PeerRenderOwners g_peerRenderOwners;
static PeerCompositeLease g_peerCompositeLease;

static ErmcPeerInfo* peer_info() {
    return (ErmcPeerInfo*)((uint8_t*)shm_header() + ERMC_OFF_PEER_INFO);
}
static ErmcPeerMode* peer_mode() {
    return (ErmcPeerMode*)((uint8_t*)shm_header() + ERMC_OFF_PEER_MODE);
}
static bool peer_mode_snapshot(ErmcPeerMode* out) {
    const ErmcPeerMode* p = peer_mode();
    for (int tries = 0; tries < 4; ++tries) {
        uint32_t seq = p->seq;
        compiler_barrier();
        if (!seq || (seq & 1)) continue;
        memcpy(out, p, sizeof(*out));
        compiler_barrier();
        if (seq == p->seq) return true;
    }
    return false;
}
static void publish_peer_info(uint64_t remote, bool hidden) {
    ErmcPeerInfo* p = peer_info();
    // Recover an abandoned odd publication after a previous core stopped.
    uint32_t seq = (p->seq + 1u) | 1u;
    p->seq = seq;
    compiler_barrier();
    p->version = ERMC_PEER_VERSION;
    p->hostStartMs = shm_header()->hostStartMs;
    p->localSteamId = g_peerLocalSteam;
    p->remoteSteamId = remote;
    p->flags = (g_peerLocalSteam ? ERMC_PEER_LOCAL_VALID : 0u) |
        (remote ? ERMC_PEER_REMOTE_VALID : 0u) | (hidden ? ERMC_PEER_REMOTE_HIDDEN : 0u);
    p->reserved = 0;
    compiler_barrier();
    p->seq = seq + 1u;
}
static void query_local_steam_identity(void* output) {
    auto* id = (uint64_t*)output;
    *id = 0;
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) return;
    using UserHandle = int(__cdecl*)();
    using UserInterface = void*(__cdecl*)();
    using SteamId = uint64_t(__cdecl*)(void*);
    auto handle = (UserHandle)GetProcAddress(steam, "SteamAPI_GetHSteamUser");
    auto interface021 = (UserInterface)GetProcAddress(steam, "SteamAPI_SteamUser_v021");
    auto getId = (SteamId)GetProcAddress(steam, "SteamAPI_ISteamUser_GetSteamID");
    if (!handle || !interface021 || !getId || handle() <= 0) return;
    // Query only an already initialized interface. No loading, init, shutdown,
    // authentication or Steam/game state mutations.
    void* user = interface021();
    if (user) {
        uint64_t candidate = getId(user);
        if (valid_peer_steam(candidate)) *id = candidate;
    }
}
static bool native_peer_identity(uintptr_t ins, uintptr_t local, NativePeerIdentity* out) {
    auto read = [](uintptr_t p, void* out, size_t n) { return mem_read((void*)p, out, n); };
    return read_native_peer(ins, local, g_base, g_peerImageSize, rva::kVtPlayerIns, read, out);
}
static bool native_peer_at_slot(uintptr_t slot, uintptr_t local, NativePeerIdentity* out) {
    uintptr_t ins = 0, reciprocal = 0;
    if (!peer_pointer(slot) || !mem_read((void*)slot, &ins, 8) || !peer_pointer(ins) ||
        !mem_read((void*)(ins + 0x10), &reciprocal, 8) || reciprocal != slot ||
        !native_peer_identity(ins, local, out)) return false;
    out->slot = slot;
    return true;
}

static void update_peer_visibility(const Player* local, bool consumer, bool viewerCanComposite, bool shutdown = false) {
    if (!g_coopExcludePlayerIns) return;
    uint64_t now = now_ms();
    if (!shutdown && now >= g_peerSteamQueryAt) {
        uint64_t id = 0;
        if (!guarded_game_call(query_local_steam_identity, &id)) id = 0;
        g_peerLocalSteam = id;
        g_peerSteamQueryAt = now + 1000;
    }
    ErmcPeerMode mode = {};
    bool snapshot = !shutdown && peer_mode_snapshot(&mode);
    if (shutdown || (!snapshot && !peer_mode()->seq)) g_peerModeLease.reset();
    bool fresh = g_peerModeLease.observe(snapshot ? &mode : nullptr, shm_header()->mcPid,
        shm_header()->mcStartMs, consumer && !shutdown, now, &mode);

    NativePeerIdentity peers[8] = {};
    size_t count = 0;
    bool complete = false;
    if (local && g_peerImageSize) {
        uint8_t* wcm = global_ptr(rva::kWorldChrMan, rva::kVtWorldChrMan);
        uint8_t* set = wcm ? typed(wcm + kPlayerChrSet, rva::kVtChrSet) : nullptr;
        uintptr_t entries = 0;
        uint32_t capacity = 0;
        if (set && mem_read(set + 0x10, &capacity, 4) && capacity <= 128 &&
            mem_read(set + 0x18, &entries, 8) && (!capacity || peer_pointer(entries))) {
            complete = true;
            for (uint32_t i = 0; i < capacity; ++i) {
                uintptr_t member = 0;
                if (!mem_read((void*)(entries + i * 0x10), &member, 8)) { complete = false; continue; }
                if (!member || member == (uintptr_t)local->ins) continue;
                NativePeerIdentity p;
                if (!native_peer_at_slot(entries + i * 0x10, (uintptr_t)local->ins, &p)) continue;
                bool duplicate = false;
                for (size_t j = 0; j < count; ++j) if (peers[j].ins == p.ins) duplicate = true;
                if (duplicate) continue;
                if (count == 8) { complete = false; break; }
                peers[count++] = p;
            }
        }
    }
    uint64_t remote = complete && count == 1 && g_peerLocalSteam && peers[0].steam != g_peerLocalSteam
        ? peers[0].steam : 0;
    if (complete && fresh && g_peerLocalSteam && mode.remoteSteamId != g_peerLocalSteam) {
        size_t matches = 0;
        for (size_t i = 0; i < count; ++i) if (peers[i].steam == mode.remoteSteamId) ++matches;
        if (matches == 1) remote = mode.remoteSteamId;
        else if (matches > 1) remote = 0;
    }
    bool requestHide = !shutdown && remote && fresh && mode.remoteSteamId == remote &&
        mode.mode == ERMC_PEER_MINECRAFT && mode.canRenderPeer == 1 && viewerCanComposite;
    g_peerRenderOwners.observe(peers, count, complete);
    bool hidden = false, remoteObserved = false;
    for (size_t i = 0; i < count; ++i) {
        // A stored pointer is never followed. Each object comes from this tick's
        // live list and must still have the exact instance/handle/identity/session.
        NativePeerIdentity current;
        if (!native_peer_at_slot(peers[i].slot, (uintptr_t)local->ins, &current) || !current.same(peers[i])) continue;
        auto* flags = (uint8_t*)(current.ins + kChrRenderFlags);
        if (!mem_readable(flags, 1)) continue;
        g_peerRenderOwners.apply(current, *flags, requestHide && current.steam == remote);
        if (current.steam == remote) {
            // Java may remove its avatar only once the native body is actually
            // visible. An engine-owned fade is hidden even when we own no bit.
            hidden = (*flags & 8) == 0;
            remoteObserved = true;
        }
    }
    if (!remoteObserved) remote = 0;
    if (shutdown) {
        g_peerLocalSteam = 0;
        g_peerRenderOwners.reset();
        remote = 0;
    }
    static uint64_t loggedRemote = 0;
    static uint32_t loggedMode = ERMC_PEER_UNKNOWN;
    static bool loggedHidden = false;
    uint32_t observedMode = remote && fresh && remote == mode.remoteSteamId ? mode.mode : ERMC_PEER_UNKNOWN;
    if (remote != loggedRemote || hidden != loggedHidden || observedMode != loggedMode) {
        log("peer-visibility: remote %llu mode %u hidden %u composite %u verified players %zu",
            (unsigned long long)remote, observedMode, unsigned(hidden), unsigned(viewerCanComposite), count);
        loggedRemote = remote; loggedMode = observedMode; loggedHidden = hidden;
    }
    publish_peer_info(remote, hidden);
}

static bool chr_dead(const Chr& c) {
    return *(int*)(c.data + kDataHp) <= 0 || (c.ins[kChrRenderFlags] & 0x80);
}

static bool can_target(const Player& me, const Chr& c) {
    if (!g_combatOk) {
        uint8_t team = c.ins[kChrTeamType];
        return team == 6 || team == 7;
    }
    return ((CanTarget_t)(g_base + combat::kCanTarget))(me.ins, c.ins) != 0;
}

// What was published last tick: Minecraft names characters by handle; the pointer is only
// trusted after re-checking its vtable and handle (block unloads free characters).
struct Published {
    uint64_t handle;
    uint8_t* ins;
};
static Published g_published[ERMC_MAX_ENTITIES];
static uint32_t g_publishedCount = 0;

static bool find_published(uint64_t handle, Chr* out) {
    for (uint32_t i = 0; i < g_publishedCount; i++) {
        if (g_published[i].handle != handle) continue;
        // Recheck at consumption as well as publication, including queued damage
        // and pending Kill retries. A previously published player is never trusted.
        return get_chr(g_published[i].ins, out) && !coop_excludes_proxy(*out) &&
               *(uint64_t*)(out->ins + kChrHandle) == handle;
    }
    return false;
}

constexpr size_t kWcmChrSets = 0x1DED8;       // ChrSet* [196]; entries 0x10: {ChrIns*, u8 state (4 = active)...}
constexpr int kDebugChrSet = 114;             // characters from the game's debug creator (our test enemies)

static uint32_t publish_entities(const Player& p) {
    ErmcEntityTable* t = shm_entities();
    static ErmcEntity local[ERMC_MAX_ENTITIES];
    static Published pub[ERMC_MAX_ENTITIES];
    uint32_t n = 0;
    g_nearestHostile.valid = false;
    float nearestSq = 1e30f;
    uint8_t* wcm = global_ptr(rva::kWorldChrMan, rva::kVtWorldChrMan);
    // Candidates: the game's list of active map characters (+0x1F1D0), plus the debug set,
    // which that list leaves out.
    static uint8_t* cands[1024 + 256];
    size_t nc = 0;
    if (wcm && mem_readable(wcm + kChrsByDistance, 0x18)) {
        uint8_t* beg = *(uint8_t**)(wcm + kChrsByDistance + 8);
        uint8_t* end = *(uint8_t**)(wcm + kChrsByDistance + 0x10);
        size_t count = (beg && end > beg) ? (size_t)(end - beg) / kChrByDistanceEntry : 0;
        if (count > 1024) count = 1024;
        if (count && mem_readable(beg, count * kChrByDistanceEntry))
            for (size_t i = 0; i < count; i++) cands[nc++] = *(uint8_t**)(beg + i * kChrByDistanceEntry);
    }
    if (wcm && mem_readable(wcm + kWcmChrSets + 8 * kDebugChrSet, 8)) {
        uint8_t* set = *(uint8_t**)(wcm + kWcmChrSets + 8 * kDebugChrSet);
        if (set && mem_readable(set, 0x20)) {
            uint32_t cap = *(uint32_t*)(set + 0x10);
            uint8_t* ent = *(uint8_t**)(set + 0x18);
            if (ent && cap > 0 && cap <= 256 && mem_readable(ent, (size_t)cap * 16))
                for (uint32_t i = 0; i < cap; i++)
                    if (ent[16 * i + 8] == 4 && *(uint8_t**)(ent + 16 * i)) cands[nc++] = *(uint8_t**)(ent + 16 * i);
        }
    }
    for (size_t i = 0; i < nc && n < ERMC_MAX_ENTITIES; i++) {
        Chr c;
        uint8_t* ins = cands[i];
        if (ins == p.ins || !get_chr(ins, &c, true)) continue;
        if (coop_excludes_proxy(c)) continue;
        // The list's own sort key isn't the distance: use the game's squared distance.
        float d2 = *(float*)(c.ins + kChrDistSq);
        if (!(d2 >= 0.0f) || d2 > kPublishRadius * kPublishRadius) continue;
        int model = *(int*)(c.ins + kChrModelId);
        if (model == 1000 || model == 100) continue;  // invisible event helpers
        int hp = *(int*)(c.data + kDataHp), mx = *(int*)(c.data + kDataMaxHp);
        if (mx <= 0) continue;
        uint64_t handle = *(uint64_t*)(c.ins + kChrHandle);
        bool dup = false;
        for (uint32_t k = 0; k < n && !dup; k++) dup = pub[k].handle == handle;
        if (dup) continue;
        const float* hpos = (const float*)(c.phys + kPhysPos);
        float h = *(float*)(c.phys + kPhysHitHeight), r = *(float*)(c.phys + kPhysHitRadius);
        if (!(h > 0.05f && h < 40.0f)) h = 1.8f;
        if (!(r > 0.05f && r < 15.0f)) r = 0.4f;
        bool hostile = can_target(p, c);
        uint8_t team = c.ins[kChrTeamType];
        ErmcEntity& o = local[n];
        memset(&o, 0, sizeof(o));
        o.id = handle;
        o.kind = !hostile ? ERMC_ENT_OTHER : (team == 7 || h > 3.0f) ? ERMC_ENT_LARGE_MONSTER : ERMC_ENT_SMALL_MONSTER;
        o.emId = (uint32_t)*(int*)(c.ins + kChrNpcParamId);
        to_stable(hpos, o.pos);
        memcpy(o.quat, c.phys + kPhysQuat, 16);
        o.boxCenter[0] = o.pos[0], o.boxCenter[1] = o.pos[1] + h * 0.5f, o.boxCenter[2] = o.pos[2];
        o.boxHalf[0] = r, o.boxHalf[1] = h * 0.5f, o.boxHalf[2] = r;
        o.hp = (float)(hp > 0 ? hp : 0);
        o.maxHp = (float)mx;
        o.flags = 2u | (chr_dead(c) ? 1u : 0u);  // world-aligned capsule box
        snprintf(o.name, sizeof(o.name), "c%04d", model);
        if (hostile && !(o.flags & 1u) && d2 < nearestSq) {
            nearestSq = d2;
            g_nearestHostile.valid = true;
            memcpy(g_nearestHostile.pos, o.pos, 12);
            g_nearestHostile.kind = o.kind;
            g_nearestHostile.handle = o.id;
        }
        pub[n] = {o.id, c.ins};
        n++;
    }
    t->seq = t->seq | 1;
    compiler_barrier();
    memcpy(t->entities, local, sizeof(ErmcEntity) * n);
    t->count = n;
    t->frame = shm_state()->frame;
    compiler_barrier();
    t->seq = (t->seq | 1) + 1;
    memcpy(g_published, pub, sizeof(Published) * n);
    g_publishedCount = n;
    return (uint32_t)nc;
}

static bool published_attacker(uint64_t handle, float* pos, uint32_t* kind) {
    ErmcEntityTable* t = shm_entities();
    for (uint32_t i = 0; i < t->count && i < ERMC_MAX_ENTITIES; i++) {
        if (t->entities[i].id != handle) continue;
        memcpy(pos, t->entities[i].pos, 12);
        *kind = t->entities[i].kind;
        return true;
    }
    return false;
}

static void clear_entities() {
    ErmcEntityTable* t = shm_entities();
    if (t->count == 0 && g_publishedCount == 0) return;
    t->seq = t->seq | 1;
    compiler_barrier();
    t->count = 0;
    compiler_barrier();
    t->seq = (t->seq | 1) + 1;
    g_publishedCount = 0;
    g_nearestHostile.valid = false;
}

// Minecraft damage points -> Elden Ring HP. An enemy with max HP H takes the hits a Minecraft
// mob with 20*sqrt(H/100) health would (clamped to 10..300): a 221-HP soldier ~30 (a few
// sword hits), a 6000-HP boss ~155, like Minecraft's own bosses.
static float er_damage(float mcAmount, int maxHp) {
    float mcHealth = 20.0f * sqrtf((float)maxHp / 100.0f);
    if (mcHealth < 10.0f) mcHealth = 10.0f;
    if (mcHealth > 300.0f) mcHealth = 300.0f;
    return mcAmount * (float)maxHp / mcHealth;
}

// -- the "poke": a player-owned Ruin Fragment, patched to flinch, for hit reaction and aggro

struct alignas(16) BulletSpawnData {
    uint64_t owner;                                  // +0x00
    int32_t behaviorId, magicId;                     // +0x08, +0x0C
    uint32_t unk10;                                  // +0x10
    int32_t bulletId, goodsId, dmyPolyId;            // +0x14, +0x18, +0x1C
    uint64_t target;                                 // +0x20
    uint32_t unk28, unk2c;                           // +0x28, +0x2C
    float unk30[4];                                  // +0x30
    uint32_t unk40, flags;                           // +0x40, +0x44
    uint8_t pad48[8];                                // +0x48
    float right[4], up[4], fwd[4], pos[4];           // +0x50, +0x60, +0x70, +0x80
    uint8_t rest[0x80];                              // +0x90..+0x10F
};
static_assert(sizeof(BulletSpawnData) == 0x110, "BulletSpawnData");
typedef uint32_t*(__fastcall* SpawnBullet_t)(void* mgr, uint32_t* outHandle, const BulletSpawnData* d, uint32_t* outErr);

constexpr int32_t kPokeBullet = 10176000;     // Ruin Fragment: the game's own "attract attention" throw
constexpr uint32_t kParamAtkPc = 8;           // SoloParamRepository index of AtkParam_Pc
constexpr size_t kAtkRowSize = 0x1C8;
static uint8_t* g_pokeRow = nullptr;          // AtkParam_Pc 10176000, patched while the core runs
static uint8_t g_pokeRowSaved[kAtkRowSize];

// A param row by id (binary search in the loaded param file); null if not found.
static uint8_t* param_row(uint32_t index, int32_t id) {
    uint8_t* rep = global_ptr(combat::kSoloParamRepository, 0);
    if (!rep || !mem_readable(rep + 0x88 + 0x48 * index, 8)) return nullptr;
    uint8_t* rescap = *(uint8_t**)(rep + 0x88 + 0x48 * index);
    if (!rescap || !mem_readable(rescap + 0x80, 8)) return nullptr;
    uint8_t* fd4rc = *(uint8_t**)(rescap + 0x80);
    if (!fd4rc || !mem_readable(fd4rc + 0x80, 8)) return nullptr;
    uint8_t* file = *(uint8_t**)(fd4rc + 0x80);
    if (!file || !mem_readable(file, 0x40)) return nullptr;
    uint16_t rows = *(uint16_t*)(file + 0x0A);
    if (!rows || !mem_readable(file + 0x40, (size_t)rows * 24)) return nullptr;
    int lo = 0, hi = rows - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t* d = file + 0x40 + (size_t)mid * 24;
        int32_t rid = *(const int32_t*)d;
        if (rid == id) {
            uint8_t* row = file + *(const uint64_t*)(d + 8);
            return mem_readable(row, kAtkRowSize) ? row : nullptr;
        }
        if (rid < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

// Hit reaction by the size of the Minecraft hit. dmgLevel is ATKPARAM_DMGTYPE_NEW (Paramdex /
// Smithbox ER): 1 SMALL (short stagger), 3 LARGE (long stagger), 7 SMALL_BLOW (launched back).
// Most vanilla melee (a netherite sword crit is 12) stays a small flinch; hits of 14+ (an axe
// crit, mod abilities, explosions) stagger, 24+ knock back. Poise still decides: superarmor
// shrugs it off.
struct PokeTier {
    uint8_t dmgLevel;
    float poise;       // atkSuperArmor
    float knockback;   // knockbackDist [m]
    const char* name;
};
static const PokeTier kPokeTiers[] = {
    {1, 12.0f, 0.0f, "flinch"},
    {3, 45.0f, 0.0f, "stagger"},
    {7, 90.0f, 3.0f, "knockback"},
};
constexpr uint64_t kPokeTierHoldMs = 400;  // a poke's flight: the row keeps its tier meanwhile
static int g_pokeTier = -1;
static uint64_t g_pokeTierUntilMs = 0;

static int poke_tier_for(const ErmcDamage& d) {
    bool outward = (d.flags & ERMC_DAMAGE_OUTWARD) != 0;
    if (d.amount >= 24.0f || (outward && d.amount >= 14.0f)) return 2;
    if (d.amount >= 14.0f) return 1;
    return 0;
}

static void set_poke_tier(int tier) {
    if (!g_pokeRow || tier == g_pokeTier) return;
    const PokeTier& t = kPokeTiers[tier];
    // Tier 0 is the bridge's original poke: the row's own knockbackDist, untouched.
    float knockback = tier == 0 ? *(const float*)(g_pokeRowSaved + 0x10) : t.knockback;
    g_pokeRow[0x72] = t.dmgLevel;                     // dmgLevel
    *(float*)(g_pokeRow + 0x84) = t.poise;            // atkSuperArmor: poise damage
    *(float*)(g_pokeRow + 0x10) = knockback;          // knockbackDist
    g_pokeTier = tier;
}

// One patched row serves every poke, and a poke in flight may read it when it lands: within
// kPokeTierHoldMs of a poke the row only goes up a tier, never down. Only a poke that wanted
// its tier renews the hold, so a stream of light hits after a big one drops back to a flinch.
static int claim_poke_tier(int want) {
    uint64_t now = now_ms();
    int tier = want;
    if (now < g_pokeTierUntilMs && g_pokeTier > tier) tier = g_pokeTier;
    set_poke_tier(tier);
    if (tier == want) g_pokeTierUntilMs = now + kPokeTierHoldMs;
    return tier;
}

// 1 physical damage as vanilla; the reaction comes from the tier. In memory only: params are
// rebuilt from regulation.bin at every launch; restored when the core unloads.
static void patch_poke_row() {
    if (g_pokeRow) return;
    uint8_t* row = param_row(kParamAtkPc, kPokeBullet);
    if (!row) {
        log("combat: AtkParam_Pc %d not found; no hit reactions", kPokeBullet);
        return;
    }
    memcpy(g_pokeRowSaved, row, kAtkRowSize);
    g_pokeRow = row;
    g_pokeTier = -1;
    set_poke_tier(0);
    log("combat: poke attack row patched (AtkParam_Pc %d)", kPokeBullet);
}

static void restore_poke_row() {
    if (g_pokeRow && mem_readable(g_pokeRow, kAtkRowSize)) memcpy(g_pokeRow, g_pokeRowSaved, kAtkRowSize);
    g_pokeRow = nullptr;
    g_pokeTier = -1;
}

// Spawns the player's poke bullet at `from` (Havok), flying along `fwd`.
static bool spawn_poke(const Player& me, const float* from, const float* fwdIn) {
    uint8_t* mgr = global_ptr(combat::kCSBulletManager, 0);
    if (!mgr || !g_pokeRow) return false;
    float fwd[3] = {fwdIn[0], fwdIn[1], fwdIn[2]};
    normalize(fwd);
    float up[3] = {0, 1, 0}, right[3];
    if (fabsf(fwd[1]) > 0.95f) up[0] = 1, up[1] = 0;
    cross(up, fwd, right);  // left-handed, as for the camera
    normalize(right);
    cross(fwd, right, up);
    BulletSpawnData d;
    memset(&d, 0, sizeof(d));
    d.owner = *(uint64_t*)(me.ins + kChrHandle);
    d.behaviorId = d.magicId = d.goodsId = d.dmyPolyId = -1;
    d.bulletId = kPokeBullet;
    d.target = ~0ull;
    memcpy(d.right, right, 12);
    memcpy(d.up, up, 12);
    memcpy(d.fwd, fwd, 12);
    memcpy(d.pos, from, 12);
    d.pos[3] = 1.0f;
    uint32_t handle = 0xFFFFFFFFu, err = 0;
    ((SpawnBullet_t)(g_base + combat::kSpawnBullet))(mgr, &handle, &d, &err);
    if (handle == 0xFFFFFFFFu) log("combat: poke bullet failed (error %u)", err);
    return handle != 0xFFFFFFFFu;
}

static bool poke(const Player& me, const Chr& victim) {
    uint8_t* mgr = global_ptr(combat::kCSBulletManager, 0);
    if (!mgr || !g_pokeRow) return false;
    const float* vp = (const float*)(victim.phys + kPhysPos);
    const float* pp = (const float*)(me.phys + kPhysPos);
    float h = *(float*)(victim.phys + kPhysHitHeight), r = *(float*)(victim.phys + kPhysHitRadius);
    if (!(h > 0.05f && h < 40.0f)) h = 1.8f;
    if (!(r > 0.05f && r < 15.0f)) r = 0.4f;
    float fwd[3] = {vp[0] - pp[0], 0.0f, vp[2] - pp[2]};
    if (fwd[0] * fwd[0] + fwd[2] * fwd[2] < 1e-4f) fwd[2] = 1.0f;
    normalize(fwd);
    float up[3] = {0, 1, 0}, right[3];
    cross(up, fwd, right);  // left-handed, as for the camera
    BulletSpawnData d;
    memset(&d, 0, sizeof(d));
    d.owner = *(uint64_t*)(me.ins + kChrHandle);  // the hit, aggro and kill credit are the player's
    d.behaviorId = d.magicId = d.goodsId = d.dmyPolyId = -1;
    d.bulletId = kPokeBullet;
    d.target = ~0ull;
    float back = r + 0.6f;
    memcpy(d.right, right, 12);
    memcpy(d.up, up, 12);
    memcpy(d.fwd, fwd, 12);
    d.pos[0] = vp[0] - fwd[0] * back;
    d.pos[1] = vp[1] + 0.6f * h;
    d.pos[2] = vp[2] - fwd[2] * back;
    d.pos[3] = 1.0f;
    uint32_t handle = 0xFFFFFFFFu, err = 0;
    ((SpawnBullet_t)(g_base + combat::kSpawnBullet))(mgr, &handle, &d, &err);
    if (handle == 0xFFFFFFFFu) log("combat: poke bullet failed (error %u)", err);
    return handle != 0xFFFFFFFFu;
}

// Kills that the game refused this tick (an animation state can block Kill): retried a while.
struct PendingKill {
    uint64_t handle;
    uint64_t untilMs;
};
static PendingKill g_pendingKills[16];

static void kill_chr(const Chr& c) {
    ((void(__fastcall*)(void*, uint8_t))(g_base + rva::kKill))(c.ins, 0);  // drops, runes to the player
    if (c.ins[kChrRenderFlags] & 0x80) return;
    uint64_t h = *(uint64_t*)(c.ins + kChrHandle);
    for (PendingKill& k : g_pendingKills) {
        if (k.handle == 0 || k.handle == h) {
            k = {h, now_ms() + 2000};
            return;
        }
    }
}

static void retry_kills() {
    for (PendingKill& k : g_pendingKills) {
        if (!k.handle) continue;
        Chr c;
        if (now_ms() > k.untilMs || !find_published(k.handle, &c) || (c.ins[kChrRenderFlags] & 0x80)) {
            k.handle = 0;
            continue;
        }
        ((void(__fastcall*)(void*, uint8_t))(g_base + rva::kKill))(c.ins, 0);
    }
}

// Per-target poke rate limit (the bullet manager has 128 bullets).
static struct {
    uint64_t handle;
    uint64_t lastMs;
} g_pokeTimes[32];

static bool poke_allowed(uint64_t handle) {
    uint64_t now = now_ms();
    int oldest = 0;
    for (int i = 0; i < 32; i++) {
        if (g_pokeTimes[i].handle == handle) {
            if (now - g_pokeTimes[i].lastMs < 250) return false;
            g_pokeTimes[i].lastMs = now;
            return true;
        }
        if (g_pokeTimes[i].lastMs < g_pokeTimes[oldest].lastMs) oldest = i;
    }
    g_pokeTimes[oldest] = {handle, now};
    return true;
}

static void apply_hit(const Player& me, const Chr& c, const ErmcDamage& d) {
    if (chr_dead(c)) return;
    int hp = *(int*)(c.data + kDataHp), mx = *(int*)(c.data + kDataMaxHp);
    if (mx <= 0) return;
    int dmg = (int)ceilf(er_damage(d.amount, mx));
    if (dmg < 1) dmg = 1;
    int model = *(int*)(c.ins + kChrModelId);
    const char* crit = (d.flags & ERMC_DAMAGE_CRITICAL) ? " critical" : "";
    if (!g_combatOk) {  // unknown build: plain HP write + Kill
        if (hp - dmg <= 0) kill_chr(c);
        else *(int*)(c.data + kDataHp) = hp - dmg;
        log("combat: Minecraft hit c%04d for %.1f -> %d HP (%d/%d)%s [basic]", model, d.amount, dmg, hp - dmg > 0 ? hp - dmg : 0, mx, crit);
        return;
    }
    // A Minecraft mob's hit (a zombie on a boss) must not turn the enemy on the player: no
    // credit, no player-owned poke. The player's own hits credit it (on-kill effects, runes).
    bool byPlayer = !(d.flags & ERMC_DAMAGE_NOT_BY_PLAYER);
    if (byPlayer) *(uint64_t*)(c.ins + kChrLastHitBy) = *(uint64_t*)(me.ins + kChrHandle);
    ((ModifyHp_t)(g_base + combat::kModifyHp))(c.data, -dmg, 0, 0, 0.0f, 1.0f, 1);
    int after = *(int*)(c.data + kDataHp);
    if (after == hp) {
        log("combat: Minecraft hit c%04d for %.1f: invincible right now", model, d.amount);
        return;
    }
    if (after > 0) {
        const char* reaction = "";
        if (byPlayer && poke_allowed(*(uint64_t*)(c.ins + kChrHandle))) {
            int tier = claim_poke_tier(poke_tier_for(d));
            if (poke(me, c)) reaction = kPokeTiers[tier].name;
        }
        log("combat: Minecraft %s hit c%04d for %.1f -> %d HP (%d/%d)%s%s%s", byPlayer ? "player" : "mob", model, d.amount,
            hp - after, after, mx, crit, *reaction ? ", " : "", reaction);
        return;
    }
    log("combat: Minecraft hit c%04d for %.1f -> %d HP: defeated%s", model, d.amount, hp, crit);
    kill_chr(c);
}

// -- a test enemy (dev): the game's debug character creator spawns a Godrick soldier ----------
// In memory only (no map entry, no event id): gone at the next map load. Removed with
// ForceDeath(chr, false) (no drops, no runes).
static uint8_t* g_testEnemy = nullptr;
static uint8_t* g_testEnemyPrev = nullptr;
static bool g_testRequested = false;

static uint8_t* debug_creator() {
    uint8_t* wcm = global_ptr(rva::kWorldChrMan, rva::kVtWorldChrMan);
    if (!wcm || !mem_readable(wcm + 0x1E648, 8)) return nullptr;
    uint8_t* dc = typed(*(void**)(wcm + 0x1E648), combat::kVtDebugChrCreator);
    return (dc && mem_readable(dc, 0x1C0)) ? dc : nullptr;
}

static void request_test_enemy(const Player& me) {
    uint8_t* dc = debug_creator();
    if (!dc || dc[0x44]) {
        log("combat: debug creator %s", dc ? "busy" : "not found");
        return;
    }
    // 5 m in front of the camera, at the Tarnished's height.
    uint8_t* cam = final_camera();
    const float* pp = (const float*)(me.phys + kPhysPos);
    float fx = 0, fz = 1;
    if (cam) {
        const float* m = (const float*)(cam + kCamMatrix);
        fx = m[8], fz = m[10];
        float l = sqrtf(fx * fx + fz * fz);
        if (l > 1e-3f) fx /= l, fz /= l;
    }
    float* pos = (float*)(dc + 0xB0);
    pos[0] = pp[0] + fx * 5.0f, pos[1] = pp[1] + 0.2f, pos[2] = pp[2] + fz * 5.0f, pos[3] = 1.0f;
    *(int32_t*)(dc + 0xF0) = 30000014;  // NpcParam: Godrick Soldier (as in Stormveil)
    *(int32_t*)(dc + 0xF4) = 30000000;  // NpcThinkParam: their AI
    *(int32_t*)(dc + 0xF8) = 0;         // no event entity: touches no event flags
    *(int32_t*)(dc + 0xFC) = 0;
    uint16_t* name = (uint16_t*)(dc + 0x100);
    memset(name, 0, 0x40);
    const char* m = "c3000";
    for (int i = 0; m[i]; i++) name[i] = (uint16_t)m[i];
    *(uint32_t*)(dc + 0x19C) = 1;
    g_testEnemyPrev = *(uint8_t**)(dc + 0x1B0);
    dc[0x44] = 1;  // the game's own steps load and create it
    g_testRequested = true;
    log("combat: test enemy requested (c3000 soldier, 5 m ahead)");
}

// Every character in the debug set is one of our test enemies: remove them all.
static void remove_test_enemy(const char* why) {
    g_testEnemy = nullptr;
    if (!g_combatOk) return;
    uint8_t* wcm = global_ptr(rva::kWorldChrMan, rva::kVtWorldChrMan);
    if (!wcm || !mem_readable(wcm + kWcmChrSets + 8 * kDebugChrSet, 8)) return;
    uint8_t* set = *(uint8_t**)(wcm + kWcmChrSets + 8 * kDebugChrSet);
    if (!set || !mem_readable(set, 0x20)) return;
    uint32_t cap = *(uint32_t*)(set + 0x10);
    uint8_t* ent = *(uint8_t**)(set + 0x18);
    if (!ent || cap == 0 || cap > 256 || !mem_readable(ent, (size_t)cap * 16)) return;
    int n = 0;
    for (uint32_t i = 0; i < cap; i++) {
        Chr c;
        uint8_t* ins = *(uint8_t**)(ent + 16 * i);
        if (!ins || ent[16 * i + 8] != 4 || !get_chr(ins, &c) || chr_dead(c)) continue;
        ((ForceDeath_t)(g_base + combat::kForceDeath))(c.ins, false);  // no drops, no runes
        n++;
    }
    if (n) log("combat: %d test enem%s removed (%s)", n, n == 1 ? "y" : "ies", why);
}

static void service_test_enemy(const Player& me) {
    ErmcHeader* h = shm_header();
    if (h->debugFlags & 4) {  // bit2: spawn a test soldier
        h->debugFlags = h->debugFlags & ~4u;
        request_test_enemy(me);
    }
    if (h->debugFlags & 8) {  // bit3: remove it (no drops, no runes)
        h->debugFlags = h->debugFlags & ~8u;
        remove_test_enemy("requested");
    }
    if (g_testRequested) {
        uint8_t* dc = debug_creator();
        uint8_t* made = dc ? *(uint8_t**)(dc + 0x1B0) : nullptr;
        if (made && made != g_testEnemyPrev) {
            g_testEnemy = made;
            g_testRequested = false;
            log("combat: test enemy created: %p", (void*)made);
        } else if (dc && !dc[0x44] && !made) {
            g_testRequested = false;
            log("combat: debug creator finished without a character");
        }
    }
}

// ---------------------------------------------------------------------------------------
// Elden Ring's "Event Action" from Minecraft: when the
// game offers a prompt (door, lever, item, grace, lost runes), a press is one byte in the
// action-button manager. The selected prompt's own handler consumes it in the next frame's
// event update (task group 25), exactly as a real tap would. No input injection, no calls.

// CSActionButtonManImp (0xC0 bytes)
constexpr size_t kAbSelected = 0x20;   // entry* of the prompt on offer, null = none
constexpr size_t kAbCanExec = 0x29;    // u8: 0 while a talk, a menu or an item popup blocks actions
constexpr size_t kAbGrayed = 0x2B;     // u8: the prompt is grayed out, a press does nothing
constexpr size_t kAbTextId = 0x2C;     // i32 ActionButtonText id of the prompt on offer, -1 = none
constexpr size_t kAbConsumed = 0x80;   // u8: set by the handler that took the press
constexpr size_t kAbPressed = 0x81;    // u8: "Event Action tapped" latch; the game clears it once used
constexpr size_t kAbeParamId = 0x08;   // entry: ActionButtonParam row id

// The game code that reads and writes those fields. A mismatch turns only actions off.
static const CodeSig kActionSigs[] = {
    {0xA658A1, "80 BB 88 00 00 00 00 74 09 66 C7 83 81 00 00 00 01 01", "ActionButtonMan::UpdateInput (+0x81)"},
    {0xA656CC, "80 BB 80 00 00 00 00 74 10 66 C7 83 80 00 00 00 00 00 C6 83 82 00 00 00 00",
     "ActionButtonMan::UpdateInput (+0x80)"},
    {0xA64FA0, "4C 8B C1 48 85 D2 74 61 0F B6 4A 20 F6 C1 04 74 58 41 80 B8 81 00 00 00 00", "ActionButtonMan::ConsumePress"},
    {0xA64E61, "38 43 28 74 51 48 8B 4B 20 48 85 C9 74 48 0F B6 41 20 A8 08 75 40 A8 01 74 04 C6 43 2B 01 "
               "F6 41 20 20 74 04 C6 43 2B 00 8B 41 10 89 43 2C", "ActionButtonMan::Update (prompt)"},
    {0x266FC40, "3B 51 10 73 29 44 3B 41 14 73 23 48 8B 41 08", "MsgRepositoryImp::GetMsg"},
    {0x2670510, "44 8B 41 0C 45 33 C9 41 FF C8 4C 8B D9 3B 51 2C", "FMG id lookup"},
};
static bool g_actionOk = false;

// Initialization-only bounded snapshots. A readable data page must not count
// as a valid hook/trampoline: every byte also has to be committed executable
// memory. Keep the guarded copy after validation to handle a concurrent unmap.
static bool read_action_code(uint64_t address, void* out, size_t size) {
    if (!address || !size || size > 64 || address > UINTPTR_MAX - size) return false;
    const uintptr_t start = (uintptr_t)address, end = start + size;
    if (!mem_readable((const void*)start, size)) return false;
    for (uintptr_t cursor = start; cursor < end;) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery((const void*)cursor, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        const uintptr_t region = (uintptr_t)mbi.BaseAddress;
        if (region > cursor || mbi.RegionSize > UINTPTR_MAX - region) return false;
        const uintptr_t next = region + mbi.RegionSize;
        if (next <= cursor) return false;
        cursor = next < end ? next : end;
    }
    return mem_read((const void*)start, out, size);
}

static bool action_target_in_module_code(HMODULE module, uint64_t target) {
    const uintptr_t base = (uintptr_t)module;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (target < base || !VirtualQuery((const void*)(uintptr_t)target, &mbi, sizeof(mbi)) ||
        mbi.Type != MEM_IMAGE || mbi.AllocationBase != module) return false;
    IMAGE_DOS_HEADER dos = {};
    if (!mem_read((const void*)base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < (LONG)sizeof(dos) || dos.e_lfanew > 0x100000 ||
        base > UINTPTR_MAX - (uintptr_t)dos.e_lfanew - sizeof(IMAGE_NT_HEADERS64)) return false;
    const uintptr_t ntAddress = base + dos.e_lfanew;
    IMAGE_NT_HEADERS64 nt = {};
    if (!mem_read((const void*)ntAddress, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96 ||
        target - base >= nt.OptionalHeader.SizeOfImage) return false;
    const uintptr_t sections = ntAddress + sizeof(nt);
    const size_t sectionBytes = (size_t)nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (sections - base > nt.OptionalHeader.SizeOfImage ||
        sectionBytes > nt.OptionalHeader.SizeOfImage - (sections - base)) return false;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section = {};
        if (!mem_read((const void*)(sections + (size_t)i * sizeof(section)), &section, sizeof(section))) return false;
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            section.VirtualAddress > nt.OptionalHeader.SizeOfImage ||
            section.Misc.VirtualSize > nt.OptionalHeader.SizeOfImage - section.VirtualAddress) continue;
        if (target - base >= section.VirtualAddress &&
            target - base - section.VirtualAddress < section.Misc.VirtualSize) {
            uint8_t code = 0;
            return read_action_code(target, &code, 1);
        }
    }
    return false;
}

static bool matches_coop_action_detour(uintptr_t rvaAddr) {
    using namespace action_detour;
    HMODULE seamless = GetModuleHandleW(L"ersc.dll");
    if (!g_coopExcludePlayerIns || !seamless) return false;
    Entry entry;
    if (rvaAddr == entry_rva(Entry::ConsumePress)) entry = Entry::ConsumePress;
    else if (rvaAddr == entry_rva(Entry::GetMsg)) entry = Entry::GetMsg;
    else return false;  // the other four anchors must always match exactly
    if (g_base > UINTPTR_MAX - rvaAddr) return false;
    uint8_t live[25] = {}, relay[14] = {}, trampoline[33] = {}, handler[28] = {};
    Evidence evidence;
    evidence.coopEnabled = true;
    evidence.seamlessLoaded = true;
    evidence.imageBase = g_base;
    evidence.seamlessBase = (uintptr_t)seamless;
    evidence.live = {live, signature_size(entry)};
    evidence.relay = {relay, sizeof(relay)};
    evidence.handler = {handler, sizeof(handler)};
    evidence.trampoline = {trampoline, trampoline_size(entry)};
    if (!read_action_code(g_base + rvaAddr, live, evidence.live.size) ||
        !relative_target(g_base + rvaAddr, evidence.live, &evidence.relayAddress) ||
        evidence.relayAddress < evidence.trampoline.size ||
        !read_action_code(evidence.relayAddress, relay, sizeof(relay)) ||
        !read_action_code(evidence.relayAddress - evidence.trampoline.size, trampoline, evidence.trampoline.size)) return false;
    evidence.handlerAddress = u64(relay + 6);
    if (!read_action_code(evidence.handlerAddress, handler, sizeof(handler)) || !matches(entry, evidence)) return false;
    // Verify actual module/section ownership, never "nearest module below".
    // Interaction still uses only the existing +0x81 byte latch, with no calls
    // to any hook, relay, handler, or trampoline and no executable-memory writes.
    return action_target_in_module_code(seamless, u64(handler + 17));
}

static uint8_t* action_man() {
    uint8_t* m = global_ptr(rva::kCSActionButtonMan, rva::kVtActionButtonMan);
    return (m && mem_readable(m, 0xC0)) ? m : nullptr;
}

static uint64_t g_pressMs = 0;      // when we set the latch; 0 = no press pending
static int32_t g_pressText = -1;

// 1 done, 0 nothing on offer (or blocked/grayed), -1 unsupported build, -2 needs Elden Ring
// input (ladders: up/down).
static int perform_action(const Player& p) {
    (void)p;
    if (!g_actionOk) return -1;
    uint8_t* man = action_man();
    if (!man) return -1;
    uint8_t* sel = *(uint8_t**)(man + kAbSelected);
    int32_t text = *(int32_t*)(man + kAbTextId);
    if (!sel || text < 0) return 0;
    if (!man[kAbCanExec] || man[kAbGrayed]) return 0;
    int32_t param = mem_readable(sel + kAbeParamId, 4) ? *(int32_t*)(sel + kAbeParamId) : -1;
    if (param == 5000 || param == 5010) return -2;  // ladders: "Climb", "Descend"
    man[kAbPressed] = 1;
    g_pressMs = now_ms();
    g_pressText = text;
    log("action: Event Action on prompt %d (ActionButtonParam %d)", text, param);
    return 1;
}

// Every tick while a press is pending: confirm it, and never leave a stale latch that could
// fire later on a different prompt.
static void check_action_press() {
    if (!g_pressMs) return;
    uint8_t* man = action_man();
    if (!man) {
        g_pressMs = 0;
        return;
    }
    if (man[kAbConsumed]) {
        log("action: the game took the press (prompt %d)", g_pressText);
        g_pressMs = 0;
    } else if (!man[kAbPressed]) {
        log("action: press latch cleared by the game (prompt %d)", g_pressText);
        g_pressMs = 0;
    } else if (now_ms() - g_pressMs > 500) {
        man[kAbPressed] = 0;
        log("action: press not taken within 0.5 s; cleared");
        g_pressMs = 0;
    }
}

// ActionButtonText is FMG category 0x20 (DLC: 0x16D, 0x1D1), read straight from the loaded
// file like MsgRepositoryImp::GetMsg does. UTF-16, owned by the game.
static const wchar_t* fmg_text(uint32_t category, int32_t id) {
    uint8_t* repo = global_ptr(rva::kMsgRepository, rva::kVtMsgRepository);
    if (!repo || id < 0 || !mem_readable(repo, 0x18)) return nullptr;
    uint8_t** versions = *(uint8_t***)(repo + 0x08);
    if (!versions || *(uint32_t*)(repo + 0x10) == 0 || category >= *(uint32_t*)(repo + 0x14)) return nullptr;
    uint8_t** files = mem_readable(versions, 8) ? (uint8_t**)versions[0] : nullptr;
    if (!files || !mem_readable(files + category, 8)) return nullptr;
    uint8_t* fmg = files[category];
    if (!fmg || !mem_readable(fmg, 0x28)) return nullptr;
    uint32_t groups = *(uint32_t*)(fmg + 0x0C);
    const int64_t* offs = *(const int64_t**)(fmg + 0x18);
    if (!offs || groups > 100000 || !mem_readable(fmg + 0x28, 16 * (size_t)groups)) return nullptr;
    for (uint32_t g = 0; g < groups; g++) {
        const uint32_t* grp = (const uint32_t*)(fmg + 0x28 + 16 * g);  // {first index, first id, last id, -}
        if ((uint32_t)id < grp[1] || (uint32_t)id > grp[2]) continue;
        const int64_t* slot = offs + grp[0] + ((uint32_t)id - grp[1]);
        if (!mem_readable(slot, 8) || *slot <= 0 || !mem_readable(fmg + *slot, 2)) return nullptr;
        return (const wchar_t*)(fmg + *slot);
    }
    return nullptr;
}

static int32_t g_promptShown = -2;  // text id currently in hostPrompt (-2 = never published)

// Every tick; usable = the Tarnished is alive and standing in for Minecraft's player.
static void publish_prompt(bool usable) {
    int32_t text = -1;
    uint8_t* man = (usable && g_actionOk) ? action_man() : nullptr;
    if (man && *(void**)(man + kAbSelected) && man[kAbCanExec]) text = *(int32_t*)(man + kAbTextId);
    if (text == g_promptShown) return;
    g_promptShown = text;
    char buf[64] = {0};
    const wchar_t* w = fmg_text(0x20, text);
    if (!w) w = fmg_text(0x16D, text);
    if (!w) w = fmg_text(0x1D1, text);
    if (w && !WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr)) buf[0] = 0;
    if (!buf[0] && text >= 0) snprintf(buf, sizeof(buf), "Action %d", text);
    ErmcHeader* h = shm_header();
    h->hostPromptSeq = h->hostPromptSeq + 1;  // odd: being written
    compiler_barrier();
    memcpy((void*)h->hostPrompt, buf, sizeof(buf));
    compiler_barrier();
    h->hostPromptSeq = h->hostPromptSeq + 1;  // even: stable
    if (text >= 0) log("action: prompt \"%s\" (text %d)", buf, text);
}

static uint32_t g_actionSeen = 0;
static bool g_actionInit = false;

static void service_action(const Player& p) {
    ErmcHeader* h = shm_header();
    uint32_t req = h->mcActionReq;
    if (!g_actionInit) {
        g_actionSeen = req;
        g_actionInit = true;
        return;
    }
    if (req == g_actionSeen) return;
    g_actionSeen = req;
    int r = perform_action(p);
    h->hostActionResult = r;
    compiler_barrier();
    h->hostActionAck = req;
    log("action: Minecraft asked for Elden Ring's action -> %d", r);
}

static void service_damage(const Player& me) {
    ErmcDamageQueue* q = shm_damage();
    uint32_t w = q->write;
    compiler_barrier();
    uint32_t r = q->read;
    if (w - r > ERMC_DAMAGE_RING) r = w - ERMC_DAMAGE_RING;  // fell behind: drop the oldest
    for (; r != w; r++) {
        ErmcDamage d = q->ring[r % ERMC_DAMAGE_RING];
        if (d.id == 0) {
            // Minecraft hit Elden Ring's ground or a prop (a terrain block): strike that point with
            // the player's poke, which breaks breakable objects (crates, pots) like any attack.
            float target[3], from[3];
            to_havok(d.hitPos, target);
            const float* pp = (const float*)(me.phys + kPhysPos);
            float dir[3] = {target[0] - pp[0], target[1] - (pp[1] + 1.2f), target[2] - pp[2]};
            float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (len > 0.3f && len < 8.0f) {
                for (int k = 0; k < 3; k++) from[k] = target[k] - dir[k] / len * 0.8f;  // 0.8 m before the point
                claim_poke_tier(0);
                bool ok = spawn_poke(me, from, dir);
                log("combat: Minecraft struck the world at %.1f %.1f %.1f (%s)", d.hitPos[0], d.hitPos[1], d.hitPos[2],
                    ok ? "poke" : "no bullet");
            }
            continue;
        }
        Chr c;
        if (!find_published(d.id, &c)) continue;
        if (!(d.amount > 0.0f && d.amount < 10000.0f)) continue;
        if (!can_target(me, c)) continue;  // friendly and quest NPCs can't be hurt from Minecraft
        apply_hit(me, c, d);
    }
    q->read = r;
    retry_kills();
}

// ---------------------------------------------------------------------------------------
// Ray queries (game thread)

typedef bool(__fastcall* CastRay_t)(void* world, uint32_t filter, const float* origin, const float* delta,
                                    float* hit, void* ignore);

static void* ray_world() {
    uint8_t* havok = global_ptr(rva::kCSHavokMan, 0);
    if (!havok || !mem_readable(havok + kHavokPhysWorld, 8)) return nullptr;
    return *(void**)(havok + kHavokPhysWorld);
}

static bool raycast(const float* s, const float* e, uint32_t filter, void* ignore, ErmcRayHit* out) {
    memset(out, 0, sizeof(*out));
    if (!movement_position_finite(s) || !movement_position_finite(e) || !g_frame.valid) return false;
    void* world = ray_world();
    if (!world) return false;
    alignas(16) float o[4], d[4], hit[4] = {0, 0, 0, 0};
    float hs[3], he[3];
    to_havok(s, hs);
    to_havok(e, he);
    if (!movement_position_finite(hs) || !movement_position_finite(he)) return false;
    o[0] = hs[0], o[1] = hs[1], o[2] = hs[2], o[3] = 1.0f;
    d[0] = he[0] - hs[0], d[1] = he[1] - hs[1], d[2] = he[2] - hs[2], d[3] = 0.0f;
    if (!movement_position_finite(d)) return false;
    bool r = ((CastRay_t)(g_base + rva::kCastRay))(world, filter, o, d, hit, ignore);
    if (r) {
        if (!movement_position_finite(hit)) return false;
        out->hit = 1;
        to_stable(hit, out->pos);
        // CastRay reports no normal: downward rays hit ground, anything else faces the ray.
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > 1e-6f && -d[1] / len > 0.7f) {
            out->normal[1] = 1.0f;
        } else if (len > 1e-6f) {
            out->normal[0] = -d[0] / len, out->normal[1] = -d[1] / len, out->normal[2] = -d[2] / len;
        }
    }
    return r;
}

static volatile LONG g_dbgRayState = 0;  // 0 idle, 1 requested, 2 done
static float g_dbgRayStart[3], g_dbgRayEnd[3];
static uint32_t g_dbgRayFlags, g_dbgFilter;
static ErmcRayHit g_dbgRayHit;
static volatile LONG g_tickAlive = 0;

bool game_debug_raycast(const float* start, const float* end, uint32_t flags, const uint32_t* filter, ErmcRayHit* out) {
    if (!g_tickAlive) return false;
    memcpy(g_dbgRayStart, start, 12);
    memcpy(g_dbgRayEnd, end, 12);
    g_dbgRayFlags = flags;
    g_dbgFilter = filter ? filter[0] : 0;
    InterlockedExchange(&g_dbgRayState, 1);
    for (int i = 0; i < 1000 && g_dbgRayState != 2; i++) Sleep(1);
    bool ok = g_dbgRayState == 2;
    if (ok) *out = g_dbgRayHit;
    InterlockedExchange(&g_dbgRayState, 0);
    return ok;
}

bool game_debug_fire_shell(uint32_t, const float*, const float*, uint64_t*) {
    return false;  // attacks come in a later stage
}

static uint32_t service_rays(void* ignore, bool consumer) {
    uint32_t cast = 0;
    if (g_dbgRayState == 1) {
        uint32_t f = (g_dbgRayFlags & ERMC_RAYS_CUSTOM_FILTER) ? g_dbgFilter : kDefaultRayFilter;
        raycast(g_dbgRayStart, g_dbgRayEnd, f, (g_dbgRayFlags & ERMC_RAYS_HIT_SELF) ? nullptr : ignore, &g_dbgRayHit);
        InterlockedExchange(&g_dbgRayState, 2);
        ++cast;
    }
    // The explicit debug ray above is independent of Minecraft. A stale terrain
    // mailbox is not permission to spend game-thread time casting thousands of rays.
    if (!consumer) return cast;
    ErmcRayHeader* h = shm_rays();
    uint32_t req = h->reqSeq;
    if (req == h->respSeq) return cast;
    // A physics world that is still loading is not a batch of genuine misses.
    // Keep the mailbox pending so Minecraft holds safely and retries/waits.
    if (!ray_world()) return cast;
    compiler_barrier();
    uint32_t count = h->count < ERMC_MAX_RAYS ? h->count : ERMC_MAX_RAYS;
    uint32_t done = h->processed;
    if (done > count) done = 0;
    const ErmcRay* rays = (const ErmcRay*)((uint8_t*)h + ERMC_RAYS_OFF_RAYS);
    ErmcRayHit* hits = (ErmcRayHit*)((uint8_t*)h + ERMC_RAYS_OFF_HITS);
    uint32_t filter = (h->flags & ERMC_RAYS_CUSTOM_FILTER) ? h->filterA : kTerrainRayFilter;
    LARGE_INTEGER fq, t0, t;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    const LONGLONG budget = fq.QuadPart / 500;  // ~2 ms per frame
    while (done < count) {
        raycast(rays[done].start, rays[done].end, filter, ignore, &hits[done]);
        done++;
        ++cast;
        // One engine cast can exceed the budget by itself; do not add another
        // fifteen casts before noticing it. Keep unfinished answers pending.
        QueryPerformanceCounter(&t);
        if (t.QuadPart - t0.QuadPart >= budget) break;
    }
    h->processed = done;
    if (done >= count) {
        compiler_barrier();
        h->respSeq = req;
        h->processed = 0;
    }
    return cast;
}

// ---------------------------------------------------------------------------------------
// F8 switching between the games

static bool g_f8Down = false;
static uint32_t g_focusSeen = 0;
static bool g_focusSeenInit = false;
static bool g_hostFocusChanged = false;

static void return_to_host(const char* why) {
    HWND hwnd = game_hwnd();
    if (!hwnd) return;
    g_hostFocusChanged = true;
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    log("switch: %s (foreground %s)", why, GetForegroundWindow() == hwnd ? "ok" : "not yet");
}

static void handle_switching() {
    g_hostFocusChanged = false;
    HWND hwnd = game_hwnd();
    if (!hwnd) return;
    bool focused = GetForegroundWindow() == hwnd;
#if ERMC_NATIVE_WINDOWS
    // The foreground host may explicitly hand focus to the live Minecraft process.
    // Grant once per consumer session, and again on the user's F8 handoff.
    static uint32_t grantedPid = 0;
    static uint64_t grantedStart = 0;
    ErmcHeader* h = shm_header();
    if (!mc_consumer_active()) {
        grantedPid = 0;
        grantedStart = 0;
    } else if (focused && (h->mcPid != grantedPid || h->mcStartMs != grantedStart)) {
        grantedPid = h->mcPid;
        grantedStart = h->mcStartMs;
        BOOL granted = AllowSetForegroundWindow(grantedPid);
        log("switch: initial Minecraft foreground grant %s", granted ? "ok" : "denied");
    }
#endif
    // Losing focus while F8 is still physically held is not a key release.
    // Otherwise the same press bounces between the two windows every tick.
    bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (focused && down && !g_f8Down) {
        ErmcHeader* h = shm_header();
        InterlockedExchange(&g_controlRevoked, 0);
        g_holdLease.reset();
#if ERMC_NATIVE_WINDOWS
        if (mc_consumer_active()) AllowSetForegroundWindow(h->mcPid);
#endif
        h->mcSwitchReq = h->mcSwitchReq + 1;
        log("switch: F8 in Elden Ring -> back to Minecraft");
    }
    g_f8Down = down;

    uint32_t req = shm_header()->hostFocusReq;
    if (!g_focusSeenInit) {
        g_focusSeen = req;
        g_focusSeenInit = true;
    }
    if (req != g_focusSeen) {
        g_focusSeen = req;
        // Java may complete the already-consumed counter after a native F8.
        // That old transaction cannot revoke the explicit newer generation.
        bool consumedOlder = g_haveTerminalHandoff && req == g_terminalFocusReq &&
            shm_header()->mcSwitchReq != g_terminalResumeReq &&
            g_handoffPid == shm_header()->mcPid && g_handoffStart == shm_header()->mcStartMs &&
            g_terminalHostStart == shm_header()->hostStartMs;
        ErmcHandoff terminal = {};
        const bool trusted = handoff_snapshot(&terminal) && terminal.seq &&
            terminal.version == ERMC_HANDOFF_VERSION && terminal.focusReq == req &&
            terminal.mcPid == shm_header()->mcPid && terminal.mcStartMs == shm_header()->mcStartMs &&
            terminal.hostStartMs == shm_header()->hostStartMs && terminal.hostLife == shm_header()->hostLife &&
            !(terminal.flags & ~ERMC_HANDOFF_AUTOMATIC) && mc_consumer_active();
        const uint32_t newerResume = shm_header()->mcSwitchReq - terminal.resumeReq;
        // Also validate an as-yet-unconsumed transaction before revocation.
        // A future generation or an unreadable/untrusted event stays fail closed.
        if (consumedOlder || (trusted && newerResume && newerResume < 0x80000000u)) {
            log("handoff: late focus completion ignored after explicit native F8");
            return;
        }
        g_hostFocusChanged = true;
        // A queued/deferred MC death may arrive during the former 2-second
        // stand-in grace. F8 explicitly revokes that authority immediately.
        g_lastStandMs = 0;
        InterlockedExchange(&g_controlRevoked, 1);
        InterlockedExchange(&g_bodyControlAccepted, 0);
        return_to_host("Minecraft handed control to Elden Ring");
    }
}

// ---------------------------------------------------------------------------------------
// Per-frame tasks

static DWORD g_tickThread = 0, g_camThread = 0;

struct GameTickPerf {
    uint64_t frames = 0, idle = 0, rays = 0, entityScans = 0, candidates = 0, reportAt = 0;
    double playerMs = 0, rayMs = 0, entityMs = 0, restMs = 0, publishMs = 0, maxMs = 0;
    uint64_t memoryQueries = 0, memoryHits = 0, memoryProbes = 0, memoryEvictions = 0, memoryFaults = 0;
    double memoryQueryMs = 0, memoryValidationMs = 0, memoryMaxQueryMs = 0;
    uint64_t residentQueries = 0, residentFallbacks = 0;
    double residentQueryMs = 0, residentMaxQueryMs = 0;
};
static GameTickPerf g_gamePerf;

static void report_game_perf(bool enabled, bool consumer, uint32_t rays, uint32_t candidates,
    const LARGE_INTEGER& begin, const LARGE_INTEGER& player, const LARGE_INTEGER& ray,
    const LARGE_INTEGER& entities, const LARGE_INTEGER& rest, const LARGE_INTEGER& end,
    const LARGE_INTEGER& frequency, bool scanned) {
    if (!enabled) { g_gamePerf = {}; return; }
    uint64_t now = GetTickCount64();
    if (!g_gamePerf.reportAt) g_gamePerf.reportAt = now;
    double scale = 1000.0 / frequency.QuadPart;
    ++g_gamePerf.frames;
    if (!consumer) ++g_gamePerf.idle;
    g_gamePerf.rays += rays; g_gamePerf.candidates += candidates;
    if (scanned) ++g_gamePerf.entityScans;
    g_gamePerf.playerMs += (player.QuadPart - begin.QuadPart) * scale;
    g_gamePerf.rayMs += (ray.QuadPart - player.QuadPart) * scale;
    g_gamePerf.entityMs += (entities.QuadPart - ray.QuadPart) * scale;
    g_gamePerf.restMs += (rest.QuadPart - entities.QuadPart) * scale;
    g_gamePerf.publishMs += (end.QuadPart - rest.QuadPart) * scale;
    double elapsed = (end.QuadPart - begin.QuadPart) * scale;
    if (elapsed > g_gamePerf.maxMs) g_gamePerf.maxMs = elapsed;
    auto memory = mem_readability_stats();
    g_gamePerf.memoryQueries += memory.queries;
    g_gamePerf.memoryHits += memory.cacheHits;
    g_gamePerf.memoryProbes += memory.cacheProbes;
    g_gamePerf.memoryEvictions += memory.evictions;
    g_gamePerf.memoryFaults += memory.copyFaults;
    g_gamePerf.memoryQueryMs += memory.queryTicks * scale;
    g_gamePerf.residentQueries += memory.residentQueries;
    g_gamePerf.residentFallbacks += memory.residentFallbacks;
    g_gamePerf.residentQueryMs += memory.residentQueryTicks * scale;
    double residentMaxMs = memory.residentQueryMaxTicks * scale;
    if (residentMaxMs > g_gamePerf.residentMaxQueryMs) g_gamePerf.residentMaxQueryMs = residentMaxMs;
    g_gamePerf.memoryValidationMs += memory.validationTicks * scale;
    double queryMaxMs = memory.queryMaxTicks * scale;
    if (queryMaxMs > g_gamePerf.memoryMaxQueryMs) g_gamePerf.memoryMaxQueryMs = queryMaxMs;
    if (now - g_gamePerf.reportAt < 5000) return;
    double n = (double)g_gamePerf.frames;
    log("game-perf: %llu ms, ticks %llu (MC-idle %llu), avg player/control %.3f ms, rays %.3f ms (%llu casts), damage/entities %.3f ms (%llu scans, %llu candidates), other services %.3f ms, state publication %.3f ms, max tick %.3f ms",
        (unsigned long long)(now - g_gamePerf.reportAt), (unsigned long long)g_gamePerf.frames,
        (unsigned long long)g_gamePerf.idle, g_gamePerf.playerMs / n, g_gamePerf.rayMs / n,
        (unsigned long long)g_gamePerf.rays, g_gamePerf.entityMs / n,
        (unsigned long long)g_gamePerf.entityScans, (unsigned long long)g_gamePerf.candidates,
        g_gamePerf.restMs / n, g_gamePerf.publishMs / n, g_gamePerf.maxMs);
    log("memory-perf: queries %llu, cache hits %llu (probes %llu), evictions %llu, copy faults %llu; avg VirtualQuery %.3f ms/tick, all validation %.3f ms/tick, worst query %.3f ms",
        (unsigned long long)g_gamePerf.memoryQueries, (unsigned long long)g_gamePerf.memoryHits,
        (unsigned long long)g_gamePerf.memoryProbes, (unsigned long long)g_gamePerf.memoryEvictions,
        (unsigned long long)g_gamePerf.memoryFaults, g_gamePerf.memoryQueryMs / n,
        g_gamePerf.memoryValidationMs / n, g_gamePerf.memoryMaxQueryMs);
    log("memory-resident-perf: queries %llu, fallbacks %llu; avg working-set query %.3f ms/tick, worst query %.3f ms",
        (unsigned long long)g_gamePerf.residentQueries, (unsigned long long)g_gamePerf.residentFallbacks,
        g_gamePerf.residentQueryMs / n, g_gamePerf.residentMaxQueryMs);
    g_gamePerf = {}; g_gamePerf.reportAt = now;
}

static bool handoff_snapshot(ErmcHandoff* out) {
    const auto* event = (const ErmcHandoff*)((const uint8_t*)shm_header() + ERMC_OFF_HANDOFF);
    for (int tries = 0; tries < 8; ++tries) {
        uint32_t seq = event->seq;
        compiler_barrier();
        if (seq & 1u) continue;
        memcpy(out, event, sizeof(*out));
        compiler_barrier();
        if (event->seq == seq) return true;
    }
    return false;
}

static uint64_t unix_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t ticks = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    constexpr uint64_t epoch = 116444736000000000ull;
    return ticks >= epoch ? (ticks - epoch) / 10000 : 0;
}

static ErmcExplicitReset* explicit_reset_mailbox() {
    return (ErmcExplicitReset*)((uint8_t*)shm_header() + ERMC_OFF_EXPLICIT_RESET);
}

static bool explicit_reset_snapshot(ErmcExplicitReset* out) {
    auto* mailbox = explicit_reset_mailbox();
    for (int tries = 0; tries < 8; ++tries) {
        uint32_t seq = mailbox->seq;
        compiler_barrier();
        if (seq & 1u) continue;
        memcpy(out, mailbox, sizeof(*out));
        compiler_barrier();
        if (mailbox->seq == seq) return true;
    }
    return false;
}

// Explicit GENERIC_KILL bypasses the stand-in lease only after its separate
// authenticated binding. Passive fire/fall still have no such authority.
static void consume_explicit_reset_before_life(const Player* p, bool consumer) {
    auto* h = shm_header();
    auto& ledger = g_explicitReset;
    if (ledger.pid != h->mcPid || ledger.mcStart != h->mcStartMs || ledger.hostStart != h->hostStartMs) {
        ledger = {};
        ledger.pid = h->mcPid; ledger.mcStart = h->mcStartMs; ledger.hostStart = h->hostStartMs;
    }
    ErmcExplicitReset request = {};
    if (!explicit_reset_snapshot(&request) || !request.seq || request.seq == ledger.stamp) return;
    auto* mailbox = explicit_reset_mailbox();
    const uint32_t previousStamp = ledger.stamp;
    if (request.seq > ledger.stamp) ledger.stamp = request.seq;
    uint32_t result = ERMC_EXPLICIT_RESET_REJECTED;
    uint64_t now = unix_ms();
    const bool fresh = request.issuedAtMs && now >= request.issuedAtMs && now - request.issuedAtMs <= 2000;
    const bool identity = h->mcPid && h->mcStartMs && h->hostStartMs &&
        request.mcStartMs == h->mcStartMs && request.hostStartMs == h->hostStartMs;
    const bool valid = identity && request.version == ERMC_EXPLICIT_RESET_VERSION &&
        !(request.seq & 1u) && request.seq <= 0x7FFFFFFEu && request.seq > previousStamp;
    // An ACK survives core reload. Never execute the same stamped command again.
    const bool alreadyAcked = uint32_t(request.ack) == request.seq &&
        (uint32_t(request.ack >> 32) == ERMC_EXPLICIT_RESET_CONSUMED ||
         uint32_t(request.ack >> 32) == ERMC_EXPLICIT_RESET_REJECTED);
    if (valid && (!alreadyAcked || uint32_t(request.ack >> 32) == ERMC_EXPLICIT_RESET_CONSUMED) &&
        request.reason == ERMC_EXPLICIT_RESET_INACTIVE && !request.event && !request.hostLife) {
        if (!request.connection) {
            ledger.retire_current();
            result = ERMC_EXPLICIT_RESET_CONSUMED;
        } else if (consumer && fresh && !ledger.exhausted && !ledger.is_retired(request.connection) &&
                   request.connection == ledger.connection) {
            result = ERMC_EXPLICIT_RESET_CONSUMED; // same binding cannot reset the nonce ledger
        } else if (consumer && fresh && !ledger.exhausted && !ledger.is_retired(request.connection) && ledger.retire_current()) {
            ledger.connection = request.connection;
            result = ERMC_EXPLICIT_RESET_CONSUMED;
        }
    } else if (valid && !alreadyAcked && request.reason == ERMC_EXPLICIT_RESET_GENERIC_KILL &&
               request.connection && request.connection == ledger.connection &&
               request.event && request.event < (1ull << 52) && request.event > ledger.event) {
        ledger.event = request.event; // rejection is terminal; no re-date/retarget of this nonce
        const bool alive = p && g_life == LIFE_ALIVE && g_frame.valid &&
            g_frame.zone == g_aliveZone && request.hostLife >= 0 &&
            uint32_t(request.hostLife) == h->hostLife && !is_dead(*p);
        // A newly committed clear/rebind/process/life change must win before the engine call.
        if (consumer && fresh && alive && mailbox->seq == request.seq && ledger.pid == h->mcPid &&
            ledger.mcStart == h->mcStartMs && ledger.hostStart == h->hostStartMs &&
            uint32_t(request.hostLife) == h->hostLife && mc_consumer_active()) {
            g_expectDeath = true;
            g_suppressMcDeaths = true;
            g_lastStandMs = 0;
            InterlockedExchange(&g_controlRevoked, 1);
            InterlockedExchange(&g_bodyControlAccepted, 0);
            kill_player(*p);
            result = ERMC_EXPLICIT_RESET_CONSUMED;
            log("life: authenticated explicit reset consumed (connection %llu, event %llu, life %u)",
                (unsigned long long)request.connection, (unsigned long long)request.event, h->hostLife);
        }
    }
    if (!alreadyAcked) {
        const uint64_t ack = (uint64_t(result) << 32) | request.seq;
        InterlockedExchange64((volatile LONG64*)&mailbox->ack, (LONG64)ack);
    }
}

// Terminal ownership events precede lifecycle/death handling. The independent
// mailbox is never rewritten by the continuous camera producer. An incomplete
// transaction cannot authorize a teleport, death or a new camera/body owner.
static bool consume_handoff_before_life(const Player* p, bool consumer, bool ctrl, const ErmcControl& c) {
    auto* h = shm_header();
    const uint64_t now = now_ms();
    g_suppressMcDeaths = false;
    if (!consumer || g_handoffPid != h->mcPid || g_handoffStart != h->mcStartMs) {
        g_handoffSeqSeen = 0; g_handoffReading = false;
        g_haveTerminalHandoff = false;
        g_handoffPid = h->mcPid; g_handoffStart = h->mcStartMs;
    }
    ErmcHandoff event = {};
    bool snapshot = consumer && handoff_snapshot(&event);
    if (consumer && !snapshot) {
        if (!g_handoffReading) { g_handoffReading = true; g_handoffReadAt = now; }
        g_suppressMcDeaths = true;
        g_lastStandMs = 0;
        InterlockedExchange(&g_controlRevoked, 1);
        InterlockedExchange(&g_bodyControlAccepted, 0);
        if (now >= g_handoffReadAt && now - g_handoffReadAt < kHandoffReadLimitMs) return true;
        if (g_handoffReadAt != UINT64_MAX) {
            log("handoff: incomplete transaction expired; release without guessed relocation");
            g_handoffReadAt = UINT64_MAX; // expired latch; no deadline renewal/log spam
        }
        g_hostFocusChanged = true;
        return false;
    }
    bool transaction = snapshot && event.seq && event.seq != g_handoffSeqSeen;
    if (transaction) g_handoffSeqSeen = event.seq;
    const bool valid = transaction && event.version == ERMC_HANDOFF_VERSION &&
        event.mcPid == h->mcPid && event.mcStartMs == h->mcStartMs &&
        event.hostStartMs == h->hostStartMs && event.hostLife == h->hostLife &&
        event.resumeReq == h->mcSwitchReq && !(event.flags & ~ERMC_HANDOFF_AUTOMATIC) &&
        (event.focusReq == h->hostFocusReq || event.focusReq == h->hostFocusReq + 1u);
    if (valid) {
        g_haveTerminalHandoff = true;
        g_terminalFocusReq = event.focusReq; g_terminalResumeReq = event.resumeReq;
        g_terminalHostStart = event.hostStartMs;
        g_suppressMcDeaths = true;
        g_lastStandMs = 0;
        // This body may genuinely have died already: do not relocate a corpse.
        if ((event.flags & ERMC_HANDOFF_AUTOMATIC) && p && g_life == LIFE_ALIVE && !is_dead(*p))
            recover_auto_handoff(*p, "committed automatic handoff");
        InterlockedExchange(&g_controlRevoked, 1);
        InterlockedExchange(&g_bodyControlAccepted, 0);
        g_autoRecoverySeen = true;
        g_hostFocusChanged = true;
    } else if (g_handoffReading) {
        // A torn event that becomes invalid is terminal too. It cannot resume
        // the former owner or retain its anchor indefinitely.
        g_suppressMcDeaths = true; g_lastStandMs = 0;
        g_hostFocusChanged = true;
    }
    g_handoffReading = false;
    // Compatibility with the existing reason producer, consumed BEFORE life.
    // Only when no dedicated event exists; its retained AUTO flag must never
    // revoke a later explicit F8 after the terminal transaction was consumed.
    if (!consumer || (ctrl && !(c.flags & ERMC_CTRL_AUTO_RECOVERY))) g_autoRecoverySeen = false;
    if (ctrl && !event.seq && (c.flags & ERMC_CTRL_AUTO_RECOVERY) && !g_autoRecoverySeen) {
        g_autoRecoverySeen = true; g_suppressMcDeaths = true;
        if (p && g_life == LIFE_ALIVE && !is_dead(*p)) recover_auto_handoff(*p, "client automatic handoff");
        InterlockedExchange(&g_controlRevoked, 1);
        g_lastStandMs = 0;
        if (!g_hostFocusChanged) return_to_host("client automatic handoff");
    }
    return false;
}

static void task_tick_body(void*) {
    if (!g_tickThread) {
        g_tickThread = GetCurrentThreadId();
        log("game: tick task runs on thread %lu", g_tickThread);
    }
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    bool diagnostics = (shm_header()->debugFlags & kNativePerfDebugFlag) != 0;
    bool consumer = mc_consumer_active();
    LARGE_INTEGER tPlayer = {}, tRays = {}, tEntities = {}, tRest = {};
    uint32_t rays = 0, candidates = 0;
    InterlockedExchange(&g_tickAlive, 1);
    handle_switching();

    Player p;
    bool foundPlayer = get_player(&p);
    bool havePlayer = foundPlayer && block_valid(p);
    if (havePlayer) update_frame(p, g_life == LIFE_ALIVE);
    else {
        g_frame.valid = false;  // a loading screen: the next frame starts a zone
        g_putHavokValid = false;
        g_haveHmR = false;
    }
    ErmcControl c;
    memset(&c, 0, sizeof(c));
    bool ctrl = control_active(&c);
    bool handoffWaiting = consume_handoff_before_life(havePlayer ? &p : nullptr, consumer, ctrl, c);
    consume_explicit_reset_before_life(havePlayer ? &p : nullptr, consumer);
    update_life(havePlayer ? &p : nullptr);
    bool alive = havePlayer && g_life == LIFE_ALIVE;
    // Terminal mailbox consumption has already revoked camera/body authority;
    // passive composition can continue through the ordinary renderer contract.
    if (InterlockedCompareExchange(&g_controlRevoked, 0, 0) &&
        !(ctrl && passive_composite_control(c))) ctrl = false;
    bool active = alive && ctrl && (c.flags & ERMC_CTRL_MOVE_HUNTER) && !(c.flags & ERMC_CTRL_PASSIVE_COMPOSITE) &&
                  !(g_coopExcludePlayerIns && g_hostFocusChanged);
    // Explicit co-op readiness hold; ordinary zero flags still release. The
    // same control/heartbeat leases apply, and a real host handoff wins.
    HWND holdWindow = game_hwnd();
    bool hold = g_coopExcludePlayerIns && alive && ctrl && consumer &&
        (c.flags & ERMC_CTRL_HOLD_HUNTER) && !(c.flags & (ERMC_CTRL_MOVE_HUNTER | ERMC_CTRL_PASSIVE_COMPOSITE)) &&
        !g_hostFocusChanged && holdWindow && GetForegroundWindow() != holdWindow;
    if (handoffWaiting && alive) hold = true; // stand_in still validates the exact accepted owner
    bool visibilityGrace = g_coopExcludePlayerIns && alive && consumer && !g_hostFocusChanged;
    if (visibilityGrace && !active) {
        // Readiness pauses publish zero flags too. A requested/actual return to
        // the host, or an explicit camera-only mode, must restore immediately.
        HWND hwnd = game_hwnd();
        visibilityGrace = hwnd && GetForegroundWindow() != hwnd && (!ctrl || c.flags == 0);
    }
    if (foundPlayer) stand_in(p, &c, active, visibilityGrace, hold);
    else {
        // No validated main-player instance: abandon ownership, never follow a
        // remembered pointer through streaming or apply it to a remote PlayerIns.
        g_visibility.reset();
        if (g_holding) log("stand-in: HOLD ended (no validated player)");
        g_holding = false;
        g_holdLease.reset();
        g_haveRecoveryAnchor = false;
        g_anchorProbeAt = 0;
        g_standPlayer = nullptr;
        g_standing = g_havePut = g_movementCorrected = false;
        InterlockedExchange(&g_bodyControlAccepted, 0);
    }
    HWND peerWindow = game_hwnd();
    const bool passiveComposite = ctrl && passive_composite_control(c);
    bool viewerIntent = alive && ctrl && consumer && composite_control_valid(c) &&
        (c.flags & ERMC_CTRL_COMPOSITE) &&
        (passiveComposite || (!g_hostFocusChanged && peerWindow && GetForegroundWindow() != peerWindow));
    bool viewerComposite = g_peerCompositeLease.update(now_ms(), viewerIntent,
        viewerIntent && compositor_world_active(passiveComposite));
    update_peer_visibility(foundPlayer ? &p : nullptr, consumer, viewerComposite);
    update_passages(havePlayer && alive && g_standing, havePlayer ? p.ins : nullptr);
    if (diagnostics) QueryPerformanceCounter(&tPlayer);
    if (alive) {
        rays = service_rays(p.ins, consumer);
        if (diagnostics) QueryPerformanceCounter(&tRays);
        service_damage(p);      // before republishing: ids refer to last tick's table
        if (consumer) candidates = publish_entities(p);
        else clear_entities();
        if (diagnostics) QueryPerformanceCounter(&tEntities);
        service_test_enemy(p);
        service_action(p);
        check_action_press();
    } else {
        clear_entities();
        if (diagnostics) {
            QueryPerformanceCounter(&tRays);
            tEntities = tRays;
        }
    }
    publish_prompt(alive && g_standing);
    if (diagnostics) QueryPerformanceCounter(&tRest);

    frame_claim_source();
    on_frame();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    perf_add_tick((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart);
    report_game_perf(diagnostics, consumer, rays, candidates, t0, tPlayer, tRays, tEntities, tRest, t1, fq,
        alive && consumer);
}

static void task_camera_body(void*) {
    if (!g_camThread) {
        g_camThread = GetCurrentThreadId();
        log("game: camera task runs on thread %lu", g_camThread);
    }
    // Only while the Tarnished is usable: death cameras and loading screens stay the game's.
    ErmcControl c;
    if (g_life == LIFE_ALIVE && control_active(&c)) apply_camera(c);
}

static void __fastcall task_tick(void*, const void*) {
    InflightGuard guard;
    MemReadabilityScope memory((shm_header()->debugFlags & kNativePerfDebugFlag) != 0);
    if (!guarded_game_call(task_tick_body, nullptr)) {
        g_frame.valid = false;
        g_putHavokValid = false;
        g_haveHmR = false;
        clear_entities();
        // No partially read object reaches the published state. The state reader
        // has its own guard, so the frame lock cannot remain held after a fault.
        frame_claim_source();
        on_frame();
        static uint64_t lastFault = 0;
        uint64_t now = now_ms();
        if (now - lastFault > 5000) {
            lastFault = now;
            log("game: skipped a tick after a streaming memory fault");
        }
    }
}

static void __fastcall task_camera(void*, const void*) {
    InflightGuard guard;
    MemReadabilityScope memory;
    guarded_game_call(task_camera_body, nullptr);
}

// The persistent page: two FD4 task objects, their vtables and forwarding stubs, and the
// function pointers the stubs jump to. Registered once per process; there is no unregister,
// so on core shutdown the pointers are cleared and the tasks do nothing.

static uint64_t page_magic() { return 0x4B53415442524545ull ^ GetCurrentProcessId(); }

typedef bool(__fastcall* RegisterTask_t)(void* taskImp, uint32_t group, void* task);

static TaskPage* find_or_make_page() {
    ErmcHeader* h = shm_header();
    TaskPage* pg = (TaskPage*)(uintptr_t)h->hostTaskPage;
    if (pg && mem_readable(pg, sizeof(TaskPage)) && pg->magic == page_magic()) return pg;
    pg = (TaskPage*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pg) return nullptr;
    memset(pg, 0, sizeof(*pg));
    const uint8_t ret0[4] = {0x31, 0xC0, 0xC3, 0xCC};
    const uint8_t ret[4] = {0xC3, 0xCC, 0xCC, 0xCC};
    memcpy(pg->stubRet0, ret0, 4);
    memcpy(pg->stubRet, ret, 4);
    for (int i = 0; i < 2; i++) {
        uint8_t* s = pg->stubExec[i];
        int32_t disp = (int32_t)((uint8_t*)&pg->target[i] - (s + 7));
        const uint8_t code[16] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x74, 0x02, 0xFF, 0xE0, 0xC3, 0xCC};
        memcpy(s, code, 16);
        memcpy(s + 3, &disp, 4);
        pg->vtbl[i][0] = pg->stubRet0;
        pg->vtbl[i][1] = pg->stubRet;
        pg->vtbl[i][2] = s;
        pg->task[i].vtbl = pg->vtbl[i];
    }
    FlushInstructionCache(GetCurrentProcess(), pg, sizeof(*pg));
    uint8_t* imp = global_ptr(rva::kCSTaskImp, rva::kVtTaskImp);
    if (!imp) {
        log("game: CSTaskImp not ready; tasks not registered");
        VirtualFree(pg, 0, MEM_RELEASE);
        return nullptr;
    }
    RegisterTask_t reg = (RegisterTask_t)(g_base + rva::kRegisterTask);
    bool a = reg(imp, kGroupWorldChrManPostPhysics, &pg->task[0]);
    bool b = reg(imp, kGroupDrawParamUpdate, &pg->task[1]);
    log("game: registered tasks: tick (group %u) %s, camera (group %u) %s", kGroupWorldChrManPostPhysics,
        a ? "ok" : "FAILED", kGroupDrawParamUpdate, b ? "ok" : "FAILED");
    pg->magic = page_magic();
    h->hostTaskPage = (uint64_t)(uintptr_t)pg;
    return pg;
}

// ---------------------------------------------------------------------------------------

bool game_init() {
    // Read once before attaching tasks; the ER process must inherit this opt-in.
    char coop[2] = {};
    g_coopExcludePlayerIns = GetEnvironmentVariableA("ERBRIDGE_COOP", coop, sizeof(coop)) == 1 && coop[0] == '1';
    if (g_coopExcludePlayerIns)
        log("coop: excluding all PlayerIns from entity proxies and direct queued damage (including PlayerIns NPCs)");
    if (g_coopExcludePlayerIns)
        log("coop: canonical raw-zone coordinate frames (stored solo stitching ignored)");
    g_base = main_module_base();
    g_peerImageSize = main_module_size();
    log("game: eldenring.exe base %p size 0x%zx", (void*)g_base, main_module_size());
    g_ok = true;
    for (const CodeSig& s : kSigs) {
        if (!bytes_match(g_base + s.rva, s.sig)) {
            log("game: signature mismatch for %s at %p; unsupported Elden Ring build", s.name, (void*)(g_base + s.rva));
            g_ok = false;
        }
    }
    if (!g_ok) return false;
    g_combatOk = true;
    for (const CodeSig& a : kCombatSigs) {
        if (!bytes_match(g_base + a.rva, a.sig)) {
            log("game: %s differs; basic combat only", a.name);
            g_combatOk = false;
        }
    }
    if (g_combatOk) patch_poke_row();
    bool actionsOk = true;
    for (const CodeSig& a : kActionSigs) {
        if (!bytes_match(g_base + a.rva, a.sig)) {
            if (matches_coop_action_detour(a.rva)) {
                log("game: %s verified cooperative detour (original instructions and branch targets preserved)", a.name);
                continue;
            }
            log("game: %s differs; Elden Ring actions disabled", a.name);
            actionsOk = false;
        }
    }
    g_actionOk = actionsOk;
    g_page = find_or_make_page();
    if (!g_page) return false;
    g_page->target[0] = (void*)&task_tick;
    g_page->target[1] = (void*)&task_camera;
    log("game: per-frame tasks attached to this core");
    return true;
}

void game_detach() {
    if (g_page) {
        g_page->target[0] = nullptr;
        g_page->target[1] = nullptr;
    }
}

void game_shutdown() {
    restore_poke_row();
    // Hand the character back (NoMove, gravity, NoDead, rendering).
    Player p;
    bool havePlayer = get_player(&p);
    if ((g_standing || g_visibility.hidden) && havePlayer) release_player(p.ins, p.data, p.phys);
    update_peer_visibility(havePlayer ? &p : nullptr, false, false, true);
    g_peerCompositeLease.reset();
    g_holdLease.reset();
    InterlockedExchange(&g_controlRevoked, 0);
    InterlockedExchange(&g_bodyControlAccepted, 0);
    g_autoRecoverySeen = false;
    g_handoffSeqSeen = g_handoffPid = 0; g_handoffStart = 0;
    g_handoffReading = g_suppressMcDeaths = false;
    g_haveTerminalHandoff = false;
    g_explicitReset = {};
    g_visibility.reset();
    InterlockedExchange(&g_tickAlive, 0);
}

static void game_fill_state_body(void* context) {
    auto* st = (ErmcGameState*)context;
    st->unitsPerMeter = 1.0f;  // Elden Ring works in metres
    if (!g_ok) return;
    // Dead, loading or settling within a session: Minecraft keeps its overlay but draws
    // nothing, so Elden Ring's own death and loading screens show. (A minute after the last
    // living or dying character, e.g. back at the title screen, Minecraft gets its window back.)
    uint64_t now = now_ms();
    if (g_life == LIFE_DEAD) st->flags |= ERMC_STATE_PLAYER_DEAD | ERMC_STATE_HOST_BUSY;
    else if (g_life != LIFE_ALIVE && g_lastAliveMs && now - g_lastAliveMs < 60000) st->flags |= ERMC_STATE_HOST_BUSY;
    if (!g_frame.valid || g_life != LIFE_ALIVE) return;
    st->stageId = g_frame.zone;
    uint8_t* cam = final_camera();
    if (cam) {
        const float* m = (const float*)(cam + kCamMatrix);
        float pos[3] = {m[12], m[13], m[14]};
        float tgt[3] = {m[12] + m[8], m[13] + m[9], m[14] + m[10]};
        to_stable(pos, st->camPos);
        to_stable(tgt, st->camTarget);
        st->camUp[0] = m[4], st->camUp[1] = m[5], st->camUp[2] = m[6];
        st->fovYDeg = *(float*)(cam + kCamFov) * 180.0f / 3.14159265f;
        st->aspect = *(float*)(cam + kCamAspect);
        st->nearZ = *(float*)(cam + kCamNear);
        st->farZ = *(float*)(cam + kCamFar);
        st->flags |= ERMC_STATE_CAMERA_VALID;
    }
    if (InterlockedExchange(&g_overrideApplied, 0)) st->flags |= ERMC_STATE_CAM_OVERRIDDEN;
    if (compositor_active()) st->flags |= ERMC_STATE_COMPOSITING;
    Player p;
    if (get_player(&p)) {
        to_stable((const float*)(p.phys + kPhysPos), st->playerPos);
        // Where Steve is, not where the reach step put the Tarnished (anchors and recalls).
        if (g_standing)
            for (int i = 0; i < 3; i++) st->playerPos[i] -= g_reachOffset[i];
        memcpy(st->playerQuat, p.phys + kPhysQuat, 16);
        st->flags |= ERMC_STATE_PLAYER_VALID;
        if (g_standing && g_movementCorrected) st->flags |= ERMC_STATE_MOVEMENT_CORRECTED;
    }
}

void game_fill_state(ErmcGameState* st) {
    MemReadabilityScope memory;
    if (!guarded_game_call(game_fill_state_body, st)) {
        st->flags &= ~(ERMC_STATE_CAMERA_VALID | ERMC_STATE_PLAYER_VALID |
                       ERMC_STATE_CAM_OVERRIDDEN | ERMC_STATE_COMPOSITING);
    }
}

}  // namespace mb
