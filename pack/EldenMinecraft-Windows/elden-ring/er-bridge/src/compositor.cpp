// Draws Minecraft's frames into Elden Ring's own frame at Present (D3D12 on CrossOver's
// D3DMetal), so both games share one image made with one camera pose: no swimming between
// two windows. Minecraft sends each frame (world color + depth, a hand layer and a HUD layer)
// through frames.shm before it hands Elden Ring the frame's camera pose.
//
// Native Windows: MinHook intercepts COM method addresses learned from a disposable
// D3D12 device/swapchain. DXGI creation hooks capture the actual presenting queue; a late
// injection can learn it from command lists transitioning this swapchain's backbuffers.
// Native host-depth uses a COM provenance ledger and fenced scene snapshots. A scene
// adapter must certify the actual camera pass/projection; unsupported evidence falls back.
//
// Hooking on CrossOver: D3DMetal gives IDXGISwapChain a
// writable PE-side vtable shared by every swapchain, found by scanning dxgi.dll's .data
// (find_swapchain_table). Its Present, ResizeBuffers, Present1 and ResizeBuffers1 slots point
// at tiny stubs in a persistent RWX page. A stub jumps to this core's hook, or straight to the
// original while no core is loaded, so hot reloads never leave the game with a dangling pointer.
//
// Drawing: Minecraft's world is hidden behind Elden Ring's depth (occlusion) and relit from a
// blurred copy of Elden Ring's frame, with distance haze. The hand is relit too but never
// occluded; the HUD is drawn as is.
#include "common.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <string.h>

// MSVC builds use native D3D12. The existing MinGW/CrossOver build retains D3DMetal;
// either build may override this explicitly with ERMC_NATIVE_WINDOWS=0 or =1.
#ifndef ERMC_NATIVE_WINDOWS
#if defined(_MSC_VER)
#define ERMC_NATIVE_WINDOWS 1
#else
#define ERMC_NATIVE_WINDOWS 0
#endif
#endif
#if ERMC_NATIVE_WINDOWS
#include "MinHook.h"
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include "compositor_depth_contract.h"
#include "compositor_depth_projection.h"
#endif
#include "consumer_liveness.h"
#include "composite_mode.h"

namespace mb {

#ifdef ERMC_COMPOSITOR_HARNESS
// Capture before the real Present: FLIP_DISCARD does not guarantee buffer contents after it.
void compositor_harness_capture(UINT backbuffer, float useDepth, float debugDepth);
void compositor_harness_after_pose_selection();
void compositor_harness_before_submission();
#endif

// ---------------------------------------------------------------------------------------
// Persistent hook page

enum { kSlotPresent = 0, kSlotResize, kSlotPresent1, kSlotResize1, kHookCount };
static const int kVtblIndex[kHookCount] = {8, 13, 22, 39};  // IDXGISwapChain4 table slots
static const char* const kHookNames[kHookCount] = {"Present", "ResizeBuffers", "Present1", "ResizeBuffers1"};

struct PresentPage {
    uint64_t magic;
    void** table;              // D3DMetal's shared IDXGISwapChain4 table that we patched
    void* orig[kHookCount];    // its original entries
    void* target[kHookCount];  // this core's hooks (null: the stubs go straight to the originals)
    void* unused;
    uint8_t stub[kHookCount][32];
};

static PresentPage* g_pp = nullptr;

#if ERMC_NATIVE_WINDOWS
enum { kSlotCreate = kHookCount, kSlotCreateHwnd, kSlotCreateCore, kSlotCreateComposition,
       kSlotExecute, kSlotBarrier, kSlotReset, kNativeRequiredHookCount,
       kSlotDepthView = kNativeRequiredHookCount, kSlotCopyDescriptors, kSlotCopyDescriptorsSimple,
       kSlotClose, kSlotDepthTargets, kSlotClearDepth, kSlotBundle, kSlotRenderPass, kSlotEnhancedBarrier,
       kNativeHookCount };
static void* g_nativeOrig[kNativeHookCount] = {};
static bool g_nativeHooked = false;
static volatile LONG g_nativeStopping = 0;
static SRWLOCK g_renderLock = SRWLOCK_INIT;
static thread_local unsigned g_nativeResizeDepth = 0;
static thread_local unsigned g_nativeCompositorCommands = 0;
struct NativeCompositorScope {
    NativeCompositorScope() { ++g_nativeCompositorCommands; }
    ~NativeCompositorScope() { --g_nativeCompositorCommands; }
};
static ID3D12CommandQueue* native_queue_for(IDXGISwapChain* sc, ID3D12Device* dev);
static void native_before_resize(IDXGISwapChain* sc);
static void native_after_resize(IDXGISwapChain* sc, HRESULT hr, UINT count, IUnknown* const* queues);
static bool native_init();
static void native_detach();
static void native_shutdown();
static void native_depth_audit_target(ID3D12Device* dev, ID3D12CommandQueue* queue);
static void native_depth_audit_request(bool requested);
static void native_depth_audit_report(ID3D12Device* dev, UINT width, UINT height, bool requested);
static void native_cancel_probe();
static void native_poll_diagnostics();
static void native_note_hook(int slot);
static void native_begin_composite_profile(LARGE_INTEGER* start);
static void native_end_composite_profile(const LARGE_INTEGER& start);
static std::atomic<bool> g_nativeSceneEnabled{false};
static void depth_clear_native_ledger();
static void depth_attach_execute();
static void depth_release_snapshots();
#endif

static void* original_hook(int slot) {
#if ERMC_NATIVE_WINDOWS
    return g_nativeOrig[slot];
#else
    return g_pp->orig[slot];
#endif
}

#if !ERMC_NATIVE_WINDOWS
static uint64_t page_magic() { return 0x5052455345545250ull ^ GetCurrentProcessId(); }

// mov rax,[rip+target]; test rax,rax; jz +2; jmp rax; jmp [rip+orig]
static void write_stub(PresentPage* pg, int k) {
    uint8_t* s = pg->stub[k];
    uint8_t code[20] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x74, 0x02, 0xFF, 0xE0, 0xFF, 0x25, 0, 0, 0, 0};
    int32_t d1 = (int32_t)((uint8_t*)&pg->target[k] - (s + 7));
    int32_t d2 = (int32_t)((uint8_t*)&pg->orig[k] - (s + 20));
    memcpy(code + 3, &d1, 4);
    memcpy(code + 16, &d2, 4);
    memcpy(s, code, sizeof(code));
}

// D3DMetal's objects live in Mach-O memory, which Wine's VirtualQuery reports as free, so
// mem_readable() rejects them. IsBadReadPtr really reads (with a fault handler) instead.
static bool readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }
#endif

template <typename T>
static void safe_release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// D3DMetal builds the hookable IDXGISwapChain4 table (41 entries) and its 16-byte stubs
// (`sub rcx,0x10; mov rax,<D3DMetal method>; jmp rax`) inside dxgi.dll's own .data scratch
// space. Find the table there. Never make a swapchain to learn it: the
// windows that needs, destroyed from our thread, deadlocked Wine's surface code against the
// game's main thread.
// Only the stub's tail is checked: other tools (the Steam overlay) overwrite the first 5
// bytes of some stubs (Release, Present, ResizeBuffers...) with a jmp to their own hooks.
#if !ERMC_NATIVE_WINDOWS
static bool is_dxgi_stub(uintptr_t a, uintptr_t lo, uintptr_t hi) {
    if (a < lo || a + 16 > hi || (a & 0xF)) return false;
    const uint8_t* p = (const uint8_t*)a;
    return p[14] == 0xFF && p[15] == 0xE0;
}

static void** find_swapchain_table() {
    uint8_t* base = (uint8_t*)GetModuleHandleA("dxgi.dll");
    if (!base || !mem_readable(base, 0x1000)) return nullptr;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    uintptr_t lo = (uintptr_t)base, hi = lo + nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    void** found = nullptr;
    int tables41 = 0, tables32 = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uintptr_t a = lo + sec[i].VirtualAddress, e = a + sec[i].Misc.VirtualSize;
        a = (a + 7) & ~(uintptr_t)7;
        // Page by page: only committed, readable pages are looked at.
        int run = 0;
        uintptr_t runStart = 0;
        for (uintptr_t page = a & ~(uintptr_t)0xFFF; page < e; page += 0x1000) {
            if (!mem_readable((void*)page, 0x1000)) {
                run = 0;
                continue;
            }
            uintptr_t q = page < a ? a : page, qe = page + 0x1000 < e ? page + 0x1000 : e;
            for (; q + 8 <= qe; q += 8) {
                uintptr_t v = *(const uintptr_t*)q;
                if (is_dxgi_stub(v, lo, hi)) {
                    if (run++ == 0) runStart = q;
                    continue;
                }
                if (run == 41) {
                    tables41++;
                    if (!found) found = (void**)runStart;
                } else if (run == 32) {
                    tables32++;
                }
                run = 0;
            }
        }
    }
    log("compositor: dxgi.dll scratch space holds %d swapchain table(s) and %d factory table(s)", tables41, tables32);
    return tables41 == 1 ? found : nullptr;
}
#endif

// ---------------------------------------------------------------------------------------
// frames.shm

static uint8_t* g_frames = nullptr;
#if !ERMC_NATIVE_WINDOWS || defined(ERMC_COMPOSITOR_HARNESS)
static uint64_t g_lastMapAttempt = 0;
#endif
static void forget_uploaded_frame();
static void reset_notice_leases();
#if ERMC_NATIVE_WINDOWS
static std::atomic<bool> g_framesRequested{false}, g_framesMapped{false};
#endif

static bool frames_validate_layout() {
    if (!g_frames) return false;
    const volatile auto* header = (const volatile ErmcFramesHeader*)g_frames;
    if (header->magic == ERMC_FRAMES_MAGIC && header->version == ERMC_FRAMES_VERSION) return true;
    static bool logged = false;
    if (!logged) {
        logged = true;
        log("compositor: frames.shm layout version %u, expected %u (Minecraft and er-bridge from different builds?)",
            header->version, ERMC_FRAMES_VERSION);
    }
    // A replacement mapping can reuse the same address, sequence and frame ID.
    // Neither its old textures nor its upload stamp belong to that new layout.
    forget_uploaded_frame();
    reset_notice_leases();
    UnmapViewOfFile(g_frames);
    g_frames = nullptr;
#if ERMC_NATIVE_WINDOWS
    g_framesMapped.store(false, std::memory_order_release);
#endif
    return false;
}

// File/driver calls can block on disk or antivirus. Native Present only requests
// this work; the worker prepares a private view outside g_renderLock.
static uint8_t* frames_map() {
    if (!ipc_ensure_dir()) return nullptr;
    std::wstring path = ipc_path(L"frames.shm");
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;  // Minecraft creates it
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size) || (uint64_t)size.QuadPart < ERMC_FRAMES_FILE_SIZE) {
        CloseHandle(f);
        return nullptr;
    }
    HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, 0, ERMC_FRAMES_FILE_SIZE, nullptr);
    CloseHandle(f);
    if (!m) return nullptr;
    auto* view = (uint8_t*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, ERMC_FRAMES_FILE_SIZE);
    CloseHandle(m);
    return view;
}

#if !ERMC_NATIVE_WINDOWS || defined(ERMC_COMPOSITOR_HARNESS)
static bool frames_open() {
    // Used by Wine and standalone fixtures, never native Present.
    if (g_frames) return frames_validate_layout();
    uint64_t now = now_ms();
    if (now - g_lastMapAttempt < 2000) return false;
    g_lastMapAttempt = now;
    g_frames = frames_map();
    if (!frames_validate_layout()) return false;
#if ERMC_NATIVE_WINDOWS
    g_framesMapped.store(g_frames != nullptr, std::memory_order_release);
#endif
    if (g_frames) log("compositor: mapped frames.shm");
    return g_frames != nullptr;
}
#endif

#if ERMC_NATIVE_WINDOWS
static bool frames_ready() {
    if (g_frames && frames_validate_layout()) return true;
    g_framesRequested.store(true, std::memory_order_release);
    return false;
}
static void frames_poll() {
    if (InterlockedCompareExchange(&g_nativeStopping, 0, 0) ||
        !g_framesRequested.load(std::memory_order_acquire) || g_framesMapped.load(std::memory_order_acquire)) return;
    static uint64_t lastAttempt = 0;
    uint64_t now = now_ms();
    if (now - lastAttempt < 2000) return;
    lastAttempt = now;
    auto* candidate = frames_map();
    if (!candidate) return;
    // Only adoption touches rendering state; never hold renderLock across file I/O.
    if (TryAcquireSRWLockExclusive(&g_renderLock)) {
        if (!g_frames && !InterlockedCompareExchange(&g_nativeStopping, 0, 0)) {
            forget_uploaded_frame();
            reset_notice_leases();
            g_frames = candidate; candidate = nullptr;
            if (frames_validate_layout()) {
                g_framesMapped.store(true, std::memory_order_release);
                log("compositor: worker mapped frames.shm");
            }
        }
        ReleaseSRWLockExclusive(&g_renderLock);
    }
    if (candidate) UnmapViewOfFile(candidate);
}
#endif

static ErmcFrameHeader* slot_header(uint32_t i) {
    return (ErmcFrameHeader*)(g_frames + 0x1000 + (size_t)i * ERMC_FRAME_SLOT_SIZE);
}

struct FrameUploadStamp {
    const ErmcFrameHeader* slot = nullptr;
    uint32_t seq = 0;
    uint64_t frameId = 0;
    uint64_t poseId = 0;
    float fov = 0, nearZ = 0.05f, farZ = 1000.0f;
    uint32_t width = 0, height = 0, flags = 0;
    float aspect = 0;
    uint32_t noticeTtlMs = 0;
};
// Opt-in extension in the reserved frame header; no transport stride/version change.
static constexpr uint32_t kFrameNotice = ERMC_FRAME_NOTICE;
static constexpr size_t kNoticeTtlOffset = 0x30;
static constexpr uint32_t kNoticeMaxTtlMs = 8000;
static_assert(offsetof(ErmcFrameHeader, aspect) + sizeof(float) == kNoticeTtlOffset, "notice metadata offset");
static_assert(offsetof(ErmcFrameHeader, noticeTtlMs) == kNoticeTtlOffset, "notice header field offset");
static_assert(kNoticeTtlOffset + sizeof(uint32_t) <= ERMC_FRAME_HDR, "notice fits reserved header");
// One bounded seqlock attempt: never spin behind the producer, and never size a
// texture from an unvalidated header. Caller owns the mapping under renderLock.
static bool capture_frame_stamp(const ErmcFrameHeader* h, FrameUploadStamp* out) {
    if (!h) return false;
    const uint32_t seq = h->seq;
    if (seq & 1) return false;
    compiler_barrier();
    FrameUploadStamp stamp = {h, seq, h->frameId, h->poseId, h->fovYDeg, h->mcNear, h->mcFar,
        h->width, h->height, h->flags, h->aspect,
        (h->flags & kFrameNotice) ? h->noticeTtlMs : 0};
    compiler_barrier();
    if (h->seq != seq || !stamp.width || !stamp.height ||
        stamp.width > ERMC_FRAME_MAX_W || stamp.height > ERMC_FRAME_MAX_H) return false;
    *out = stamp;
    return true;
}
static bool same_frame_upload(const FrameUploadStamp& a, const FrameUploadStamp& b) {
    return a.slot == b.slot && a.seq == b.seq && a.frameId == b.frameId && a.poseId == b.poseId &&
        a.width == b.width && a.height == b.height && a.flags == b.flags && a.fov == b.fov &&
        a.nearZ == b.nearZ && a.farZ == b.farZ && a.aspect == b.aspect && a.noticeTtlMs == b.noticeTtlMs;
}

// Only two passive payloads are known: ordinary world/depth, or world/depth plus
// freshly cleared notice GUI. A GUI/NOTICE plane by itself is never world proof.
static bool passive_frame_valid(const FrameUploadStamp& stamp, uint64_t pose) {
    return pose && stamp.poseId == pose &&
        (stamp.flags == 1u || (stamp.flags == (1u | 2u | kFrameNotice) &&
            stamp.noticeTtlMs >= 1 && stamp.noticeTtlMs <= kNoticeMaxTtlMs));
}
struct NoticeLease {
    FrameUploadStamp stamp;
    uint64_t firstSeenMs = 0, deadlineMs = 0;
};
// One record per transport slot. Keep consumed generations even when the textures
// are invalidated, the mode changes, or GPU backpressure prevents submission.
static NoticeLease g_noticeLeases[ERMC_FRAME_SLOTS];
static uint32_t g_noticeProducerPid = 0;
static uint64_t g_noticeProducerStart = 0;
static void reset_notice_leases() { for (auto& lease : g_noticeLeases) lease = {}; }
static bool observe_notice(const FrameUploadStamp& stamp, uint64_t now) {
    if (!(stamp.flags & kFrameNotice)) return true;
    NoticeLease* free = nullptr;
    for (auto& lease : g_noticeLeases) {
        if (!lease.stamp.slot) { if (!free) free = &lease; continue; }
        if (lease.stamp.slot != stamp.slot) continue;
        if (same_frame_upload(lease.stamp, stamp)) return true;
        // Seqlock generations must advance. Same-seq metadata changes and replay
        // cannot buy another deadline; signed subtraction permits uint32 wrap.
        if (int32_t(stamp.seq - lease.stamp.seq) <= 0) return false;
        free = &lease;
        break;
    }
    if (!free) return false;
    *free = {stamp, now, now + stamp.noticeTtlMs};
    return true;
}
static bool notice_live(const FrameUploadStamp& stamp, uint64_t now) {
    if (stamp.flags != (1u | 2u | kFrameNotice)) return false;
    for (const auto& lease : g_noticeLeases)
        if (same_frame_upload(lease.stamp, stamp))
            return now >= lease.firstSeenMs && now < lease.deadlineMs;
    return false;
}

// ---------------------------------------------------------------------------------------
// Pose bookkeeping: which Minecraft pose Elden Ring rendered each game frame with. Present
// shows the image made with the pose applied `poseLag` game frames earlier.

static uint64_t g_poseHistory[8];
static volatile uint32_t g_poseHistoryPos = 0;

void compositor_note_applied_pose(uint64_t poseId) {
    g_poseHistory[g_poseHistoryPos & 7] = poseId;
    compiler_barrier();
    g_poseHistoryPos = g_poseHistoryPos + 1;
}

static uint64_t pose_for_present(uint32_t lag) {
    uint32_t pos = g_poseHistoryPos;
    if (pos == 0) return 0;
    if (lag >= pos) lag = pos - 1;
    if (lag > 6) lag = 6;
    return g_poseHistory[(pos - 1 - lag) & 7];
}

static ErmcFrameHeader* pick_slot(uint64_t poseId, FrameUploadStamp* selected = nullptr) {
    ErmcFrameHeader* best = nullptr;
    FrameUploadStamp bestStamp;
    for (uint32_t i = 0; i < ERMC_FRAME_SLOTS; i++) {
        ErmcFrameHeader* h = slot_header(i);
        FrameUploadStamp stamp;
        if (!capture_frame_stamp(h, &stamp)) continue;
        if (stamp.poseId == poseId) {
            if (selected) *selected = stamp;
            return h;
        }
        // Else the newest frame that is not newer than the pose Elden Ring rendered.
        if (stamp.poseId < poseId && (!best || stamp.poseId > bestStamp.poseId)) { best = h; bestStamp = stamp; }
    }
    if (selected) *selected = bestStamp;
    return best;
}

static uint32_t g_syncExact = 0, g_syncOlder = 0, g_syncMissing = 0;

static void note_sync(ErmcFrameHeader* slot, uint64_t poseId) {
    if (slot && slot->poseId == poseId) g_syncExact++;
    else if (slot) g_syncOlder++;
    else g_syncMissing++;
    if (g_syncExact + g_syncOlder + g_syncMissing >= 300) {
        log("sync: frames with the presented pose: exact %u, older %u, missing %u", g_syncExact, g_syncOlder,
            g_syncMissing);
        g_syncExact = g_syncOlder = g_syncMissing = 0;
    }
}

// ---------------------------------------------------------------------------------------
// D3D12 objects

static const char* kShader = R"(
Texture2D<float4> tWorld : register(t0);
Texture2D<float>  tMcDepth : register(t1);
Texture2D<float4> tGui : register(t2);
Texture2D<float>  tHostDepth : register(t3);
Texture2D<float4> tLight : register(t4);  // Elden Ring's frame blurred to ~1/64: local light (blur passes: their source)
Texture2D<float4> tHaze : register(t5);   // ~1/8: the scenery behind a block, for distance haze
Texture2D<float4> tHand : register(t6);   // Minecraft's hand and screen effects (lit, never occluded)
SamplerState sPoint : register(s0);
SamplerState sLinear : register(s1);
cbuffer Params : register(b0) {
    float mcNear; float mcFar; float hostNear; float hostFar;
    float hostReversed; float useDepth; float debugView; float bias;
    float relight; float lightGain; float lightMin; float fogStrength;
    float fogStart; float fogEnd; float tintAmount; float testPattern;
    float handLayer; float pad0; float pad1; float pad2;
    float suppressWorld; float suppressGui; float pad4; float pad5;
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float linMc(float d) {
    float z = d * 2 - 1;
    return 2 * mcNear * mcFar / (mcFar + mcNear - z * (mcFar - mcNear));
}
float linHost(float d) {
    if (pad2 > 0.5) {
        float den = d - pad0;
        return abs(den) < 1e-8 ? hostFar : min(hostFar, abs(pad1 / den));
    }
    return hostReversed > 0.5 ? hostFar * hostNear / (hostNear + d * (hostFar - hostNear))
                              : hostFar * hostNear / (hostFar - d * (hostFar - hostNear));
}
// 8x box downsample of tLight: 16 bilinear taps, each averaging 2x2 texels.
float4 PSDown(VSOut i) : SV_Target {
    float2 size;
    tLight.GetDimensions(size.x, size.y);
    float2 texel = 1.0 / size;
    float4 acc = 0;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++)
            acc += tLight.SampleLevel(sLinear, i.uv + (float2(x, y) - 1.5) * 2.0 * texel, 0);
    return acc / 16.0;
}
// Elden Ring's lighting, estimated from its own image: a heavily blurred copy of the frame is
// the local light around this pixel (dark chapels, torches, open sky), with a little of its colour.
float3 lightAt(float2 uv) {
    float3 env = tLight.SampleLevel(sLinear, uv, 0).rgb;
    float lum = dot(env, float3(0.299, 0.587, 0.114));
    float3 tint = saturate(env / max(lum, 1e-3) * 0.5);
    float k = clamp(lightMin + lum * lightGain, 0.0, 1.15);
    return k * lerp(float3(1, 1, 1), tint * 2.0, tintAmount);
}
float4 PS(VSOut i) : SV_Target {
    if (testPattern > 0.5) {
        // Translucent checkerboard in the top-left quarter: proves the hook draws.
        if (i.uv.x > 0.25 || i.uv.y > 0.25) return 0;
        float c = fmod(floor(i.uv.x * 64) + floor(i.uv.y * 64), 2);
        return float4(0.6 * c, 0.2, 0.6 * (1 - c), 1) * 0.6;
    }
    float2 uvMc = float2(i.uv.x, 1 - i.uv.y);   // Minecraft rows are bottom-up
    float hd = tHostDepth.SampleLevel(sPoint, i.uv, 0);
    if (debugView > 0.5) return float4(frac(linHost(hd) / 10).xxx, 1);   // 10 m depth bands
    float4 world = tWorld.SampleLevel(sPoint, uvMc, 0);
    if (suppressWorld > 0.5) world = 0;
    float md = tMcDepth.SampleLevel(sPoint, uvMc, 0);
    if (world.a > 0 && md < 1.0) {
        float mc = linMc(md);
        if (useDepth > 0.5 && mc > linHost(hd) + min(0.08, bias + mc * 0.004)) world = 0;
        if (relight > 0.5) world.rgb *= lightAt(i.uv);
        // Distance haze: fade toward the Elden Ring scenery behind the block.
        float f = saturate((mc - fogStart) / max(fogEnd - fogStart, 1.0)) * fogStrength;
        if (f > 0 && relight > 0.5) {
            float3 behind = tHaze.SampleLevel(sLinear, i.uv, 0).rgb;
            world.rgb = lerp(world.rgb, behind * world.a, f);
        }
    }
    // The hand is lit like the world but never hidden: nothing of Elden Ring is that close.
    float4 hand = handLayer > 0.5 ? tHand.SampleLevel(sPoint, uvMc, 0) : 0;
    if (hand.a > 0 && relight > 0.5) hand.rgb *= lightAt(i.uv);
    float4 scene = hand + world * (1 - hand.a);
    float4 gui = suppressGui > 0.5 ? 0 : tGui.SampleLevel(sPoint, uvMc, 0);
    return gui + scene * (1 - gui.a);
}
)";

struct Params {
    float mcNear, mcFar, hostNear, hostFar;
    float hostReversed, useDepth, debugView, bias;
    float relight, lightGain, lightMin, fogStrength;
    float fogStart, fogEnd, tintAmount, testPattern;
    float handLayer, pad0, pad1, pad2;
    float suppressWorld, suppressGui, pad4, pad5;
};

// Shader-visible descriptors: three tables of kTableSize (t0..t5). The composite table holds
// Minecraft's layers and the blurred copies of Elden Ring's frame; each blur pass has its
// own table whose t4 is that pass's source.
enum { kSrvWorld = 0, kSrvMcDepth, kSrvGui, kSrvHostDepth, kSrvLight, kSrvHaze, kSrvHand, kTableSize };
enum { kTableComposite = 0, kTableDown1, kTableDown2, kTableCount };
static const int kRing = 3;       // frames in flight
static const int kMaxBuffers = 8;
static const int kRtvDown1 = kMaxBuffers, kRtvDown2 = kMaxBuffers + 1, kRtvCount = kMaxBuffers + 2;

struct RingSlot {
    ID3D12CommandAllocator* alloc;
    ID3D12Resource* upload;
    uint8_t* uploadPtr;
    uint64_t uploadSize;
    uint64_t fence;
};

static IDXGISwapChain3* g_sc = nullptr;  // the game's swapchain (not AddRef'd: it outlives us)
static ID3D12Device* g_dev = nullptr;
static ID3D12CommandQueue* g_queue = nullptr;
static ID3D12RootSignature* g_rootSig = nullptr;
static ID3D12PipelineState* g_pso = nullptr;
static ID3D12PipelineState* g_psoDown = nullptr;   // blur passes
static ID3D12GraphicsCommandList* g_list = nullptr;
static ID3D12Fence* g_fence = nullptr;
static HANDLE g_fenceEvent = nullptr;
static uint64_t g_fenceNext = 0;
static RingSlot g_ring[kRing];
static uint32_t g_ringPos = 0;
static ID3D12DescriptorHeap* g_srvHeap = nullptr;
static ID3D12DescriptorHeap* g_srvCpuHeap = nullptr;
static ID3D12DescriptorHeap* g_rtvHeap = nullptr;
static UINT g_srvInc = 0, g_rtvInc = 0;
static ID3D12Resource* g_bb[kMaxBuffers] = {};
static UINT g_bbCount = 0, g_bbW = 0, g_bbH = 0;
static DXGI_FORMAT g_bbFormat = DXGI_FORMAT_UNKNOWN;
static ID3D12Resource* g_sceneCopy = nullptr;  // Elden Ring's finished frame, before we draw
static ID3D12Resource* g_down1 = nullptr;      // 1/8 of it
static ID3D12Resource* g_down2 = nullptr;      // 1/64 of it
static UINT g_down1W = 0, g_down1H = 0, g_down2W = 0, g_down2H = 0;
static const int kLayers = 4;
static ID3D12Resource* g_tex[kLayers] = {};  // world BGRA8, Minecraft depth R32F, HUD BGRA8, hand BGRA8
static UINT g_texW = 0, g_texH = 0;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp[kLayers];
static bool g_texHand = false;               // the hand texture belongs to the frame in the others
static uint64_t g_layerBytes = 0;
static bool g_failed = false;
static bool g_haveFrame = false;             // the textures hold a frame worth drawing
static FrameUploadStamp g_uploadedFrame;
#if ERMC_NATIVE_WINDOWS
static uint64_t g_uploadedPose = 0;
static uint64_t g_compositeTargetPose = 0; // diagnostic uses the same latched pose as this draw
static float g_uploadedFov = 0, g_uploadedNear = 0.05f, g_uploadedFar = 1000.0f;
#endif
static uint64_t g_lastCompositeMs = 0;
static std::atomic<uint64_t> g_worldCompositeStamp{0};
static bool g_lastCompositePassive = false;
static DWORD g_presentThread = 0;

static void forget_uploaded_frame() {
    g_worldCompositeStamp.store(0, std::memory_order_release);
    g_haveFrame = false;
    g_texHand = false;
    g_uploadedFrame = {};
#if ERMC_NATIVE_WINDOWS
    g_uploadedPose = 0;
#endif
}

bool compositor_active() { return now_ms() - g_lastCompositeMs < 500; }
bool compositor_world_active(bool passive) {
    uint64_t stamp = g_worldCompositeStamp.load(std::memory_order_acquire);
    uint64_t at = stamp >> 1, now = now_ms();
    return at && bool(stamp & 1) == passive && now >= at && now - at < 250;
}
static void commit_uploaded_frame(const FrameUploadStamp& stamp, bool hand) {
    g_uploadedFrame = stamp;
    g_haveFrame = true;
    g_texHand = hand;
#if ERMC_NATIVE_WINDOWS
    g_uploadedPose = stamp.poseId; g_uploadedFov = stamp.fov;
    g_uploadedNear = stamp.nearZ; g_uploadedFar = stamp.farZ;
#endif
}

typedef HRESULT(WINAPI* D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

static D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES h = {};
    h.Type = t;
    h.CreationNodeMask = 1;
    h.VisibleNodeMask = 1;
    return h;
}

static D3D12_RESOURCE_DESC tex_desc(UINT w, UINT h, DXGI_FORMAT f) {
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return d;
}

static void transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    g_list->ResourceBarrier(1, &b);
}

static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(int i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * g_srvInc;
    return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu(int i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * g_rtvInc;
    return h;
}

// r may be null: a null descriptor reads as 0 (the host depth slot stays null until the game's
// depth buffer is known; the shader only reads it when told to).
static void make_srv(ID3D12Resource* r, DXGI_FORMAT f, int slot, int table = kTableComposite) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = f;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    auto source = g_srvCpuHeap->GetCPUDescriptorHandleForHeapStart();
    source.ptr += SIZE_T(table * kTableSize + slot) * g_srvInc;
    g_dev->CreateShaderResourceView(r, &d, source);
    g_dev->CopyDescriptorsSimple(1, srv_cpu(table * kTableSize + slot), source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

static D3D12_GPU_DESCRIPTOR_HANDLE table_gpu(int table) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)table * kTableSize * g_srvInc;
    return h;
}

// Null descriptors for a whole table (every slot of a bound table must be valid).
static void null_table(int table) {
    const DXGI_FORMAT f[kTableSize] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM,
                                       DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                       DXGI_FORMAT_B8G8R8A8_UNORM};
    static_assert(kTableSize == 7, "one format per table slot");
    for (int i = 0; i < kTableSize; i++) make_srv(nullptr, f[i], i, table);
}

// Waits until the GPU finished with a ring slot. False if it takes suspiciously long (then
// the slot is not touched this frame).
static bool wait_fence(uint64_t value, DWORD ms) {
    if (!value || g_fence->GetCompletedValue() >= value) return true;
    if (FAILED(g_fence->SetEventOnCompletion(value, g_fenceEvent))) return false;
    WaitForSingleObject(g_fenceEvent, ms);
    return g_fence->GetCompletedValue() >= value;
}

static bool wait_idle() {
    if (!g_fence || !g_queue) return true;
    uint64_t v = ++g_fenceNext;
    if (SUCCEEDED(g_queue->Signal(g_fence, v)) && wait_fence(v, 2000)) return true;
    // A removed device can no longer use our allocations. Otherwise keep them alive.
    return g_dev && FAILED(g_dev->GetDeviceRemovedReason());
}

// Poll once. UINT64_MAX means device removal, never permission to reuse memory.
#if ERMC_NATIVE_WINDOWS
static bool frame_fence_ready(uint64_t value) {
    if (!g_fence) return value == 0;
    const uint64_t completed = g_fence->GetCompletedValue();
    if (completed == UINT64_MAX) { g_failed = true; return false; }
    return completed >= value;
}
static bool frame_resources_idle() {
    uint64_t last = 0;
    for (const auto& r : g_ring) if (r.fence > last) last = r.fence;
    return frame_fence_ready(last);
}
#endif

static bool init_pipeline() {
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompile_t compile = dc ? (D3DCompile_t)GetProcAddress(dc, "D3DCompile") : nullptr;
    auto serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(GetModuleHandleA("d3d12.dll"),
                                                                         "D3D12SerializeRootSignature");
    if (!compile || !serialize) {
        log("compositor: D3DCompile or D3D12SerializeRootSignature not available");
        return false;
    }
    ID3DBlob *vs = nullptr, *ps = nullptr, *psDown = nullptr, *err = nullptr;
    if (FAILED(compile(kShader, strlen(kShader), "erbridge", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(compile(kShader, strlen(kShader), "erbridge", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &err)) ||
        FAILED(compile(kShader, strlen(kShader), "erbridge", nullptr, nullptr, "PSDown", "ps_5_0", 0, 0, &psDown, &err))) {
        log("compositor: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        safe_release(err);
        safe_release(vs);
        safe_release(ps);
        safe_release(psDown);
        return false;
    }

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kTableSize;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = sizeof(Params) / 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    for (int i = 0; i < 2; i++) {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].MaxAnisotropy = 1;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 2;
    rsd.pStaticSamplers = samplers;
    ID3DBlob* rs = nullptr;
    bool ok = SUCCEEDED(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err)) &&
              SUCCEEDED(g_dev->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(),
                                                    __uuidof(ID3D12RootSignature), (void**)&g_rootSig));
    if (!ok) log("compositor: root signature failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
    safe_release(rs);
    safe_release(err);

    if (ok) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_rootSig;
        pd.VS.pShaderBytecode = vs->GetBufferPointer();
        pd.VS.BytecodeLength = vs->GetBufferSize();
        pd.PS.pShaderBytecode = ps->GetBufferPointer();
        pd.PS.BytecodeLength = ps->GetBufferSize();
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE;  // premultiplied "over"
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.LogicOp = D3D12_LOGIC_OP_NOOP;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        pd.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        pd.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        pd.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DepthStencilState.BackFace = pd.DepthStencilState.FrontFace;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = g_bbFormat;
        pd.SampleDesc.Count = 1;
        ok = SUCCEEDED(g_dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&g_pso));
        if (!ok) log("compositor: pipeline state failed (back buffer format %u)", g_bbFormat);
        if (ok) {
            // Blur passes: same root signature, plain writes into RGBA8 targets.
            pd.PS.pShaderBytecode = psDown->GetBufferPointer();
            pd.PS.BytecodeLength = psDown->GetBufferSize();
            b.BlendEnable = FALSE;
            pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            ok = SUCCEEDED(g_dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&g_psoDown));
            if (!ok) log("compositor: blur pipeline state failed");
        }
    }
    safe_release(vs);
    safe_release(ps);
    safe_release(psDown);
    return ok;
}

static bool init_device_objects() {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kTableSize * kTableCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_srvHeap))) return false;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_srvCpuHeap))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kRtvCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_rtvHeap))) return false;
    g_srvInc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_rtvInc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (int i = 0; i < kRing; i++) {
        if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                 (void**)&g_ring[i].alloc)))
            return false;
    }
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ring[0].alloc, nullptr,
                                        __uuidof(ID3D12GraphicsCommandList), (void**)&g_list)))
        return false;
    g_list->Close();
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence))) return false;
    g_fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) return false;
    // Every slot of a bound descriptor table must hold a valid descriptor: start with null ones.
    for (int t = 0; t < kTableCount; t++) null_table(t);
    return init_pipeline();
}

static void release_scene() {
    safe_release(g_sceneCopy);
    safe_release(g_down1);
    safe_release(g_down2);
    g_down1W = g_down1H = g_down2W = g_down2H = 0;
    if (g_srvHeap) {
        null_table(kTableDown1);
        null_table(kTableDown2);
        make_srv(nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, kSrvLight);
        make_srv(nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, kSrvHaze);
    }
}

static void release_depth();

static void release_backbuffers() {
    for (UINT i = 0; i < kMaxBuffers; i++) safe_release(g_bb[i]);
    g_bbCount = 0;
    release_scene();
    release_depth();
}

// The copy of Elden Ring's frame and its two blurred levels (relighting and haze). Optional:
// without them Minecraft is drawn with its own lighting.
static bool make_scene_resources(UINT w, UINT h) {
    D3D12_HEAP_PROPERTIES def = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC d = tex_desc(w, h, g_bbFormat);
    if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                              nullptr, __uuidof(ID3D12Resource), (void**)&g_sceneCopy)))
        return false;
    g_down1W = (w + 7) / 8, g_down1H = (h + 7) / 8;
    g_down2W = (g_down1W + 7) / 8, g_down2H = (g_down1H + 7) / 8;
    D3D12_RESOURCE_DESC d1 = tex_desc(g_down1W, g_down1H, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_RESOURCE_DESC d2 = tex_desc(g_down2W, g_down2H, DXGI_FORMAT_R8G8B8A8_UNORM);
    d1.Flags = d2.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                              nullptr, __uuidof(ID3D12Resource), (void**)&g_down1)) ||
        FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d2, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                              nullptr, __uuidof(ID3D12Resource), (void**)&g_down2)))
        return false;
    g_dev->CreateRenderTargetView(g_down1, nullptr, rtv_cpu(kRtvDown1));
    g_dev->CreateRenderTargetView(g_down2, nullptr, rtv_cpu(kRtvDown2));
    make_srv(g_sceneCopy, g_bbFormat, kSrvLight, kTableDown1);
    make_srv(g_down1, DXGI_FORMAT_R8G8B8A8_UNORM, kSrvLight, kTableDown2);
    make_srv(g_down2, DXGI_FORMAT_R8G8B8A8_UNORM, kSrvLight);
    make_srv(g_down1, DXGI_FORMAT_R8G8B8A8_UNORM, kSrvHaze);
    log("compositor: scene copy %ux%u, light %ux%u, haze %ux%u", w, h, g_down2W, g_down2H, g_down1W, g_down1H);
    return true;
}

static bool acquire_backbuffers() {
    DXGI_SWAP_CHAIN_DESC1 d;
    if (FAILED(g_sc->GetDesc1(&d))) return false;
    if (!d.BufferCount || d.BufferCount > kMaxBuffers || !d.Width || !d.Height || d.SampleDesc.Count != 1) return false;
    if (g_bbFormat != DXGI_FORMAT_UNKNOWN && d.Format != g_bbFormat) {
        log("compositor: back buffer format changed %u -> %u; compositing off", g_bbFormat, d.Format);
        return false;
    }
    for (UINT i = 0; i < d.BufferCount; i++) {
        if (FAILED(g_sc->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&g_bb[i]))) {
            release_backbuffers();
            return false;
        }
        g_dev->CreateRenderTargetView(g_bb[i], nullptr, rtv_cpu((int)i));
    }
    g_bbCount = d.BufferCount;
    g_bbW = d.Width;
    g_bbH = d.Height;
    if (!make_scene_resources(d.Width, d.Height)) {
        log("compositor: no scene copy (relighting off)");
        release_scene();
    }
    return true;
}

static void release_frame_resources() {
    for (auto& t : g_tex) safe_release(t);
    for (auto& r : g_ring) {
        if (r.upload) r.upload->Unmap(0, nullptr);
        safe_release(r.upload);
        r.uploadPtr = nullptr;
        r.uploadSize = 0;
    }
    g_texW = g_texH = 0;
    g_haveFrame = false;
    g_texHand = false;
    g_uploadedFrame = {};
}

// Textures and upload buffers for w x h Minecraft frames.
static bool ensure_frame_resources(UINT w, UINT h) {
    if (w == g_texW && h == g_texH && g_tex[0]) return true;
#if ERMC_NATIVE_WINDOWS
    // These allocations are used only by compositor submissions. Never insert
    // a whole-game queue Signal + 2s wait in Present on MC resolution changes.
    if (!frame_resources_idle()) return false;
#else
    if (!wait_idle()) return false;
#endif
    release_frame_resources();
    const DXGI_FORMAT fmt[kLayers] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      DXGI_FORMAT_B8G8R8A8_UNORM};
    const int srv[kLayers] = {kSrvWorld, kSrvMcDepth, kSrvGui, kSrvHand};
    D3D12_HEAP_PROPERTIES def = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    uint64_t offset = 0;
    for (int i = 0; i < kLayers; i++) {
        D3D12_RESOURCE_DESC d = tex_desc(w, h, fmt[i]);
        if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                  __uuidof(ID3D12Resource), (void**)&g_tex[i]))) {
            log("compositor: cannot create %ux%u frame textures", w, h);
            release_frame_resources();
            g_failed = true;
            return false;
        }
        make_srv(g_tex[i], fmt[i], srv[i]);
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        g_dev->GetCopyableFootprints(&d, 0, 1, offset, &g_fp[i], &rows, &rowBytes, &total);
        offset = (g_fp[i].Offset + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                 ~(uint64_t)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }
    D3D12_HEAP_PROPERTIES up = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = offset;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (auto& r : g_ring) {
        D3D12_RANGE none = {0, 0};
        if (FAILED(g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, __uuidof(ID3D12Resource), (void**)&r.upload)) ||
            FAILED(r.upload->Map(0, &none, (void**)&r.uploadPtr))) {
            log("compositor: cannot create upload buffers (%llu bytes)", (unsigned long long)offset);
            release_frame_resources();
            g_failed = true;
            return false;
        }
        r.uploadSize = offset;
    }
    g_texW = w;
    g_texH = h;
    g_layerBytes = (uint64_t)w * h * 4;
    log("compositor: frame textures %ux%u, upload %llu KB x %d", w, h, (unsigned long long)(offset >> 10), kRing);
    return true;
}

// Copies a new frames.shm publication into a ring slot's upload buffer. An unchanged
// publication is already in g_tex: keep drawing it without repeating the CPU/GPU upload.
// False also means an inconsistent slot; callers must validate identity before reuse.
static bool copy_slot(ErmcFrameHeader* h, RingSlot& r, bool* hand, FrameUploadStamp* captured,
    uint64_t expectedPassivePose = 0, const FrameUploadStamp* expected = nullptr) {
    FrameUploadStamp stamp;
    if (!capture_frame_stamp(h, &stamp) || (expected && !same_frame_upload(stamp, *expected))) return false;
    if (expectedPassivePose && !passive_frame_valid(stamp, expectedPassivePose)) return false;
    // Frame IDs restart with Minecraft; slot + seqlock generation distinguish a new
    // publication of the same ID. g_haveFrame invalidates reuse after producer loss.
    if (g_haveFrame && same_frame_upload(g_uploadedFrame, stamp)) return false;
    UINT w = stamp.width, ht = stamp.height;
    if (w != g_texW || ht != g_texH) return false;
    *hand = !expectedPassivePose && (stamp.flags & ERMC_FRAME_HAND) &&
        g_layerBytes * 4 <= (uint64_t)ERMC_FRAME_SLOT_SIZE - ERMC_FRAME_HDR;
    const uint8_t* base = (const uint8_t*)h + ERMC_FRAME_HDR;
    // Ordinary passive frames never consume old GUI/hand. The opt-in third plane
    // is a freshly rendered notice, with an already consumed, bounded deadline.
    const int passiveLayers = notice_live(stamp, now_ms()) ? 3 : 2;
    for (int i = 0; i < (expectedPassivePose ? passiveLayers : (*hand ? 4 : 3)); i++) {
        const uint8_t* src = base + g_layerBytes * i;
        uint8_t* dst = r.uploadPtr + g_fp[i].Offset;
        UINT pitch = g_fp[i].Footprint.RowPitch;
        if (pitch == w * 4) {
            memcpy(dst, src, g_layerBytes);
        } else {
            for (UINT y = 0; y < ht; y++) memcpy(dst + (size_t)y * pitch, src + (size_t)y * w * 4, w * 4);
        }
    }
    FrameUploadStamp after;
    if (!capture_frame_stamp(h, &after) || !same_frame_upload(stamp, after)) return false;
    *captured = stamp;
    return true;
}
static bool uploaded_frame_matches(const ErmcFrameHeader* h, uint64_t expectedPassivePose = 0) {
    FrameUploadStamp stamp;
    return capture_frame_stamp(h, &stamp) &&
        (!expectedPassivePose || passive_frame_valid(stamp, expectedPassivePose)) &&
        same_frame_upload(stamp, g_uploadedFrame);
}

// ---------------------------------------------------------------------------------------
// Elden Ring's scene depth, for occlusion. The engine wraps each depth-stencil view in a
// DLCG3::CGDepthStencilView (vtable RVA 0x30A7850): +0x08 is its DLCG3::CGTexture2D (vtable
// 0x30A7030), +0x18 the DSV format; the texture's +0x20 is D3DMetal's ID3D12Resource. The
// worker thread finds the views by scanning the heap for that vtable; the Present thread
// re-validates each chain, asks D3D12 for the resource's size and keeps the full-screen ones.

#if !ERMC_NATIVE_WINDOWS
constexpr uintptr_t kVtCGDepthStencilView = 0x30A7850;
constexpr uintptr_t kVtCGTexture2D = 0x30A7030;
static const int kMaxDepthScan = 32;
static const int kMaxDepthCands = 8;
struct DepthScanHit {
    uint8_t* dsv;
    uint8_t* tex;
    ID3D12Resource* res;
    uint32_t dsvFormat;
};
static DepthScanHit g_scanHits[kMaxDepthScan];
static int g_scanHitCount = 0;
static volatile LONG g_depthScanState = 0;  // 0 idle, 1 requested, 2 scanning (worker), 3 results ready
static ID3D12Resource* g_depthCands[kMaxDepthCands] = {};  // AddRef'd, full-screen, discovery order
static DXGI_FORMAT g_depthSrvFmt[kMaxDepthCands];
static int g_depthCandCount = 0;
static int g_depthBound = -1;           // candidate whose SRV is in the table
static uint64_t g_depthScanAtMs = 0;

static DXGI_FORMAT depth_srv_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// Worker thread (compositor_poll): the heap scan takes a few seconds.
static void scan_depth_views() {
    uintptr_t base = main_module_base();
    uint64_t pat = base + kVtCGDepthStencilView;
    uint8_t mask[8];
    memset(mask, 0xFF, sizeof(mask));
    static uintptr_t hits[256];
    size_t n = mem_scan(0x10000, 0x7FFFFFFFFFFFull, (const uint8_t*)&pat, mask, 8, ERMC_SCAN_PRIVATE, hits, 256);
    int m = 0;
    for (size_t i = 0; i < n && m < kMaxDepthScan; i++) {
        uint8_t* dsv = (uint8_t*)hits[i];
        if (!mem_readable(dsv, 0x20)) continue;
        uint8_t* tex = *(uint8_t**)(dsv + 8);
        if (!tex || !mem_readable(tex, 0x28) || *(uintptr_t*)tex != base + kVtCGTexture2D) continue;
        ID3D12Resource* res = *(ID3D12Resource**)(tex + 0x20);
        if (!res) continue;
        bool dup = false;
        for (int k = 0; k < m; k++) dup |= g_scanHits[k].res == res;
        if (dup) continue;
        g_scanHits[m++] = {dsv, tex, res, *(uint32_t*)(dsv + 0x18)};
    }
    g_scanHitCount = m;
    log("compositor: %zu depth-stencil views in the heap, %d distinct textures", n, m);
}

static void release_depth() {
    for (int i = 0; i < g_depthCandCount; i++) safe_release(g_depthCands[i]);
    g_depthCandCount = 0;
    g_depthBound = -1;
    if (g_srvHeap) make_srv(nullptr, DXGI_FORMAT_R32_FLOAT, kSrvHostDepth);
}

// Present thread: take the worker's results.
static void adopt_depth_views() {
    uintptr_t base = main_module_base();
    release_depth();
    for (int i = 0; i < g_scanHitCount && g_depthCandCount < kMaxDepthCands; i++) {
        DepthScanHit& h = g_scanHits[i];
        // Still the same view -> texture -> resource chain (nothing was destroyed meanwhile)?
        if (!mem_readable(h.dsv, 0x20) || *(uintptr_t*)h.dsv != base + kVtCGDepthStencilView ||
            *(uint8_t**)(h.dsv + 8) != h.tex || !mem_readable(h.tex, 0x28) ||
            *(uintptr_t*)h.tex != base + kVtCGTexture2D || *(ID3D12Resource**)(h.tex + 0x20) != h.res)
            continue;
        if (!readable(h.res, 8)) continue;
        D3D12_RESOURCE_DESC d = h.res->GetDesc();
        bool full = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == g_bbW && d.Height == g_bbH &&
                    d.SampleDesc.Count == 1 && d.DepthOrArraySize == 1 &&
                    !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
        DXGI_FORMAT sf = depth_srv_format(d.Format);
        log("compositor: depth view %p: %llux%u format %u (dsv %u) %s", (void*)h.res, (unsigned long long)d.Width,
            d.Height, d.Format, h.dsvFormat & 0xFFFF, full && sf != DXGI_FORMAT_UNKNOWN ? "-> candidate" : "");
        if (!full || sf == DXGI_FORMAT_UNKNOWN) continue;
        h.res->AddRef();
        g_depthCands[g_depthCandCount] = h.res;
        g_depthSrvFmt[g_depthCandCount] = sf;
        g_depthCandCount++;
    }
    log("compositor: %d full-screen depth candidate(s)", g_depthCandCount);
}

// Worker thread: runs a requested scan.
void compositor_depth_poll() {
    if (InterlockedCompareExchange(&g_depthScanState, 2, 1) != 1) return;
    scan_depth_views();
    InterlockedExchange(&g_depthScanState, 3);
}
#else
// The old heap scan identifies engine wrappers, not submitted D3D12 states. In particular
// it cannot establish StateBefore, split barriers, subresources, aliasing or queue ownership.
// Never call guessed COM pointers or transition a native depth resource with assumed states.
static const int g_depthBound = -1; // native snapshot has no user-selectable resource index
static void release_depth() {
    depth_release_snapshots(); // only called after wait_idle / before any submissions exist
    depth_clear_native_ledger();
    if (g_srvHeap) make_srv(nullptr, DXGI_FORMAT_R32_FLOAT, kSrvHostDepth);
}
void compositor_depth_poll() {}
#endif

static bool g_testWasOn = false;
static bool g_loggedFirstMc = false;

static int g_initTries = 0;
#if ERMC_NATIVE_WINDOWS
static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain*, UINT, UINT);
static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static HRESULT STDMETHODCALLTYPE hkResizeBuffers1(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
    const UINT*, IUnknown* const*);
#include "compositor_windows.h"
#include "compositor_reshade.h"
#endif

static void composite(IDXGISwapChain* sc, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return;
    struct SubmissionProof {
        bool submitted = false;
        ~SubmissionProof() {
            if (!submitted) g_worldCompositeStamp.store(0, std::memory_order_release);
        }
    } proof;
#if ERMC_NATIVE_WINDOWS
    NativeCompositorScope internalCommands;  // audit host calls only, including nested hooks
    static McHeartbeatWatch consumer;
    ErmcHeader* header = shm_header();
    bool live = consumer.update(header->mcPid, header->mcStartMs, header->mcHeartbeat, GetTickCount64());
    if (live && (g_noticeProducerPid != header->mcPid || g_noticeProducerStart != header->mcStartMs)) {
        // An actually progressed replacement producer is a new publication epoch.
        // Mere loss/recovery of heartbeat or composition eligibility is not.
        reset_notice_leases();
        g_noticeProducerPid = header->mcPid;
        g_noticeProducerStart = header->mcStartMs;
    }
    if (!live) forget_uploaded_frame();  // never reuse a frame across an inactive producer
    ErmcControl ctrl = {};
    bool snapshot = live && control_snapshot(&ctrl);
    bool test = (header->debugFlags & 1) != 0;
    const bool passive = passive_composite_control(ctrl);
    bool mc = snapshot && composite_control_valid(ctrl) && (ctrl.flags & ERMC_CTRL_COMPOSITE) && frames_ready();
    if (passive) {
        uint32_t stateFlags = shm_state()->flags;
        mc = mc && (stateFlags & ERMC_STATE_CAMERA_VALID) && (stateFlags & ERMC_STATE_PLAYER_VALID) &&
            !(stateFlags & (ERMC_STATE_PLAYER_DEAD | ERMC_STATE_HOST_BUSY));
    }
    if (!mc || passive != g_lastCompositePassive) forget_uploaded_frame();
    g_lastCompositePassive = passive;
    const uint64_t targetPose = composite_frame_target(ctrl, pose_for_present(ctrl.poseLag));
    g_compositeTargetPose = targetPose;
    native_depth_audit_request(snapshot && ((ctrl.flags & ERMC_CTRL_DEBUG_DEPTH) || g_nativeSceneEnabled));
    // No consumer/frame means no queue probing, swapchain adoption, shader compilation,
    // resource allocation or fence waits. The explicit synthetic test remains available.
    ErmcFrameHeader* slot = nullptr;
    FrameUploadStamp firstPicked;
    if (mc) slot = pick_slot(targetPose, &firstPicked);
    if (passive && (!slot || !passive_frame_valid(firstPicked, targetPose) || !observe_notice(firstPicked, now_ms()))) {
        forget_uploaded_frame();
        slot = nullptr;
    }
    if (!test && (!mc || (!slot && !g_haveFrame))) {
        native_cancel_probe();
        return;
    }
#endif
    if (!g_presentThread) {
        g_presentThread = GetCurrentThreadId();
        log("compositor: the game presents on thread %lu (swapchain %p)", g_presentThread, (void*)sc);
    }
    if (g_failed) return;
    if (!g_sc) {
#if ERMC_NATIVE_WINDOWS
        if (InterlockedCompareExchange(&g_nativeStopping, 0, 0)) return;
        DXGI_SWAP_CHAIN_DESC sd = {};
        HWND hwnd = game_hwnd();
        if (FAILED(sc->GetDesc(&sd)) || (hwnd && sd.OutputWindow != hwnd)) return;
        ID3D12Device* dev = nullptr;
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev))) return;
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) {
            safe_release(dev);
            return;
        }
        DXGI_SWAP_CHAIN_DESC1 d = {};
        if (FAILED(sc3->GetDesc1(&d)) || dev->GetNodeCount() != 1 ||
            (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             d.Format != DXGI_FORMAT_R10G10B10A2_UNORM)) {
            safe_release(sc3);
            safe_release(dev);
            return;
        }
        ID3D12CommandQueue* q = native_queue_for(sc, dev);  // owned reference, never an offset
        if (!q) {
            safe_release(sc3);
            safe_release(dev);
            return;  // late injection: wait for an observed backbuffer submission
        }
        g_sc = sc3;
        g_dev = dev;
        g_queue = q;
        native_depth_audit_target(dev, q);
        g_bbFormat = d.Format;
        log("compositor: native swapchain %ux%u format %u, %u buffers, captured queue %p", d.Width, d.Height,
            d.Format, d.BufferCount, (void*)q);
        log("compositor: native host-depth requires authoritative scene/projection certification; safe snapshot path available, fallback until evidence is complete");
#else
        // The game's swapchain: its device, and its direct queue (D3DMetal keeps the inner
        // queue object at swapchain+0x28, 0x58 bytes into the app-visible one). Before
        // touching that queue, check it is of the same class as a queue we make ourselves.
        if (*(void***)sc != g_pp->table) log("compositor: note: this swapchain's table is %p", (void*)*(void***)sc);
        if (FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&g_dev))) {
            log("compositor: no D3D12 device from the swapchain; compositing off");
            g_failed = true;
            return;
        }
        uint8_t* inner = *(uint8_t**)((uint8_t*)sc + 0x28);
        ID3D12CommandQueue* q = inner ? (ID3D12CommandQueue*)(inner - 0x58) : nullptr;
        ID3D12CommandQueue* mine = nullptr;
        D3D12_COMMAND_QUEUE_DESC md = {};
        md.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        bool same = SUCCEEDED(g_dev->CreateCommandQueue(&md, __uuidof(ID3D12CommandQueue), (void**)&mine)) && q &&
                    readable(q, 8) && *(void**)q == *(void**)mine;
        safe_release(mine);
        if (!same) {
            log("compositor: the swapchain's command queue doesn't look like D3DMetal 3.0's; compositing off");
            safe_release(g_dev);
            g_failed = true;
            return;
        }
        D3D12_COMMAND_QUEUE_DESC qd = q->GetDesc();
        if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            log("compositor: the swapchain's queue is not a direct queue (%d); compositing off", qd.Type);
            safe_release(g_dev);
            g_failed = true;
            return;
        }
        g_sc = (IDXGISwapChain3*)sc;
        g_queue = q;
        g_queue->AddRef();
        DXGI_SWAP_CHAIN_DESC1 d;
        g_sc->GetDesc1(&d);
        g_bbFormat = d.Format;
        log("compositor: game swapchain %ux%u format %u, %u buffers, queue %p", d.Width, d.Height, d.Format,
            d.BufferCount, (void*)q);
        if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            d.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
            log("compositor: unsupported back buffer format %u (HDR?); compositing off", d.Format);
            g_failed = true;
            return;
        }
#endif
    } else if ((IDXGISwapChain3*)sc != g_sc) {
        return;  // not the game's swapchain
    }

#if !ERMC_NATIVE_WINDOWS
    bool test = (shm_header()->debugFlags & 1) != 0;
#endif
    if (test != g_testWasOn) {
        g_testWasOn = test;
        log("compositor: test pattern %s", test ? "on" : "off");
    }
#if !ERMC_NATIVE_WINDOWS
    ErmcControl ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    bool mc = control_snapshot(&ctrl) && composite_control_valid(ctrl) && (ctrl.flags & ERMC_CTRL_COMPOSITE) && frames_open();
    const bool passive = passive_composite_control(ctrl);
    if (!mc || passive != g_lastCompositePassive) forget_uploaded_frame();
    g_lastCompositePassive = passive;
    const uint64_t targetPose = composite_frame_target(ctrl, pose_for_present(ctrl.poseLag));
#endif
    if (!test && !mc) return;

    // Pipeline objects are made the first time something is to be drawn.
    if (!g_pso) {
        if (!init_device_objects()) {
            log("compositor: device objects FAILED; compositing off");
            g_failed = true;
            return;
        }
        log("compositor: device objects ready");
    }
    if (!g_bbCount && !acquire_backbuffers()) {
        log("compositor: cannot get the back buffers; compositing off");
        g_failed = true;
        return;
    }

#if !ERMC_NATIVE_WINDOWS
    ErmcFrameHeader* slot = nullptr;
#endif
    FrameUploadStamp picked;
    if (mc) {
        slot = pick_slot(targetPose, &picked);
        if (passive && (!slot || !passive_frame_valid(picked, targetPose) || !observe_notice(picked, now_ms()))) {
            forget_uploaded_frame();
            return;
        }
        note_sync(slot, targetPose);
#ifdef ERMC_COMPOSITOR_HARNESS
        compositor_harness_after_pose_selection();
#endif
        if (slot && !ensure_frame_resources(picked.width, picked.height)) return;
        if (!slot && !g_haveFrame) return;
        if (!slot && !uploaded_frame_matches(g_uploadedFrame.slot, passive ? targetPose : 0)) return;
    }

    RingSlot& r = g_ring[g_ringPos % kRing];
#if ERMC_NATIVE_WINDOWS
    if (!frame_fence_ready(r.fence)) return;
#else
    if (!wait_fence(r.fence, 50)) return;  // GPU far behind: skip rather than overwrite in-use memory
#endif
    bool hand = false;
    FrameUploadStamp uploadStamp = {};
    bool uploaded = slot && r.uploadPtr && copy_slot(slot, r, &hand, &uploadStamp, passive ? targetPose : 0, &picked);
    // False can mean unchanged OR inconsistent. Only a stable, exact publication
    // can reuse textures; a torn/new slot cannot certify the previous geometry.
    if (slot && !uploaded && (!uploaded_frame_matches(slot, passive ? targetPose : 0) ||
        !same_frame_upload(picked, g_uploadedFrame))) return;
    // Copying CPU pixels does not commit GPU texture metadata. Failed recording or
    // submission must leave the metadata describing only the last submitted frame.
    const FrameUploadStamp selected = uploaded ? uploadStamp : g_uploadedFrame;
    if (passive && (!selected.slot || selected.poseId != targetPose || (!uploaded && !g_haveFrame))) return;
    if (mc && !uploaded && !g_haveFrame) return;

    if (FAILED(r.alloc->Reset()) || FAILED(g_list->Reset(r.alloc, g_pso))) return;
    if (uploaded) {
        int layers = passive ? (notice_live(selected, now_ms()) ? 3 : 2) : (hand ? 4 : 3);
        for (int i = 0; i < layers; i++) transition(g_tex[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        for (int i = 0; i < layers; i++) {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = g_tex[i];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = r.upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = g_fp[i];
            g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        for (int i = 0; i < layers; i++) transition(g_tex[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    UINT bi = g_sc->GetCurrentBackBufferIndex();
    if (bi >= g_bbCount) {
        g_list->Close();
        return;
    }
    // Occlusion: find the game's depth buffers once (and again after a resize).
#if !ERMC_NATIVE_WINDOWS
    if (mc && g_depthScanState == 0 && g_depthCandCount == 0 && now_ms() - g_depthScanAtMs > 15000) {
        g_depthScanAtMs = now_ms();
        InterlockedExchange(&g_depthScanState, 1);
    }
    if (g_depthScanState == 3) {
        adopt_depth_views();
        InterlockedExchange(&g_depthScanState, 0);
    }
    int want = g_depthCandCount ? (int)(ctrl.depthIndex % (uint32_t)g_depthCandCount) : -1;
    if (want != g_depthBound) {
        if (want >= 0) make_srv(g_depthCands[want], g_depthSrvFmt[want], kSrvHostDepth);
        else make_srv(nullptr, DXGI_FORMAT_R32_FLOAT, kSrvHostDepth);
        g_depthBound = want;
        if (want >= 0) log("compositor: occlusion uses depth candidate %d (%p)", want, (void*)g_depthCands[want]);
    }
#endif
    ErmcGameState* st = shm_state();
    Params p = {};
    p.mcNear = selected.nearZ;
    p.mcFar = selected.farZ;
    p.hostNear = st->nearZ > 0.001f ? st->nearZ : 0.05f;
    p.hostFar = st->farZ > 1.0f ? st->farZ : 10000.0f;
    p.hostReversed = (shm_header()->debugFlags & 2) ? 0.0f : 1.0f;  // Elden Ring: reversed Z (dev flag bit1: standard)
    p.useDepth = (g_depthBound >= 0 && !(ctrl.flags & ERMC_CTRL_NO_DEPTH_TEST)) ? 1.0f : 0.0f;
    p.debugView = (ctrl.flags & ERMC_CTRL_DEBUG_DEPTH) && g_depthBound >= 0 ? 1.0f : 0.0f;
    p.bias = 0.03f;
    p.testPattern = (test && !mc) ? 1.0f : 0.0f;
    bool relight = mc && g_sceneCopy && !(ctrl.flags & ERMC_CTRL_NO_RELIGHT);
    p.relight = relight ? 1.0f : 0.0f;
    p.lightGain = ctrl.lightGain > 0 ? ctrl.lightGain : 2.6f;
    p.lightMin = ctrl.lightMin > 0 ? ctrl.lightMin : 0.18f;
    p.fogStrength = ctrl.fogStrength > 0 ? ctrl.fogStrength : 0.55f;
    p.fogStart = 40.0f;
    p.fogEnd = 400.0f;
    p.tintAmount = 0.35f;
    p.handLayer = (mc && (uploaded ? hand : g_texHand) && !passive) ? 1.0f : 0.0f;
    p.suppressGui = passive && !notice_live(selected, now_ms()) ? 1.0f : 0.0f;

#if ERMC_NATIVE_WINDOWS
    NativeDepthPresentScope depthSubmission(g_nativeSceneEnabled.load(std::memory_order_relaxed));
    const UINT depthRing = g_ringPos % kRing;
    bool nativeDepth = false;
    const bool depthEnabled = mc && !(ctrl.flags & ERMC_CTRL_NO_DEPTH_TEST);
    const bool poseMatches = selected.poseId == targetPose;
    if (depthEnabled && reshade_depth_available()) {
        if (!poseMatches || !reshade_aspect_matches(g_texW, g_texH, g_bbW, g_bbH)) {
            // An unrelated world image cannot be depth-tested against this host scene.
            // Keep the independent hand/HUD; missing depth and intentional bypass retain legacy behavior.
            p.suppressWorld = 1.0f;
        } else {
            nativeDepth = reshade_prepare_snapshot(depthRing, p, ctrl);
        }
    }
    if (!nativeDepth && !p.suppressWorld && depthEnabled && depthSubmission.locked && poseMatches)
        nativeDepth = depth_prepare_snapshot(depthRing,selected.poseId,selected.fov,p);
    ID3D12DescriptorHeap* frameHeap = nativeDepth ? g_depthRings[depthRing].heap : g_srvHeap;
#endif
    g_list->SetGraphicsRootSignature(g_rootSig);
#if ERMC_NATIVE_WINDOWS
    g_list->SetDescriptorHeaps(1, &frameHeap);
#else
    g_list->SetDescriptorHeaps(1, &g_srvHeap);
#endif
    g_list->SetGraphicsRoot32BitConstants(1, sizeof(Params) / 4, &p, 0);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (relight) {
        // Elden Ring's finished frame, then two 8x box blurs of it.
        transition(g_bb[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(g_sceneCopy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        g_list->CopyResource(g_sceneCopy, g_bb[bi]);
        transition(g_sceneCopy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(g_bb[bi], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_list->SetPipelineState(g_psoDown);
        struct Pass {
            ID3D12Resource* target;
            int rtv, table;
            UINT w, h;
        } passes[2] = {{g_down1, kRtvDown1, kTableDown1, g_down1W, g_down1H},
                       {g_down2, kRtvDown2, kTableDown2, g_down2W, g_down2H}};
        for (const Pass& ps : passes) {
            transition(ps.target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE rt = rtv_cpu(ps.rtv);
            g_list->OMSetRenderTargets(1, &rt, FALSE, nullptr);
            D3D12_VIEWPORT pv = {0, 0, (float)ps.w, (float)ps.h, 0, 1};
            D3D12_RECT pr = {0, 0, (LONG)ps.w, (LONG)ps.h};
            g_list->RSSetViewports(1, &pv);
            g_list->RSSetScissorRects(1, &pr);
            g_list->SetGraphicsRootDescriptorTable(0,
#if ERMC_NATIVE_WINDOWS
                nativeDepth ? depth_table_gpu(depthRing,ps.table) :
#endif
                table_gpu(ps.table));
            g_list->DrawInstanced(3, 1, 0, 0);
            transition(ps.target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        g_list->SetPipelineState(g_pso);
    } else {
        transition(g_bb[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    g_list->SetGraphicsRootDescriptorTable(0,
#if ERMC_NATIVE_WINDOWS
        nativeDepth ? depth_table_gpu(depthRing,kTableComposite) :
#endif
        table_gpu(kTableComposite));
    D3D12_VIEWPORT vp = {0, 0, (float)g_bbW, (float)g_bbH, 0, 1};
    D3D12_RECT sr = {0, 0, (LONG)g_bbW, (LONG)g_bbH};
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_cpu((int)bi);
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    // D3DMetal textures are untracked: these barriers are what orders our read of the game's
    // depth against its next frame's clear. The before-state isn't checked.
#if !ERMC_NATIVE_WINDOWS
    ID3D12Resource* hostDepth = (p.useDepth > 0.5f || p.debugView > 0.5f) ? g_depthCands[g_depthBound] : nullptr;
    if (hostDepth) transition(hostDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
#endif
    g_list->DrawInstanced(3, 1, 0, 0);
#if !ERMC_NATIVE_WINDOWS
    if (hostDepth) transition(hostDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
#endif
    transition(g_bb[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    if (FAILED(g_list->Close())) return;
#ifdef ERMC_COMPOSITOR_HARNESS
    compositor_harness_before_submission();
#endif
    if (passive) {
        FrameUploadStamp finalStamp;
        if (!capture_frame_stamp(selected.slot, &finalStamp) || !same_frame_upload(selected, finalStamp) ||
            !passive_frame_valid(finalStamp, targetPose) ||
            (p.suppressGui == 0.0f && !notice_live(selected, now_ms()))) return;
    }
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    uint64_t fence = ++g_fenceNext;
    if (FAILED(g_queue->Signal(g_fence, fence))) {
        // The list was submitted, so no ring slot is safe to reuse without a completion.
        g_failed = true;
        log("compositor: queue Signal failed after submission; compositing stopped");
        return;
    }
#if ERMC_NATIVE_WINDOWS
    native_depth_audit_report(g_dev, g_bbW, g_bbH, (ctrl.flags & ERMC_CTRL_DEBUG_DEPTH) != 0);
#endif
    r.fence = fence;
    if (uploaded) commit_uploaded_frame(uploadStamp, hand);
    g_ringPos++;
#ifdef ERMC_COMPOSITOR_HARNESS
    compositor_harness_capture(bi, p.useDepth, p.debugView);
#endif
    if (mc) {
        g_lastCompositeMs = now_ms();
        g_worldCompositeStamp.store(p.suppressWorld == 0.0f ? (g_lastCompositeMs << 1) | uint64_t(passive) : 0,
            std::memory_order_release);
        proof.submitted = true;
        if (!g_loggedFirstMc) {
            g_loggedFirstMc = true;
            log("compositor: drawing Minecraft's frames into Elden Ring's (%ux%u into %ux%u)", g_texW, g_texH, g_bbW, g_bbH);
        }
    }
}

// ---------------------------------------------------------------------------------------
// Hooks

typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* Resize_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Resize1_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                              IUnknown* const*);

static volatile LONG g_inPresent = 0;

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    InflightGuard guard;
#if ERMC_NATIVE_WINDOWS
    native_note_hook(kSlotPresent);
    if (!reshade_adapter_loaded() && !InterlockedCompareExchange(&g_nativeStopping, 0, 0) && TryAcquireSRWLockExclusive(&g_renderLock)) {
        LARGE_INTEGER start = {}; native_begin_composite_profile(&start);
        composite(sc, flags);
        native_end_composite_profile(start);
        ReleaseSRWLockExclusive(&g_renderLock);
    }
#else
    if (InterlockedCompareExchange(&g_inPresent, 1, 0) == 0) {
        composite(sc, flags);
        InterlockedExchange(&g_inPresent, 0);
    }
#endif
    return ((Present_t)original_hook(kSlotPresent))(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                            const DXGI_PRESENT_PARAMETERS* params) {
    InflightGuard guard;
#if ERMC_NATIVE_WINDOWS
    native_note_hook(kSlotPresent1);
    if (!reshade_adapter_loaded() && !InterlockedCompareExchange(&g_nativeStopping, 0, 0) && TryAcquireSRWLockExclusive(&g_renderLock)) {
        LARGE_INTEGER start = {}; native_begin_composite_profile(&start);
        composite(sc, flags);
        native_end_composite_profile(start);
        ReleaseSRWLockExclusive(&g_renderLock);
    }
#else
    if (InterlockedCompareExchange(&g_inPresent, 1, 0) == 0) {
        composite(sc, flags);
        InterlockedExchange(&g_inPresent, 0);
    }
#endif
    return ((Present1_t)original_hook(kSlotPresent1))(sc, sync, flags, params);
}

// The swapchain can only resize once nobody holds its buffers.
static bool before_resize(void* sc) {
    if ((IDXGISwapChain3*)sc != g_sc || !g_bbCount) return true;
    if (!wait_idle()) {
        log("compositor: GPU did not become idle for resize; buffers retained");
        return false;
    }
    release_backbuffers();
    log("compositor: swapchain resizing; back buffers released");
    return true;
}

static HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    InflightGuard guard;
#if ERMC_NATIVE_WINDOWS
    if (g_nativeResizeDepth || reshade_adapter_loaded()) return ((Resize_t)original_hook(kSlotResize))(sc, n, w, h, f, fl);
    AcquireSRWLockExclusive(&g_renderLock);
    ++g_nativeResizeDepth;
    native_before_resize(sc);
    HRESULT hr = before_resize(sc) ? ((Resize_t)original_hook(kSlotResize))(sc, n, w, h, f, fl)
                                  : DXGI_ERROR_WAS_STILL_DRAWING;
    native_after_resize(sc, hr, 0, nullptr);
    --g_nativeResizeDepth;
    ReleaseSRWLockExclusive(&g_renderLock);
    return hr;
#else
    if (!before_resize(sc)) return DXGI_ERROR_WAS_STILL_DRAWING;
    return ((Resize_t)original_hook(kSlotResize))(sc, n, w, h, f, fl);
#endif
}

static HRESULT STDMETHODCALLTYPE hkResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl,
                                                  const UINT* mask, IUnknown* const* queues) {
    InflightGuard guard;
#if ERMC_NATIVE_WINDOWS
    if (g_nativeResizeDepth || reshade_adapter_loaded()) return ((Resize1_t)original_hook(kSlotResize1))(sc, n, w, h, f, fl, mask, queues);
    AcquireSRWLockExclusive(&g_renderLock);
    ++g_nativeResizeDepth;
    native_before_resize(sc);
    HRESULT hr = before_resize(sc) ? ((Resize1_t)original_hook(kSlotResize1))(sc, n, w, h, f, fl, mask, queues)
                                  : DXGI_ERROR_WAS_STILL_DRAWING;
    native_after_resize(sc, hr, n, queues);
    --g_nativeResizeDepth;
    ReleaseSRWLockExclusive(&g_renderLock);
    return hr;
#else
    if (!before_resize(sc)) return DXGI_ERROR_WAS_STILL_DRAWING;
    return ((Resize1_t)original_hook(kSlotResize1))(sc, n, w, h, f, fl, mask, queues);
#endif
}

// Installs the Present hooks (or adopts those of an earlier core). The swapchain table only
// exists once the game made its swapchain, so the worker thread retries (compositor_poll).
bool compositor_init() {
#if ERMC_NATIVE_WINDOWS
    if (reshade_adapter_loaded()) return true;
    bool ready = native_init();
    if (ready) depth_attach_execute();
    return ready;
#else
    if (g_pp) return true;
    ErmcHeader* hdr = shm_header();
    PresentPage* pg = (PresentPage*)(uintptr_t)hdr->hostPresentPage;
    if (pg && mem_readable(pg, sizeof(*pg)) && pg->magic == page_magic()) {
        log("compositor: Present hooks from an earlier core found (table %p)", (void*)pg->table);
    } else {
        g_initTries++;
        void** table = find_swapchain_table();
        if (!table || !mem_readable(table, 41 * sizeof(void*))) {
            if (g_initTries == 1) log("compositor: no swapchain table yet; will retry");
            return false;
        }
        pg = (PresentPage*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!pg) return false;
        memset(pg, 0, sizeof(*pg));
        pg->table = table;
        for (int k = 0; k < kHookCount; k++) {
            pg->orig[k] = table[kVtblIndex[k]];
            write_stub(pg, k);
        }
        FlushInstructionCache(GetCurrentProcess(), pg, sizeof(*pg));
        pg->magic = page_magic();
        hdr->hostPresentPage = (uint64_t)(uintptr_t)pg;
        // Plain stores: D3DMetal keeps this table writable (D3DM_ALLOW_HOOKING, on by default).
        for (int k = 0; k < kHookCount; k++) table[kVtblIndex[k]] = pg->stub[k];
        log("compositor: hooked %s/%s/%s/%s in D3DMetal's swapchain table %p", kHookNames[0], kHookNames[1],
            kHookNames[2], kHookNames[3], (void*)table);
    }
    g_pp = pg;
    pg->target[kSlotResize] = (void*)&hkResizeBuffers;
    pg->target[kSlotResize1] = (void*)&hkResizeBuffers1;
    pg->target[kSlotPresent1] = (void*)&hkPresent1;
    pg->target[kSlotPresent] = (void*)&hkPresent;
    return true;
#endif
}

// Worker thread, about once a second until the hooks are in (gives up after 3 minutes).
void compositor_poll() {
    static uint64_t last = 0;
#if ERMC_NATIVE_WINDOWS
    frames_poll();
    if (reshade_adapter_loaded()) return;
    if (g_nativeHooked) { native_poll_diagnostics(); return; }
    if (g_initTries >= 180) return;
#else
    if (g_pp || g_initTries >= 180) return;
#endif
    uint64_t now = now_ms();
    if (now - last < 1000) return;
    last = now;
    if (!compositor_init() && g_initTries >= 180) log("compositor: gave up discovering Present hooks");
}

// First step of a core shutdown: from now on the stubs go straight to the originals.
void compositor_detach() {
#if ERMC_NATIVE_WINDOWS
    native_detach();
#else
    if (!g_pp) return;
    for (int k = 0; k < kHookCount; k++) g_pp->target[k] = nullptr;
#endif
}

// After every hook has returned: release everything (the GPU must be done with it).
void compositor_shutdown() {
    if (!wait_idle()) {
        // A deliberate leak is safer than freeing allocations referenced by a running GPU.
        log("compositor: GPU still busy at shutdown; retaining D3D12 allocations until process exit");
#if ERMC_NATIVE_WINDOWS
        native_shutdown();
#endif
        reset_notice_leases();
        if (g_frames) UnmapViewOfFile(g_frames);
        g_frames = nullptr;
        return;
    }
    release_frame_resources();
    release_backbuffers();
    release_depth();
    safe_release(g_pso);
    safe_release(g_psoDown);
    safe_release(g_rootSig);
    safe_release(g_list);
    for (auto& r : g_ring) safe_release(r.alloc);
    safe_release(g_srvHeap);
    safe_release(g_srvCpuHeap);
    safe_release(g_rtvHeap);
    safe_release(g_fence);
    if (g_fenceEvent) CloseHandle(g_fenceEvent);
    g_fenceEvent = nullptr;
    safe_release(g_queue);
    safe_release(g_dev);
#if ERMC_NATIVE_WINDOWS
    safe_release(g_sc);
    native_shutdown();
#endif
    g_sc = nullptr;
    reset_notice_leases();
    if (g_frames) UnmapViewOfFile(g_frames);
    g_frames = nullptr;
}

}  // namespace mb

#if ERMC_NATIVE_WINDOWS
#include "reshade_bridge_api.h"
extern "C" __declspec(dllexport) void erb_reshade_depth_v2(unsigned version, unsigned event,
    IDXGISwapChain3* sc, ID3D12CommandQueue* queue, ID3D12Device* device, ID3D12Resource* depth, unsigned long long generation) {
    using namespace mb;
    InflightGuard guard;
    if (version != 2 || !sc || !shm_header() || InterlockedCompareExchange(&g_nativeStopping, 0, 0)) return;
    AcquireSRWLockExclusive(&g_renderLock);
    if (event == ErmcDepthResize) {
        before_resize(sc);
    } else if (event == ErmcDepthRender && queue) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        if (SUCCEEDED(sc->GetDesc(&sd)) && sd.OutputWindow == game_hwnd()) {
            // Associate once; repeated SetPrivateDataInterface/logging here costs a
            // COM update and disk write on every frame. Reject an unexpected queue.
            if (g_queue && native_identity(g_queue) != native_identity(queue)) {
                ReleaseSRWLockExclusive(&g_renderLock);
                return;
            }
            if (!g_sc) {
                ID3D12CommandQueue* associated = nullptr;
                UINT size = sizeof(associated);
                sc->GetPrivateData(kNativeQueueGuid, &size, &associated);
                if (!associated) native_capture_queue(queue, sc);
                safe_release(associated);
            }
            g_reshadeDepth = depth;
            g_reshadeDevice = device;
            g_reshadeGeneration = generation;
            LARGE_INTEGER start = {}; native_begin_composite_profile(&start);
            composite(sc, 0);
            native_end_composite_profile(start);
            static uint64_t lastDepthStatus = 0;
            if (now_ms() - lastDepthStatus > 5000) {
                lastDepthStatus = now_ms();
                ErmcControl current = {}; control_snapshot(&current);
                log("depth adapter: source %p pose uploaded %llu present %llu state %u control %u",
                    depth, (unsigned long long)g_uploadedPose,
                    (unsigned long long)g_compositeTargetPose, shm_state()->flags, current.flags);
            }
            g_reshadeDepth = nullptr;
            g_reshadeDevice = nullptr;
            g_reshadeGeneration = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_renderLock);
}
#endif
