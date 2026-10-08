// Included inside namespace mb by compositor_windows.h. Diagnostic counts stay distinct
// from the authoritative scene/state ledger. Recorded StateAfter alone NEVER enables depth.
#pragma once

static const GUID kNativeDepthListGuid = {0x7bf342d9, 0x5e2c, 0x4f6b, {0x98, 0x8a, 0x9d, 0x47, 0x81, 0xc6, 0x42, 0x13}};
static const GUID kNativeDepthDeviceGuid = {0x7bf342d9, 0x5e2c, 0x4f6b, {0x98, 0x8a, 0x9d, 0x47, 0x81, 0xc6, 0x42, 0x14}};
static SRWLOCK g_depthAuditLock = SRWLOCK_INIT;
static uint64_t g_depthAuditEpoch = 1;
static uint64_t g_depthAuditMetadataErrors = 0, g_depthAuditReportAt = 0;
static bool g_depthAuditReported = false;
static UINT g_depthAuditHooks = 0;
static std::atomic<bool> g_depthAuditEnabled{false};  // opt in with ERMC_CTRL_DEBUG_DEPTH
// Only the selected device/queue are retained. Neither reference points to a depth resource
// or a descriptor heap, so diagnostics add no GPU work or resource/fence retirement problem.
static ID3D12Device* g_depthAuditDevice = nullptr;
static ID3D12CommandQueue* g_depthAuditQueue = nullptr;

struct NativeDepthAuditLock {
    NativeDepthAuditLock() { AcquireSRWLockExclusive(&g_depthAuditLock); }
    ~NativeDepthAuditLock() { ReleaseSRWLockExclusive(&g_depthAuditLock); }
};
struct NativeDepthAuditCounts {
    uint64_t binds = 0, clears = 0, transitions = 0, split = 0, subresources = 0;
    uint64_t aliasing = 0, bundles = 0, renderPassDepth = 0, enhanced = 0;
};
struct NativeDepthAuditList {
    uint64_t epoch = 0;
    bool resetObserved = false, closed = false;
    NativeDepthAuditCounts events;
};
struct NativeDepthAuditDevice {
    uint64_t epoch = 0, creates = 0, nullCreates = 0, denySrv = 0, msaa = 0, arrayOrMips = 0;
    uint64_t copies = 0, simpleCopies = 0;
    D3D12_RESOURCE_DESC lastResource = {};
    DXGI_FORMAT lastViewFormat = DXGI_FORMAT_UNKNOWN;
};
struct NativeDepthAuditTotals {
    uint64_t submissions = 0, lists = 0, unknown = 0, partial = 0;
    NativeDepthAuditCounts events;
};
static NativeDepthAuditTotals g_depthAuditHost, g_depthAuditOther;

static bool native_depth_audit_active() {
    return g_depthAuditEnabled.load(std::memory_order_relaxed) && !g_nativeCompositorCommands &&
        !InterlockedCompareExchange(&g_nativeStopping, 0, 0);
}
template <typename T>
static bool native_depth_audit_read(ID3D12Object* object, REFGUID key, T& data) {
    UINT bytes = sizeof(data);
    if (SUCCEEDED(object->GetPrivateData(key, &bytes, &data)) && bytes == sizeof(data) &&
        data.epoch == g_depthAuditEpoch) return true;
    data = {};
    data.epoch = g_depthAuditEpoch;
    return false;
}
template <typename T>
static void native_depth_audit_write(ID3D12Object* object, REFGUID key, const T& data) {
    if (FAILED(object->SetPrivateData(key, sizeof(data), &data))) ++g_depthAuditMetadataErrors;
}
#include "compositor_depth_native.h"
static void native_depth_audit_reset() {
    depth_clear_native_ledger();
    NativeDepthAuditLock lock;
    ++g_depthAuditEpoch;
    g_depthAuditHost = {}; g_depthAuditOther = {};
    g_depthAuditMetadataErrors = 0;
    g_depthAuditReported = false;
}
static void native_depth_audit_begin() {
    LARGE_INTEGER stamp;
    QueryPerformanceCounter(&stamp);
    NativeDepthAuditLock lock;
    // A new load cannot consume an earlier core's COM private data as current evidence.
    g_depthAuditEpoch = (uint64_t)stamp.QuadPart;
    g_depthAuditHooks = 0;
    g_depthAuditHost = {}; g_depthAuditOther = {};
    g_depthAuditMetadataErrors = 0; g_depthAuditReported = false;
    g_depthAuditEnabled.store(false, std::memory_order_relaxed);
}
static void native_depth_audit_request(bool requested) {
    if (g_depthAuditEnabled.load(std::memory_order_relaxed) == requested) return;
    if (g_depthAuditEnabled.exchange(requested, std::memory_order_relaxed) != requested)
        native_depth_audit_reset();  // never consume recordings from an earlier audit session
}
static void native_depth_audit_target(ID3D12Device* dev, ID3D12CommandQueue* queue) {
    NativeDepthAuditLock lock;
    dev->AddRef(); queue->AddRef();
    safe_release(g_depthAuditDevice); safe_release(g_depthAuditQueue);
    g_depthAuditDevice = dev; g_depthAuditQueue = queue;
}
static void native_depth_audit_shutdown() {
    depth_clear_native_ledger();
    NativeDepthAuditLock lock;
    safe_release(g_depthAuditQueue); safe_release(g_depthAuditDevice);
    g_depthAuditHost = {}; g_depthAuditOther = {};
    g_depthAuditHooks = 0;
    g_depthAuditEnabled.store(false, std::memory_order_relaxed);
}
static void native_depth_audit_reset_list(ID3D12GraphicsCommandList* list) {
    if (!native_depth_audit_active()) return;
    depth_capture_reset(list);
    NativeDepthAuditLock lock;
    NativeDepthAuditList data;
    data.epoch = g_depthAuditEpoch; data.resetObserved = true;
    native_depth_audit_write(list, kNativeDepthListGuid, data);
}
static void native_depth_audit_barriers(ID3D12GraphicsCommandList* list, UINT count,
                                       const D3D12_RESOURCE_BARRIER* barriers) {
    if (!native_depth_audit_active() || !barriers || !count) return;
    depth_capture_barriers(list,count,barriers);
    NativeDepthAuditCounts events;
    for (UINT i = 0; i < count; ++i) {
        const auto& b = barriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) ++events.aliasing;
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) continue;
        // pResource belongs to the API caller for the duration of this call. Copy its public
        // description now; never remember the pointer or attempt to inspect it after return.
        const auto d = b.Transition.pResource->GetDesc();
        if (!(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) continue;
        ++events.transitions;
        if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) ++events.split;
        if (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) ++events.subresources;
    }
    if (!events.transitions && !events.aliasing) return;
    NativeDepthAuditLock lock;
    NativeDepthAuditList data;
    native_depth_audit_read(list, kNativeDepthListGuid, data);
    data.events.transitions += events.transitions; data.events.split += events.split;
    data.events.subresources += events.subresources; data.events.aliasing += events.aliasing;
    native_depth_audit_write(list, kNativeDepthListGuid, data);
}
static void native_depth_audit_sum(NativeDepthAuditCounts& a, const NativeDepthAuditCounts& b) {
    a.binds += b.binds; a.clears += b.clears; a.transitions += b.transitions;
    a.split += b.split; a.subresources += b.subresources; a.aliasing += b.aliasing;
    a.bundles += b.bundles; a.renderPassDepth += b.renderPassDepth; a.enhanced += b.enhanced;
}
static void native_depth_audit_execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (!native_depth_audit_active() || !lists || !count) return;
    depth_begin_submission(queue,count,lists);
    ID3D12Device* dev = nullptr;
    if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), (void**)&dev))) return;
    {
        NativeDepthAuditLock lock;
        if (g_depthAuditDevice && native_identity(dev) == native_identity(g_depthAuditDevice)) {
            auto& total = native_identity(queue) == native_identity(g_depthAuditQueue) ? g_depthAuditHost : g_depthAuditOther;
            ++total.submissions;
            for (UINT i = 0; i < count; ++i) {
                ++total.lists;
                NativeDepthAuditList data;
                if (!lists[i] || !native_depth_audit_read(lists[i], kNativeDepthListGuid, data)) { ++total.unknown; continue; }
                if (!data.resetObserved || !data.closed) ++total.partial;
                native_depth_audit_sum(total.events, data.events);
            }
        }
    }
    dev->Release();
}
static void native_depth_audit_report(ID3D12Device* dev, UINT width, UINT height, bool requested) {
    if (!requested || !dev) return;
    NativeDepthAuditDevice device;
    NativeDepthAuditTotals host, other;
    UINT hooks; uint64_t errors, epoch;
    {
        NativeDepthAuditLock lock;
        const uint64_t now = now_ms();
        if (g_depthAuditReported && now - g_depthAuditReportAt < 5000) return;
        g_depthAuditReportAt = now; g_depthAuditReported = true;
        native_depth_audit_read(dev, kNativeDepthDeviceGuid, device);
        host = g_depthAuditHost; other = g_depthAuditOther;
        hooks = g_depthAuditHooks; errors = g_depthAuditMetadataErrors; epoch = g_depthAuditEpoch;
    }
    log("depth-audit: native depth %s; epoch %llu, hooks 0x%x/0x1ff, metadata write errors %llu, backbuffer %ux%u",
        g_nativeDepthSubmitted ? "certified snapshot active" : "fallback",
        (unsigned long long)epoch, hooks, (unsigned long long)errors, width, height);
    log("depth-audit: DSV creates %llu (null %llu, deny-SRV %llu, MSAA %llu, array/mips %llu), DSV copy calls %llu/%llu; last API resource %llux%u format %u, view %u, flags 0x%x",
        (unsigned long long)device.creates, (unsigned long long)device.nullCreates, (unsigned long long)device.denySrv,
        (unsigned long long)device.msaa, (unsigned long long)device.arrayOrMips, (unsigned long long)device.copies,
        (unsigned long long)device.simpleCopies, (unsigned long long)device.lastResource.Width,
        device.lastResource.Height, device.lastResource.Format, device.lastViewFormat, device.lastResource.Flags);
    log("depth-audit: presenting queue API submissions %llu, lists %llu (unknown %llu, partial %llu), DSV binds/clears %llu/%llu, depth barriers %llu (split %llu, per-subresource %llu), aliasing %llu, bundles %llu, depth render-pass %llu, enhanced calls %llu; other queues submissions %llu, binds/clears %llu/%llu",
        (unsigned long long)host.submissions, (unsigned long long)host.lists, (unsigned long long)host.unknown,
        (unsigned long long)host.partial, (unsigned long long)host.events.binds, (unsigned long long)host.events.clears,
        (unsigned long long)host.events.transitions, (unsigned long long)host.events.split,
        (unsigned long long)host.events.subresources, (unsigned long long)host.events.aliasing,
        (unsigned long long)host.events.bundles, (unsigned long long)host.events.renderPassDepth,
        (unsigned long long)host.events.enhanced, (unsigned long long)other.submissions,
        (unsigned long long)other.events.binds, (unsigned long long)other.events.clears);
    { DepthNativeLock lock;
      log("depth-audit: registered depth resources %zu, heap generations %zu; %s; certificates require engine scene/projection evidence",
          g_depthResources.size(),g_depthHeaps.size(),g_depthFallbackReason); }
}

using NativeDepthView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using NativeCopyDescriptors = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using NativeCopyDescriptorsSimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using NativeClose = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using NativeDepthTargets = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using NativeClearDepth = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
using NativeBundle = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
using NativeRenderPass = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
using NativeEnhancedBarrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*, UINT32, const D3D12_BARRIER_GROUP*);

static void STDMETHODCALLTYPE hkNativeDepthView(ID3D12Device* self, ID3D12Resource* resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    InflightGuard guard;
    native_note_hook(kSlotDepthView);
    ((NativeDepthView)g_nativeOrig[kSlotDepthView])(self, resource, desc, dest);
    if (!native_depth_audit_active()) return;
    depth_capture_view(self,resource,desc,dest);
    NativeDepthAuditLock lock;
    NativeDepthAuditDevice data;
    native_depth_audit_read(self, kNativeDepthDeviceGuid, data);
    ++data.creates;
    if (!resource) ++data.nullCreates;
    else {
        data.lastResource = resource->GetDesc();  // API-borrowed resource, description only
        data.lastViewFormat = desc ? desc->Format : data.lastResource.Format;
        if (data.lastResource.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ++data.denySrv;
        if (data.lastResource.SampleDesc.Count != 1) ++data.msaa;
        if (data.lastResource.DepthOrArraySize != 1 || data.lastResource.MipLevels != 1) ++data.arrayOrMips;
    }
    native_depth_audit_write(self, kNativeDepthDeviceGuid, data);
}
static void native_depth_audit_copy(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, bool simple) {
    if (!native_depth_audit_active() || type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return;
    NativeDepthAuditLock lock;
    NativeDepthAuditDevice data;
    native_depth_audit_read(dev, kNativeDepthDeviceGuid, data);
    if (simple) ++data.simpleCopies; else ++data.copies;
    native_depth_audit_write(dev, kNativeDepthDeviceGuid, data);
}
static void STDMETHODCALLTYPE hkNativeCopyDescriptors(ID3D12Device* self, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* d,
    const UINT* ds, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* s, const UINT* ss, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    InflightGuard guard;
    native_note_hook(kSlotCopyDescriptors);
    ((NativeCopyDescriptors)g_nativeOrig[kSlotCopyDescriptors])(self, nd, d, ds, ns, s, ss, type);
    native_depth_audit_copy(self, type, false);
    depth_capture_copies(self,nd,d,ds,ns,s,ss,type);
}
static void STDMETHODCALLTYPE hkNativeCopyDescriptorsSimple(ID3D12Device* self, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE d,
    D3D12_CPU_DESCRIPTOR_HANDLE s, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    InflightGuard guard;
    native_note_hook(kSlotCopyDescriptorsSimple);
    ((NativeCopyDescriptorsSimple)g_nativeOrig[kSlotCopyDescriptorsSimple])(self, count, d, s, type);
    native_depth_audit_copy(self, type, true);
    depth_capture_copies(self,1,&d,&count,1,&s,&count,type);
}
static HRESULT STDMETHODCALLTYPE hkNativeClose(ID3D12GraphicsCommandList* self) {
    InflightGuard guard;
    native_note_hook(kSlotClose);
    const HRESULT hr = ((NativeClose)g_nativeOrig[kSlotClose])(self);
    if (SUCCEEDED(hr) && native_depth_audit_active()) {
        depth_capture_close(self);
        NativeDepthAuditLock lock;
        NativeDepthAuditList data;
        native_depth_audit_read(self, kNativeDepthListGuid, data);
        data.closed = true;
        native_depth_audit_write(self, kNativeDepthListGuid, data);
    }
    return hr;
}
// These counters deliberately treat every handle as unresolved. Never read handle.ptr.
enum NativeDepthAuditEvent { kAuditBind, kAuditClear, kAuditBundle, kAuditRenderPass, kAuditEnhanced };
static void native_depth_audit_event(ID3D12GraphicsCommandList* list, NativeDepthAuditEvent event) {
    if (!native_depth_audit_active()) return;
    if (event == kAuditBundle || event == kAuditRenderPass || event == kAuditEnhanced) depth_capture_invalid(list);
    NativeDepthAuditLock lock;
    NativeDepthAuditList data;
    native_depth_audit_read(list, kNativeDepthListGuid, data);
    switch (event) {
        case kAuditBind: ++data.events.binds; break;
        case kAuditClear: ++data.events.clears; break;
        case kAuditBundle: ++data.events.bundles; break;
        case kAuditRenderPass: ++data.events.renderPassDepth; break;
        case kAuditEnhanced: ++data.events.enhanced; break;
    }
    native_depth_audit_write(list, kNativeDepthListGuid, data);
}
static void STDMETHODCALLTYPE hkNativeDepthTargets(ID3D12GraphicsCommandList* self, UINT count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL range, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    InflightGuard guard;
    native_note_hook(kSlotDepthTargets);
    ((NativeDepthTargets)g_nativeOrig[kSlotDepthTargets])(self, count, targets, range, depth);
    if (depth && depth->ptr) native_depth_audit_event(self, kAuditBind);
    depth_capture_bind(self,depth);
}
static void STDMETHODCALLTYPE hkNativeClearDepth(ID3D12GraphicsCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE depth,
    D3D12_CLEAR_FLAGS flags, FLOAT value, UINT8 stencil, UINT count, const D3D12_RECT* rects) {
    InflightGuard guard;
    native_note_hook(kSlotClearDepth);
    ((NativeClearDepth)g_nativeOrig[kSlotClearDepth])(self, depth, flags, value, stencil, count, rects);
    native_depth_audit_event(self, kAuditClear);
    depth_capture_clear(self,depth,flags,count);
}
static void STDMETHODCALLTYPE hkNativeBundle(ID3D12GraphicsCommandList* self, ID3D12GraphicsCommandList* bundle) {
    InflightGuard guard;
    native_note_hook(kSlotBundle);
    ((NativeBundle)g_nativeOrig[kSlotBundle])(self, bundle);
    native_depth_audit_event(self, kAuditBundle);  // bundle contents are not flattened
}
static void STDMETHODCALLTYPE hkNativeRenderPass(ID3D12GraphicsCommandList4* self, UINT count,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS flags) {
    InflightGuard guard;
    native_note_hook(kSlotRenderPass);
    ((NativeRenderPass)g_nativeOrig[kSlotRenderPass])(self, count, rt, ds, flags);
    if (ds) native_depth_audit_event(self, kAuditRenderPass);
}
static void STDMETHODCALLTYPE hkNativeEnhancedBarrier(ID3D12GraphicsCommandList7* self, UINT32 count, const D3D12_BARRIER_GROUP* groups) {
    InflightGuard guard;
    native_note_hook(kSlotEnhancedBarrier);
    ((NativeEnhancedBarrier)g_nativeOrig[kSlotEnhancedBarrier])(self, count, groups);
    if (count) native_depth_audit_event(self, kAuditEnhanced);  // no legacy-state conversion
}
