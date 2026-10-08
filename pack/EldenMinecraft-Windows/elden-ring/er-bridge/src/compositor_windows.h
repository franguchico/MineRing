// Native backend, included inside namespace mb by compositor.cpp only.
// These indices are COM interface method slots from the Windows SDK, never object offsets.
#pragma once

static const GUID kNativeQueueGuid = {0x7bf342d9, 0x5e2c, 0x4f6b, {0x98, 0x8a, 0x9d, 0x47, 0x81, 0xc6, 0x42, 0x10}};
static const GUID kNativeListGuid = {0x7bf342d9, 0x5e2c, 0x4f6b, {0x98, 0x8a, 0x9d, 0x47, 0x81, 0xc6, 0x42, 0x11}};
static const GUID kNativeProbeGuid = {0x7bf342d9, 0x5e2c, 0x4f6b, {0x98, 0x8a, 0x9d, 0x47, 0x81, 0xc6, 0x42, 0x12}};
static void* g_nativeAddresses[kNativeHookCount] = {};
static bool g_nativeHookMade[kNativeHookCount] = {};
static bool g_nativeHookEnabled[kNativeHookCount] = {};
static bool g_nativeAuditHooksRequested = false;
static std::atomic<bool> g_probeActive{false};
static std::atomic<bool> g_nativePerfEnabled{false};
static std::atomic<uint64_t> g_nativeHookCalls[kNativeHookCount]{};
static std::atomic<uint64_t> g_nativeProbeCalls{0}, g_nativeMetadataCalls{0};
static std::atomic<uint64_t> g_nativeCompositeCalls{0}, g_nativeCompositeTicks{0}, g_nativeCompositeMax{0};
static void native_begin_composite_profile(LARGE_INTEGER* start) {
    if (g_nativePerfEnabled.load(std::memory_order_relaxed)) QueryPerformanceCounter(start);
}
static void native_end_composite_profile(const LARGE_INTEGER& start) {
    if (!start.QuadPart) return;
    LARGE_INTEGER end; QueryPerformanceCounter(&end);
    uint64_t elapsed = (uint64_t)(end.QuadPart - start.QuadPart);
    g_nativeCompositeCalls.fetch_add(1, std::memory_order_relaxed);
    g_nativeCompositeTicks.fetch_add(elapsed, std::memory_order_relaxed);
    uint64_t high = g_nativeCompositeMax.load(std::memory_order_relaxed);
    while (elapsed > high && !g_nativeCompositeMax.compare_exchange_weak(high, elapsed, std::memory_order_relaxed)) {}
}
static void native_note_hook(int slot) {
    if (g_nativePerfEnabled.load(std::memory_order_relaxed))
        g_nativeHookCalls[slot].fetch_add(1, std::memory_order_relaxed);
}
static void native_note_probe(bool metadata = false) {
    if (g_nativePerfEnabled.load(std::memory_order_relaxed))
        (metadata ? g_nativeMetadataCalls : g_nativeProbeCalls).fetch_add(1, std::memory_order_relaxed);
}
static SRWLOCK g_probeLock = SRWLOCK_INIT;
static uint64_t g_probeEpoch = 0;
static void* g_probeIdentity = nullptr;  // borrowed COM identity, not a retained swapchain
static ID3D12Resource* g_probeBuffers[kMaxBuffers] = {};  // identities only; no resize-blocking refs
static UINT g_probeCount = 0;
static ID3D12CommandQueue* g_observedQueue = nullptr;  // owned until adoption/invalidation
static bool g_probeAmbiguous = false;

struct NativeListTag {
    uint64_t epoch;
    UINT presentMask;
};

static void* native_identity(IUnknown* object) {
    IUnknown* identity = nullptr;
    if (!object || FAILED(object->QueryInterface(__uuidof(IUnknown), (void**)&identity))) return nullptr;
    void* value = identity;
    identity->Release();
    return value;
}

#include "compositor_depth_audit.h"

static bool native_queue_valid(ID3D12CommandQueue* q, ID3D12Device* dev) {
    if (!q || !dev || dev->GetNodeCount() != 1) return false;
    D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
    if (d.Type != D3D12_COMMAND_LIST_TYPE_DIRECT || d.NodeMask > 1) return false;
    ID3D12Device* qdev = nullptr;
    if (FAILED(q->GetDevice(__uuidof(ID3D12Device), (void**)&qdev))) return false;
    bool same = native_identity(qdev) == native_identity(dev);
    qdev->Release();
    return same;
}

static void native_capture_queue(IUnknown* device, IDXGISwapChain* sc) {
    if (!device || !sc || InterlockedCompareExchange(&g_nativeStopping, 0, 0)) return;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&q))) return;  // D3D11
    if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        HRESULT hr = sc->SetPrivateDataInterface(kNativeQueueGuid, q);  // DXGI owns an AddRef
        if (SUCCEEDED(hr)) log("compositor: captured DXGI creation queue %p for swapchain %p", (void*)q, (void*)sc);
    }
    q->Release();
}

// Must hold g_probeLock exclusively. No refs to swapchain/backbuffers survive this call.
static void native_clear_probe() {
    g_probeActive.store(false, std::memory_order_release);
    ++g_probeEpoch;
    g_probeIdentity = nullptr;
    g_probeCount = 0;
    memset(g_probeBuffers, 0, sizeof(g_probeBuffers));
    safe_release(g_observedQueue);
    g_probeAmbiguous = false;
}

static void native_cancel_probe() {
    if (!g_probeActive.load(std::memory_order_acquire)) return;
    AcquireSRWLockExclusive(&g_probeLock);
    native_clear_probe();
    ReleaseSRWLockExclusive(&g_probeLock);
}

static ID3D12CommandQueue* native_queue_for(IDXGISwapChain* sc, ID3D12Device* dev) {
    ID3D12CommandQueue* queue = nullptr;
    UINT bytes = sizeof(queue);
    // GetPrivateData adds a reference for an interface set by SetPrivateDataInterface.
    if (SUCCEEDED(sc->GetPrivateData(kNativeQueueGuid, &bytes, &queue)) && queue) {
        if (bytes == sizeof(queue) && native_queue_valid(queue, dev)) {
            AcquireSRWLockExclusive(&g_probeLock);
            if (native_identity(sc) == g_probeIdentity) native_clear_probe();
            ReleaseSRWLockExclusive(&g_probeLock);
            return queue;
        }
        safe_release(queue);
        return nullptr;  // explicit but invalid association: never replace it with a guess
    }

    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d)) || !d.BufferCount || d.BufferCount > kMaxBuffers) return nullptr;
    ID3D12Resource* buffers[kMaxBuffers] = {};
    bool ok = true;
    for (UINT i = 0; i < d.BufferCount; ++i) {
        if (FAILED(sc->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&buffers[i]))) { ok = false; break; }
    }
    if (!ok) {
        for (auto& b : buffers) safe_release(b);
        return nullptr;
    }
    void* identity = native_identity(sc);
    AcquireSRWLockExclusive(&g_probeLock);
    uint64_t tag = 0;
    bytes = sizeof(tag);
    bool tagged = SUCCEEDED(sc->GetPrivateData(kNativeProbeGuid, &bytes, &tag)) && bytes == sizeof(tag);
    bool changed = identity != g_probeIdentity || !tagged || tag != g_probeEpoch || g_probeCount != d.BufferCount;
    for (UINT i = 0; i < d.BufferCount; ++i) changed |= buffers[i] != g_probeBuffers[i];
    if (changed) {
        native_clear_probe();
        g_probeIdentity = identity;
        g_probeCount = d.BufferCount;
        memcpy(g_probeBuffers, buffers, sizeof(buffers));
        sc->SetPrivateData(kNativeProbeGuid, sizeof(g_probeEpoch), &g_probeEpoch);
        g_probeActive.store(true, std::memory_order_release);
        log("compositor: waiting for a submitted PRESENT transition of this swapchain's backbuffer");
    } else if (g_observedQueue && !g_probeAmbiguous && native_queue_valid(g_observedQueue, dev)) {
        if (SUCCEEDED(sc->SetPrivateDataInterface(kNativeQueueGuid, g_observedQueue))) {
            queue = g_observedQueue;
            queue->AddRef();
            log("compositor: adopted backbuffer submission queue %p (late injection)", (void*)queue);
            native_clear_probe();
        }
    }
    ReleaseSRWLockExclusive(&g_probeLock);
    for (auto& b : buffers) safe_release(b);
    return queue;
}

using NativeCreate = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using NativeCreateHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using NativeCreateCore = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
    IDXGIOutput*, IDXGISwapChain1**);
using NativeCreateComposition = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
    IDXGIOutput*, IDXGISwapChain1**);
using NativeExecute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using NativeBarrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using NativeReset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);

static HRESULT STDMETHODCALLTYPE hkNativeCreate(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                                 IDXGISwapChain** out) {
    InflightGuard guard;
    HRESULT hr = ((NativeCreate)g_nativeOrig[kSlotCreate])(self, device, desc, out);
    if (SUCCEEDED(hr) && out) native_capture_queue(device, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkNativeCreateHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full, IDXGIOutput* restrictTo,
    IDXGISwapChain1** out) {
    InflightGuard guard;
    HRESULT hr = ((NativeCreateHwnd)g_nativeOrig[kSlotCreateHwnd])(self, device, hwnd, desc, full, restrictTo, out);
    if (SUCCEEDED(hr) && out) native_capture_queue(device, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkNativeCreateCore(IDXGIFactory2* self, IUnknown* device, IUnknown* window,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictTo, IDXGISwapChain1** out) {
    InflightGuard guard;
    HRESULT hr = ((NativeCreateCore)g_nativeOrig[kSlotCreateCore])(self, device, window, desc, restrictTo, out);
    if (SUCCEEDED(hr) && out) native_capture_queue(device, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkNativeCreateComposition(IDXGIFactory2* self, IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictTo, IDXGISwapChain1** out) {
    InflightGuard guard;
    HRESULT hr = ((NativeCreateComposition)g_nativeOrig[kSlotCreateComposition])(self, device, desc, restrictTo, out);
    if (SUCCEEDED(hr) && out) native_capture_queue(device, *out);
    return hr;
}

static void STDMETHODCALLTYPE hkNativeBarrier(ID3D12GraphicsCommandList* self, UINT count,
                                               const D3D12_RESOURCE_BARRIER* barriers) {
    InflightGuard guard;
    native_note_hook(kSlotBarrier);
    ((NativeBarrier)g_nativeOrig[kSlotBarrier])(self, count, barriers);
    native_depth_audit_barriers(self, count, barriers);
    if (!g_probeActive.load(std::memory_order_acquire) || g_nativeCompositorCommands) return;
    if (InterlockedCompareExchange(&g_nativeStopping, 0, 0)) return;
    native_note_probe();
    AcquireSRWLockShared(&g_probeLock);
    if (g_probeCount && barriers) {
        NativeListTag tag = {};
        UINT bytes = sizeof(tag);
        native_note_probe(true);
        if (FAILED(self->GetPrivateData(kNativeListGuid, &bytes, &tag)) || bytes != sizeof(tag) || tag.epoch != g_probeEpoch)
            tag = {g_probeEpoch, 0};
        bool touched = false;
        for (UINT n = 0; n < count; ++n) {
            const D3D12_RESOURCE_BARRIER& b = barriers[n];
            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
            for (UINT i = 0; i < g_probeCount; ++i) {
                if (b.Transition.pResource != g_probeBuffers[i]) continue;
                touched = true;
                tag.presentMask &= ~(1u << i);
                if (b.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                    (b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || b.Transition.Subresource == 0) &&
                    b.Transition.StateAfter == D3D12_RESOURCE_STATE_PRESENT) tag.presentMask |= 1u << i;
            }
        }
        if (touched) {
            native_note_probe(true);
            self->SetPrivateData(kNativeListGuid, sizeof(tag), &tag);
        }
    }
    ReleaseSRWLockShared(&g_probeLock);
}

static HRESULT STDMETHODCALLTYPE hkNativeReset(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* alloc,
                                                ID3D12PipelineState* pso) {
    InflightGuard guard;
    native_note_hook(kSlotReset);
    HRESULT hr = ((NativeReset)g_nativeOrig[kSlotReset])(self, alloc, pso);
    if (SUCCEEDED(hr)) {
        if (g_probeActive.load(std::memory_order_acquire) && !g_nativeCompositorCommands) {
            native_note_probe(true);
            self->SetPrivateData(kNativeListGuid, 0, nullptr);
        }
        native_depth_audit_reset_list(self);
    }
    return hr;
}

static void STDMETHODCALLTYPE hkNativeExecute(ID3D12CommandQueue* self, UINT count, ID3D12CommandList* const* lists) {
    InflightGuard guard;
    native_note_hook(kSlotExecute);
    // Snapshot API observations before submission. Reset is legal immediately after
    // ExecuteCommandLists returns; reading list metadata afterwards could see the next list.
    // This audit does not claim GPU completion, successful execution or resource states.
    native_depth_audit_execute(self, count, lists);
    // Queue discovery must snapshot before submission too: Reset may erase/reuse this
    // private data as soon as the original ExecuteCommandLists returns. Never keep the
    // list pointers or hold our lock across the driver's submission.
    bool match = false;
    uint64_t epoch = 0;
    if (g_probeActive.load(std::memory_order_acquire) && !g_nativeCompositorCommands &&
        !InterlockedCompareExchange(&g_nativeStopping, 0, 0)) {
        native_note_probe();
        AcquireSRWLockShared(&g_probeLock);
        epoch = g_probeEpoch;
        if (g_probeCount && lists) {
            for (UINT i = 0; i < count; ++i) {
                if (!lists[i]) continue;
                NativeListTag tag = {};
                UINT bytes = sizeof(tag);
                native_note_probe(true);
                if (SUCCEEDED(lists[i]->GetPrivateData(kNativeListGuid, &bytes, &tag)) && bytes == sizeof(tag) &&
                    tag.epoch == g_probeEpoch && tag.presentMask) match = true;
            }
        }
        ReleaseSRWLockShared(&g_probeLock);
    }
    ((NativeExecute)g_nativeOrig[kSlotExecute])(self, count, lists);
    if (!match || InterlockedCompareExchange(&g_nativeStopping, 0, 0)) return;
    AcquireSRWLockExclusive(&g_probeLock);
    if (g_probeCount && epoch == g_probeEpoch && self->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        if (!g_observedQueue) { self->AddRef(); g_observedQueue = self; }
        else if (native_identity(self) != native_identity(g_observedQueue) && !g_probeAmbiguous) {
            g_probeAmbiguous = true;
            log("compositor: conflicting backbuffer submission queues; native composition remains disabled");
        }
    }
    ReleaseSRWLockExclusive(&g_probeLock);
}

static void native_before_resize(IDXGISwapChain* sc) {
    if ((IDXGISwapChain3*)sc == g_sc) native_depth_audit_reset();
    AcquireSRWLockExclusive(&g_probeLock);
    if (native_identity(sc) == g_probeIdentity) native_clear_probe();
    ReleaseSRWLockExclusive(&g_probeLock);
}

static void native_after_resize(IDXGISwapChain* sc, HRESULT hr, UINT count, IUnknown* const* queues) {
    if (FAILED(hr) || !queues) return;
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (!count && SUCCEEDED(sc->GetDesc(&desc))) count = desc.BufferCount;
    bool same = count && count <= kMaxBuffers && queues[0];
    for (UINT i = 1; same && i < count; ++i) same = native_identity(queues[i]) == native_identity(queues[0]);
    bool ours = (IDXGISwapChain3*)sc == g_sc;
    if (!same) {
        sc->SetPrivateDataInterface(kNativeQueueGuid, nullptr);
        if (ours) g_failed = true;
        log("compositor: ResizeBuffers1 selected multiple presentation queues; composition disabled for this swapchain");
        return;
    }
    native_capture_queue(queues[0], sc);
    if (ours) {
        ID3D12CommandQueue* q = native_queue_for(sc, g_dev);
        if (!q) { g_failed = true; return; }
        safe_release(g_queue);
        g_queue = q;
        native_depth_audit_target(g_dev, q);
    }
}

// Use a private, never shown HWND on the calling worker thread. Its window procedure lives
// in user32, and both the HWND and class are destroyed on that same thread before returning.
// Nothing is created on or attached to the game's window, and no game UI is manipulated.
static bool native_discover(void** addresses) {
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    IDXGISwapChain3* sc3 = nullptr;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    wchar_t className[80];
    wsprintfW(className, L"ERMC_D3D12_Probe_%lu_%lu", GetCurrentProcessId(), GetCurrentThreadId());
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    ATOM atom = RegisterClassW(&wc);
    HWND hwnd = atom ? CreateWindowExW(0, className, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, instance, nullptr) : nullptr;
    bool ok = false;
    do {
        if (!hwnd || FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) break;
        // Try hardware adapters in DXGI order. WARP also allows the synthetic harness to run
        // on machines without a usable hardware D3D12 device; wrappers/other runtimes may
        // expose different method implementations and need additional runtime validation.
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            if (!adapter) break;
            HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
            safe_release(adapter);
            if (SUCCEEDED(hr)) break;
        }
        if (!dev) {
            if (FAILED(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), (void**)&adapter))) break;
            HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
            safe_release(adapter);
            if (FAILED(hr)) break;
        }
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&queue))) break;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&alloc))) break;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                        __uuidof(ID3D12GraphicsCommandList), (void**)&list))) break;
        if (FAILED(list->Close())) break;
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = sd.Height = 64;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc1))) break;
        if (FAILED(sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) break;
        void** sv = *(void***)sc3;
        for (int i = 0; i < kHookCount; ++i) addresses[i] = sv[kVtblIndex[i]];
        void** fv = *(void***)factory;
        addresses[kSlotCreate] = fv[10];
        addresses[kSlotCreateHwnd] = fv[15];
        addresses[kSlotCreateCore] = fv[16];
        addresses[kSlotCreateComposition] = fv[24];
        addresses[kSlotExecute] = (*(void***)queue)[10];
        addresses[kSlotBarrier] = (*(void***)list)[26];
        addresses[kSlotReset] = (*(void***)list)[10];
        // Optional audit slots follow the public Windows SDK COM interfaces. No engine
        // offsets or descriptor-heap bytes are accessed. Unsupported interfaces stay null.
        void** dv = *(void***)dev;
        void** lv = *(void***)list;
        addresses[kSlotDepthView] = dv[21];
        addresses[kSlotCopyDescriptors] = dv[23];
        addresses[kSlotCopyDescriptorsSimple] = dv[24];
        addresses[kSlotClose] = lv[9];
        addresses[kSlotDepthTargets] = lv[46];
        addresses[kSlotClearDepth] = lv[47];
        addresses[kSlotBundle] = lv[27];
        ID3D12GraphicsCommandList4* list4 = nullptr;
        if (SUCCEEDED(list->QueryInterface(__uuidof(ID3D12GraphicsCommandList4), (void**)&list4))) {
            addresses[kSlotRenderPass] = (*(void***)list4)[68];
            list4->Release();
        }
        ID3D12GraphicsCommandList7* list7 = nullptr;
        if (SUCCEEDED(list->QueryInterface(__uuidof(ID3D12GraphicsCommandList7), (void**)&list7))) {
            addresses[kSlotEnhancedBarrier] = (*(void***)list7)[80];
            list7->Release();
        }
        ok = true;
    } while (false);
    safe_release(sc3);
    safe_release(sc1);
    safe_release(list);
    safe_release(alloc);
    safe_release(queue);
    safe_release(dev);
    safe_release(adapter);
    safe_release(factory);
    if (hwnd) DestroyWindow(hwnd);
    if (atom) UnregisterClassW(className, instance);
    return ok;
}

static bool native_init() {
    if (g_nativeHooked) return true;
    ++g_initTries;
    if (!native_discover(g_nativeAddresses)) {
        if (g_initTries == 1) log("compositor: native D3D12 hook discovery unavailable; will retry");
        return false;
    }
    void* detours[kNativeHookCount] = {(void*)&hkPresent, (void*)&hkResizeBuffers, (void*)&hkPresent1,
        (void*)&hkResizeBuffers1, (void*)&hkNativeCreate, (void*)&hkNativeCreateHwnd, (void*)&hkNativeCreateCore,
        (void*)&hkNativeCreateComposition, (void*)&hkNativeExecute, (void*)&hkNativeBarrier, (void*)&hkNativeReset,
        (void*)&hkNativeDepthView, (void*)&hkNativeCopyDescriptors, (void*)&hkNativeCopyDescriptorsSimple,
        (void*)&hkNativeClose, (void*)&hkNativeDepthTargets, (void*)&hkNativeClearDepth, (void*)&hkNativeBundle,
        (void*)&hkNativeRenderPass, (void*)&hkNativeEnhancedBarrier};
    native_depth_audit_begin();
    int made = 0;
    MH_STATUS status = MH_OK;
    for (; made < kNativeRequiredHookCount; ++made) {
        status = MH_CreateHook(g_nativeAddresses[made], detours[made], &g_nativeOrig[made]);
        if (status != MH_OK) break;
        g_nativeHookMade[made] = true;
    }
    if (made == kNativeRequiredHookCount) {
        InterlockedExchange(&g_nativeStopping, 0);
        for (int i = 0; i < made && status == MH_OK; ++i) status = MH_QueueEnableHook(g_nativeAddresses[i]);
        if (status == MH_OK) status = MH_ApplyQueued();
    }
    if (status != MH_OK) {
        InterlockedExchange(&g_nativeStopping, 1);
        for (int i = 0; i < made; ++i) MH_DisableHook(g_nativeAddresses[i]);
        // Keep trampolines alive if any already-enabled detour is still returning.
        for (int i = 0; i < 400 && g_inflight > 0; ++i) Sleep(5);
        if (g_inflight == 0) for (int i = 0; i < made; ++i) {
            MH_RemoveHook(g_nativeAddresses[i]);
            g_nativeHookMade[i] = false;
        }
        log("compositor: native MinHook installation failed at slot %d: %s", made, MH_StatusToString(status));
        return false;
    }
    g_nativeHooked = true;
    for (int i = 0; i < kNativeRequiredHookCount; ++i) g_nativeHookEnabled[i] = true;
    // High-frequency descriptor/DSV/list hooks remain unpatched during ordinary play.
    // The worker installs them only for an explicit audit, never from a render detour.
    log("compositor: native Present/Present1/Resize hooks installed; DXGI creation and backbuffer submission capture enabled");
    return true;
}

static void native_update_audit_hooks() {
    bool requested = g_depthAuditEnabled.load(std::memory_order_relaxed);
    if (requested == g_nativeAuditHooksRequested) return;
    g_nativeAuditHooksRequested = requested;  // retry unavailable slots only on a new opt-in
    void* detours[] = {(void*)&hkNativeDepthView, (void*)&hkNativeCopyDescriptors,
        (void*)&hkNativeCopyDescriptorsSimple, (void*)&hkNativeClose, (void*)&hkNativeDepthTargets,
        (void*)&hkNativeClearDepth, (void*)&hkNativeBundle, (void*)&hkNativeRenderPass,
        (void*)&hkNativeEnhancedBarrier};
    bool queued[kNativeHookCount] = {};
    for (int i = kNativeRequiredHookCount; i < kNativeHookCount; ++i) {
        if (!g_nativeAddresses[i]) continue;
        if (requested && !g_nativeHookMade[i]) {
            MH_STATUS made = MH_CreateHook(g_nativeAddresses[i], detours[i - kNativeRequiredHookCount], &g_nativeOrig[i]);
            if (made != MH_OK) {
                log("depth-audit: optional hook slot %d unavailable: %s", i, MH_StatusToString(made));
                continue;
            }
            g_nativeHookMade[i] = true;  // keep trampoline until all detours drain at shutdown
        }
        if (!g_nativeHookMade[i] || g_nativeHookEnabled[i] == requested) continue;
        MH_STATUS status = requested ? MH_QueueEnableHook(g_nativeAddresses[i]) : MH_QueueDisableHook(g_nativeAddresses[i]);
        if (status == MH_OK) queued[i] = true;
        else log("depth-audit: optional hook slot %d toggle failed: %s", i, MH_StatusToString(status));
    }
    MH_STATUS applied = MH_ApplyQueued();
    UINT coverage = 0;
    for (int i = kNativeRequiredHookCount; i < kNativeHookCount; ++i) {
        if (queued[i] && applied == MH_OK) g_nativeHookEnabled[i] = requested;
        else if (queued[i]) {
            // ApplyQueued can fail after patching an earlier slot. Reconcile each
            // target explicitly so neither shutdown nor coverage uses a guessed state.
            MH_STATUS fixed = requested ? MH_EnableHook(g_nativeAddresses[i]) : MH_DisableHook(g_nativeAddresses[i]);
            if (fixed == MH_OK || fixed == (requested ? MH_ERROR_ENABLED : MH_ERROR_DISABLED))
                g_nativeHookEnabled[i] = requested;
            else log("depth-audit: optional hook slot %d recovery failed: %s", i, MH_StatusToString(fixed));
        }
        if (g_nativeHookEnabled[i]) coverage |= 1u << (i - kNativeRequiredHookCount);
    }
    { NativeDepthAuditLock lock; g_depthAuditHooks = coverage; }
    log("depth-audit: optional hooks %s; coverage 0x%x/0x1ff (%s); host-depth stays disabled",
        requested ? "requested" : "idle", coverage, MH_StatusToString(applied));
}

// Worker-only maintenance and opt-in counters: one bounded line per five seconds.
static void native_poll_diagnostics() {
    if (InterlockedCompareExchange(&g_nativeStopping, 0, 0)) return;
    native_update_audit_hooks();
    bool enabled = (shm_header()->debugFlags & kNativePerfDebugFlag) != 0;
    bool wasEnabled = g_nativePerfEnabled.load(std::memory_order_relaxed);
    static uint64_t reportAt = 0;
    uint64_t now = GetTickCount64();
    if (enabled != wasEnabled) {
        g_nativePerfEnabled.store(enabled, std::memory_order_relaxed);
        for (auto& calls : g_nativeHookCalls) calls.exchange(0, std::memory_order_relaxed);
        g_nativeProbeCalls.exchange(0, std::memory_order_relaxed);
        g_nativeMetadataCalls.exchange(0, std::memory_order_relaxed);
        g_nativeCompositeCalls.exchange(0, std::memory_order_relaxed);
        g_nativeCompositeTicks.exchange(0, std::memory_order_relaxed);
        g_nativeCompositeMax.exchange(0, std::memory_order_relaxed);
        reportAt = now;
    }
    if (!enabled || now - reportAt < 5000) return;
    uint64_t calls[kNativeHookCount] = {};
    for (int i = 0; i < kNativeHookCount; ++i) calls[i] = g_nativeHookCalls[i].exchange(0, std::memory_order_relaxed);
    uint64_t optional = 0;
    for (int i = kNativeRequiredHookCount; i < kNativeHookCount; ++i) optional += calls[i];
    uint64_t samples = g_nativeCompositeCalls.exchange(0, std::memory_order_relaxed);
    uint64_t ticks = g_nativeCompositeTicks.exchange(0, std::memory_order_relaxed);
    uint64_t high = g_nativeCompositeMax.exchange(0, std::memory_order_relaxed);
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    double scale = 1000.0 / frequency.QuadPart;
    log("native-perf: %llu ms, Present/Present1 %llu/%llu, Barrier/Reset/Execute %llu/%llu/%llu, optional calls %llu, probe lock entries %llu, probe metadata API calls %llu, bridge composite avg/max %.3f/%.3f ms (excludes original Present), mcPid %u, mcHeartbeat %llu",
        (unsigned long long)(now - reportAt), (unsigned long long)calls[kSlotPresent], (unsigned long long)calls[kSlotPresent1],
        (unsigned long long)calls[kSlotBarrier], (unsigned long long)calls[kSlotReset], (unsigned long long)calls[kSlotExecute],
        (unsigned long long)optional, (unsigned long long)g_nativeProbeCalls.exchange(0, std::memory_order_relaxed),
        (unsigned long long)g_nativeMetadataCalls.exchange(0, std::memory_order_relaxed),
        samples ? ticks * scale / samples : 0.0, high * scale, shm_header()->mcPid,
        (unsigned long long)shm_header()->mcHeartbeat);
    reportAt = now;
}

static void native_detach() {
    InterlockedExchange(&g_nativeStopping, 1);
    if (!g_nativeHooked) return;
    for (int i = 0; i < kNativeHookCount; ++i)
        if (g_nativeHookMade[i]) MH_QueueDisableHook(g_nativeAddresses[i]);
    MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK && status != MH_ERROR_NOT_INITIALIZED)
        log("compositor: disabling native hooks failed: %s", MH_StatusToString(status));
}

// Called after the core has disabled hooks and waited for InflightGuard to drain.
static void native_shutdown() {
    for (int i = 0; i < kNativeHookCount; ++i) {
        if (g_nativeHookMade[i]) MH_RemoveHook(g_nativeAddresses[i]);  // may already be uninitialized by core.cpp
        g_nativeHookMade[i] = false;
        g_nativeHookEnabled[i] = false;
    }
    AcquireSRWLockExclusive(&g_probeLock);
    native_clear_probe();
    ReleaseSRWLockExclusive(&g_probeLock);
    native_depth_audit_shutdown();
    g_nativeAuditHooksRequested = false;
    g_nativeHooked = false;
}
