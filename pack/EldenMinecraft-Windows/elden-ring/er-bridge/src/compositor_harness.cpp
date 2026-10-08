// Standalone synthetic executable. Compile THIS file (it includes compositor.cpp), not
// core.cpp/game.cpp/shm.cpp/paths.cpp. Link MinHook, user32, d3d12, dxgi and d3dcompiler.
// It creates only a private hidden window, a D3D12 device and a temporary frames.shm.
// No game process, game files, input injection or game-memory access is involved.
#define ERMC_NATIVE_WINDOWS 1
#define ERMC_COMPOSITOR_HARNESS 1
#ifdef ERMC_COMPOSITOR_SOURCE
#include ERMC_COMPOSITOR_SOURCE
#else
#include "compositor.cpp"
#endif
#include <d3d12sdklayers.h>
#include <stdio.h>
#include <stdarg.h>
#include <vector>
#include <thread>

namespace mb {
volatile LONG g_inflight = 0;
static ErmcHeader testHeader = {};
static ErmcGameState testState = {};
static ErmcControl testControl = {};
static std::wstring testDirectory;
static std::atomic<unsigned> mappingCalls{0};
static HANDLE mappingGate = nullptr, mappingEntered = nullptr;
void log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
}
static uint64_t noticeTestNow = 0;
uint64_t now_ms() { return noticeTestNow ? noticeTestNow : GetTickCount64(); }
ErmcHeader* shm_header() { return &testHeader; }
ErmcGameState* shm_state() { return &testState; }
bool control_snapshot(ErmcControl* out) { *out = testControl; return testControl.flags != 0; }
HWND game_hwnd() { return nullptr; }
std::wstring ipc_path(const wchar_t* name) { return testDirectory + L"\\" + name; }
bool ipc_ensure_dir() {
    mappingCalls.fetch_add(1);
    if (mappingGate) {
        SetEvent(mappingEntered);
        WaitForSingleObject(mappingGate, 2000); // isolated simulated disk I/O, never production
    }
    return CreateDirectoryW(testDirectory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
}

static int failures = 0;
static void check(bool ok, const char* name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
static void require(HRESULT hr, const char* what) {
    if (FAILED(hr)) { printf("FATAL %s: 0x%08lx\n", what, (unsigned long)hr); ExitProcess(2); }
}

static void test_consumer_liveness() {
    mb::McHeartbeatWatch watch;
    check(!watch.update(0, 0, 4000, 10), "missing MC PID rejects a persisted heartbeat");
    check(!watch.update(7, 11, 4000, 20) && !watch.update(7, 11, 4000, 500),
          "persisted MC session needs witnessed heartbeat progress");
    check(watch.update(7, 11, 4001, 501) && watch.update(7, 11, 4001, 1501),
          "progressed MC heartbeat stays live for the bounded grace period");
    check(!watch.update(7, 11, 4001, 1502) && watch.update(7, 11, 4002, 1503),
          "stopped heartbeat expires and fresh progress resumes work");
    check(!watch.update(8, 11, 4002, 1504) && !watch.update(8, 12, 4003, 1505),
          "PID or session-start change invalidates the old consumer");
    check(!watch.update(8, 12, 0, 1506) && watch.update(8, 12, 1, 1507),
          "heartbeat reset cannot validate stale work; new progress is accepted");
}

static mb::NativeExecute submittedExecute = nullptr;
static void STDMETHODCALLTYPE execute_then_reuse_metadata(ID3D12CommandQueue* queue, UINT count,
    ID3D12CommandList* const* lists) {
    submittedExecute(queue, count, lists);
    // Deterministic simulation of legal list metadata reuse immediately after the
    // driver's submission returns. The fixture retains its lists and allocator.
    for (UINT i = 0; lists && i < count; ++i)
        if (lists[i]) lists[i]->SetPrivateData(mb::kNativeListGuid, 0, nullptr);
}

struct Synthetic {
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* rtv = nullptr;
    ID3D12DescriptorHeap* dsv = nullptr;
    ID3D12Resource* depth = nullptr;  // synthetic host owns every depth resource and state
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12CommandAllocator* prehookAlloc = nullptr;
    ID3D12GraphicsCommandList* prehookList = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    UINT64 value = 0;
    HWND window = nullptr;
    IDXGISwapChain3* sc = nullptr;
    UINT width = 256, height = 160;

    void init() {
        require(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory), "factory");
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&device);
            mb::safe_release(adapter);
            if (SUCCEEDED(hr)) break;
        }
        if (!device) {
            require(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), (void**)&adapter), "WARP adapter");
            require(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&device), "WARP device");
            mb::safe_release(adapter);
        }
        if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), __uuidof(IDXGIAdapter1), (void**)&adapter))) {
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) printf("D3D12 adapter: %ls\n", desc.Description);
            mb::safe_release(adapter);
        }
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        require(device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&queue), "queue");
        require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&alloc), "allocator");
        require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                         __uuidof(ID3D12GraphicsCommandList), (void**)&list), "list");
        require(list->Close(), "initial close");
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        require(device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&rtv), "RTV heap");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 3;
        require(device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&dsv), "DSV heap");
        make_depth();  // created before hooks: diagnostic cannot reconstruct this descriptor
        require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&prehookAlloc), "pre-hook allocator");
        require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, prehookAlloc, nullptr,
            __uuidof(ID3D12GraphicsCommandList), (void**)&prehookList), "pre-hook list");
        require(prehookList->Close(), "pre-hook list close");
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence), "fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ERMC_Synthetic_Compositor";
        if (!RegisterClassW(&wc)) ExitProcess(2);
        window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, width, height, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window || !event) ExitProcess(2);
        sc = create_swapchain(window);  // before hooks: exercise late injection
    }
    IDXGISwapChain3* create_swapchain(HWND hwnd) {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = width; d.Height = height;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* one = nullptr;
        IDXGISwapChain3* three = nullptr;
        require(factory->CreateSwapChainForHwnd(queue, hwnd, &d, nullptr, nullptr, &one), "swapchain");
        require(one->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&three), "swapchain3");
        one->Release();
        return three;
    }
    void wait() {
        require(queue->Signal(fence, ++value), "synthetic signal");
        require(fence->SetEventOnCompletion(value, event), "synthetic fence event");
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) { puts("FATAL GPU timeout"); ExitProcess(2); }
    }
    void begin() { wait(); require(alloc->Reset(), "allocator reset"); require(list->Reset(alloc, nullptr), "list reset"); }
    void end() { require(list->Close(), "list close"); ID3D12CommandList* lists[] = {list}; queue->ExecuteCommandLists(1, lists); wait(); }
    void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from; b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE depth_handle(UINT index = 0) {
        auto h = dsv->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)index * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        return h;
    }
    ID3D12Resource* depth_resource(bool denySrv = false) {
        auto d = mb::tex_desc(width, height, denySrv ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32_TYPELESS);
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (denySrv) d.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        auto hp = mb::heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
        ID3D12Resource* resource = nullptr;
        require(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
            &cv, __uuidof(ID3D12Resource), (void**)&resource), "synthetic host depth");
        return resource;
    }
    void depth_view(ID3D12Resource* resource, UINT index) {
        D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
        desc.Format = DXGI_FORMAT_D32_FLOAT; desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(resource, &desc, depth_handle(index));
    }
    void make_depth() {
        depth = depth_resource(); depthState = D3D12_RESOURCE_STATE_COMMON;
        depth_view(depth, 0);
    }
    void record_depth() {
        // These are known fixture states supplied by the synthetic host, never inferred by
        // the compositor. Half the depth is near, half far, while fallback must draw both.
        barrier(depth, depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto ds = depth_handle();
        list->OMSetRenderTargets(0, nullptr, FALSE, &ds);
        D3D12_RECT left = {0, 0, (LONG)width / 2, (LONG)height};
        D3D12_RECT right = {(LONG)width / 2, 0, (LONG)width, (LONG)height};
        list->ClearDepthStencilView(ds, D3D12_CLEAR_FLAG_DEPTH, 0.95f, 0, 1, &left);
        list->ClearDepthStencilView(ds, D3D12_CLEAR_FLAG_DEPTH, 0.001f, 0, 1, &right);
        barrier(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
    }
    void clear_depth() { begin(); record_depth(); end(); depthState = D3D12_RESOURCE_STATE_DEPTH_READ; }
    UINT clear_host() {
        if (mb::testHeader.mcPid) ++mb::testHeader.mcHeartbeat;  // fixture producer's new frame
        UINT index = sc->GetCurrentBackBufferIndex();
        ID3D12Resource* bb = nullptr;
        require(sc->GetBuffer(index, __uuidof(ID3D12Resource), (void**)&bb), "host backbuffer");
        begin();
        barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(bb, nullptr, handle);
        const float color[] = {0.2f, 0.4f, 0.6f, 1.0f};
        list->ClearRenderTargetView(handle, color, 0, nullptr);
        barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        end(); bb->Release();
        return index;
    }
    std::vector<uint8_t> pixels(UINT index) {
        ID3D12Resource* bb = nullptr;
        ID3D12Resource* readback = nullptr;
        require(sc->GetBuffer(index, __uuidof(ID3D12Resource), (void**)&bb), "readback backbuffer");
        D3D12_RESOURCE_DESC desc = bb->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1;
        rd.DepthOrArraySize = rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto hp = mb::heap_props(D3D12_HEAP_TYPE_READBACK);
        require(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, __uuidof(ID3D12Resource), (void**)&readback), "readback buffer");
        begin();
        barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        end();
        uint8_t* mapped = nullptr;
        require(readback->Map(0, nullptr, (void**)&mapped), "readback map");
        std::vector<uint8_t> out(width * height * 4);
        for (UINT y = 0; y < height; ++y) memcpy(out.data() + y * width * 4, mapped + y * fp.Footprint.RowPitch, width * 4);
        readback->Unmap(0, nullptr); readback->Release(); bb->Release();
        return out;
    }
    void shutdown() {
        wait();
        mb::safe_release(depth); mb::safe_release(dsv);
        mb::safe_release(prehookList); mb::safe_release(prehookAlloc);
        mb::safe_release(sc); mb::safe_release(list); mb::safe_release(alloc); mb::safe_release(rtv);
        mb::safe_release(fence); mb::safe_release(queue); mb::safe_release(device); mb::safe_release(factory);
        CloseHandle(event); DestroyWindow(window);
        UnregisterClassW(L"ERMC_Synthetic_Compositor", GetModuleHandleW(nullptr));
    }
};

static Synthetic* captureHost = nullptr;
static std::vector<uint8_t> capturedPixels;
static float capturedUseDepth = -1, capturedDebugDepth = -1;
static bool captureReadback = true;
static uint64_t advancePoseDuringComposite = 0;
static uint64_t replaceSlotPoseDuringComposite = 0;
static bool tearNoticeBeforeSubmission = false, changeNoticeTtlAfterSelection = false;
namespace mb {
void compositor_harness_after_pose_selection() {
    if (changeNoticeTtlAfterSelection) {
        // Fault injection: changed reserved metadata without a generation advance.
        auto* ttl = (uint32_t*)((uint8_t*)slot_header(0) + kNoticeTtlOffset);
        ++*ttl;
        changeNoticeTtlAfterSelection = false;
    }
    if (replaceSlotPoseDuringComposite) {
        auto* slot = slot_header(0);
        uint32_t seq = slot->seq | 1u; slot->seq = seq; compiler_barrier();
        slot->poseId = replaceSlotPoseDuringComposite;
        compiler_barrier(); slot->seq = seq + 1u;
        replaceSlotPoseDuringComposite = 0;
    }
    if (advancePoseDuringComposite) {
        compositor_note_applied_pose(advancePoseDuringComposite);
        advancePoseDuringComposite = 0;
    }
}
void compositor_harness_before_submission() {
    if (tearNoticeBeforeSubmission) {
        slot_header(0)->seq |= 1u;
        tearNoticeBeforeSubmission = false;
    }
}
void compositor_harness_capture(UINT backbuffer, float useDepth, float debugDepth) {
    if (captureReadback) capturedPixels = captureHost->pixels(backbuffer);
    capturedUseDepth = useDepth; capturedDebugDepth = debugDepth;
}
}

static bool near_pixel(const std::vector<uint8_t>& p, UINT width, UINT x, UINT y, int r, int g, int b) {
    const uint8_t* px = p.data() + (y * width + x) * 4;
    bool ok = abs((int)px[0] - r) <= 3 && abs((int)px[1] - g) <= 3 && abs((int)px[2] - b) <= 3;
    if (!ok) printf("pixel (%u,%u) = %u %u %u, expected %d %d %d\n", x, y, px[0], px[1], px[2], r, g, b);
    return ok;
}

// A destruction marker owned by a resource's COM private data checks lifetime without
// trusting an implementation-specific AddRef return value or reading driver internals.
class ResourceDeathMarker : public IUnknown {
    volatile LONG refs = 1;
    bool* destroyed;
public:
    explicit ResourceDeathMarker(bool* value) : destroyed(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG value = InterlockedDecrement(&refs);
        if (!value) { *destroyed = true; delete this; }
        return value;
    }
};

static mb::NativeDepthAuditTotals audit_snapshot(bool other = false) {
    mb::NativeDepthAuditLock lock;
    return other ? mb::g_depthAuditOther : mb::g_depthAuditHost;
}
static mb::NativeDepthAuditDevice audit_device(Synthetic& host) {
    mb::NativeDepthAuditLock lock;
    mb::NativeDepthAuditDevice result;
    mb::native_depth_audit_read(host.device, mb::kNativeDepthDeviceGuid, result);
    return result;
}

static void test_depth_audit(Synthetic& host) {
    using namespace mb;
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_NO_RELIGHT | ERMC_CTRL_DEBUG_DEPTH;
    native_depth_audit_request(true);  // collect before recording, as the next host Present would
    compositor_poll();  // emulate worker-only hook installation before the host records
    check((g_depthAuditHooks & 0x7f) == 0x7f, "optional base COM depth audit hooks installed");
    auto before = audit_snapshot();
    ID3D12CommandList* unknown[] = {host.prehookList};
    host.queue->ExecuteCommandLists(1, unknown); host.wait();
    check(audit_snapshot().unknown == before.unknown + 1, "pre-hook command list is unknown, never validated");

    before = audit_snapshot();
    host.begin(); host.record_depth(); require(host.list->Close(), "unsubmitted depth close");
    NativeDepthAuditList recorded;
    {
        NativeDepthAuditLock lock;
        native_depth_audit_read(host.list, kNativeDepthListGuid, recorded);
    }
    check(recorded.closed && recorded.resetObserved && recorded.events.binds == 1 && recorded.events.clears == 2 &&
          audit_snapshot().events.binds == before.events.binds,
          "recorded depth calls do not count as submitted work");
    host.begin(); host.end();  // discard the closed recording, submit an empty new generation
    check(audit_snapshot().events.binds == before.events.binds,
          "successful Reset drops abandoned depth recording");

    auto device = audit_device(host);
    check(device.creates == 0, "pre-hook DSV resource/descriptor provenance remains unknown");
    before = audit_snapshot();
    host.clear_depth();
    auto after = audit_snapshot();
    check(after.events.binds == before.events.binds + 1 && after.events.clears == before.events.clears + 2 &&
          after.events.transitions == before.events.transitions + 2 && after.partial == before.partial,
          "real host depth clear/barriers observed on the effective presenting queue");
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_NO_RELIGHT | ERMC_CTRL_DEBUG_DEPTH;
    host.clear_host(); require(host.sc->Present(0, 0), "unknown-depth fallback Present");
    check(capturedUseDepth == 0 && capturedDebugDepth == 0 &&
          near_pixel(capturedPixels, host.width, 48, 120, 25, 179, 76) &&
          near_pixel(capturedPixels, host.width, 208, 120, 25, 179, 76),
          "near/far host depth stays unsampled; fallback pixels remain correct");
    NativeDepthAuditList compositorTag;
    bool compositorTagged;
    {
        NativeDepthAuditLock lock;
        compositorTagged = native_depth_audit_read(g_list, kNativeDepthListGuid, compositorTag);
    }
    // DXGI may submit its own lists inside Present; do not assume one host submission.
    check(!compositorTagged && audit_snapshot().events.binds == after.events.binds,
          "compositor/readback command lists excluded from host depth audit");

    host.depth_view(host.depth, 0);  // API-observed creation still does not resolve bindings
    host.device->CopyDescriptorsSimple(1, host.depth_handle(1), host.depth_handle(0), D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    auto dst = host.depth_handle(2), src = host.depth_handle(1);
    host.device->CopyDescriptors(1, &dst, nullptr, 1, &src, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    device = audit_device(host);
    check(device.creates == 1 && device.simpleCopies == 1 && device.copies == 1 &&
          device.lastResource.Format == DXGI_FORMAT_R32_TYPELESS && device.lastResource.Width == host.width,
          "DSV API descriptions and both copy APIs audited without heap reads");

    bool destroyed = false;
    auto denied = host.depth_resource(true);
    auto marker = new ResourceDeathMarker(&destroyed);
    require(denied->SetPrivateDataInterface(kNativeProbeGuid, marker), "resource death marker");
    marker->Release();
    host.depth_view(denied, 2);
    check(audit_device(host).denySrv == 1, "DENY_SHADER_RESOURCE captured as a concrete depth blocker");
    denied->Release();  // not submitted to GPU; audit may retain descriptions, never this resource
    host.depth_view(nullptr, 2);
    check(destroyed && audit_device(host).nullCreates == 1,
          "diagnostics retain no host resource; null DSV never becomes a candidate");

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* other = nullptr;
    require(host.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&other), "other direct queue");
    before = audit_snapshot(); auto otherBefore = audit_snapshot(true);
    host.begin(); host.record_depth(); require(host.list->Close(), "other queue depth close");
    ID3D12CommandList* work[] = {host.list};
    other->ExecuteCommandLists(1, work);
    require(other->Signal(host.fence, ++host.value), "other queue fence signal");
    require(host.fence->SetEventOnCompletion(host.value, host.event), "other queue fence event");
    if (WaitForSingleObject(host.event, 10000) != WAIT_OBJECT_0) ExitProcess(2);
    host.depthState = D3D12_RESOURCE_STATE_DEPTH_READ;
    check(audit_snapshot().events.binds == before.events.binds &&
          audit_snapshot(true).events.binds == otherBefore.events.binds + 1 &&
          native_identity(g_queue) == native_identity(host.queue),
          "other direct queue depth is distinguished, never selected as host depth");
    other->Release();

    before = audit_snapshot();
    host.begin();
    D3D12_RESOURCE_BARRIER split = {};
    split.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    split.Transition.pResource = host.depth; split.Transition.Subresource = 0;
    split.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_READ;
    split.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    split.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY; host.list->ResourceBarrier(1, &split);
    split.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY; host.list->ResourceBarrier(1, &split);
    D3D12_RESOURCE_BARRIER alias = {}; alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    host.list->ResourceBarrier(1, &alias);  // documented null/null global aliasing barrier
    host.barrier(host.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
    host.end();
    after = audit_snapshot();
    check(after.events.split == before.events.split + 2 && after.events.subresources == before.events.subresources + 2 &&
          after.events.aliasing == before.events.aliasing + 1 && capturedUseDepth == 0,
          "split/per-subresource/aliasing observations do not reconstruct a guessed state");

    ID3D12CommandAllocator* bundleAlloc = nullptr;
    ID3D12GraphicsCommandList* bundle = nullptr;
    require(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, __uuidof(ID3D12CommandAllocator),
        (void**)&bundleAlloc), "bundle allocator");
    require(host.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundleAlloc, nullptr,
        __uuidof(ID3D12GraphicsCommandList), (void**)&bundle), "empty bundle");
    require(bundle->Close(), "empty bundle close");
    before = audit_snapshot(); host.begin(); host.list->ExecuteBundle(bundle); host.end();
    check(audit_snapshot().events.bundles == before.events.bundles + 1,
          "submitted bundle reported as incomplete depth coverage");
    bundle->Release(); bundleAlloc->Release();  // host.end waited for actual GPU completion

    ID3D12GraphicsCommandList4* list4 = nullptr;
    if ((g_depthAuditHooks & 0x80) && SUCCEEDED(host.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList4), (void**)&list4))) {
        before = audit_snapshot(); host.begin();
        host.barrier(host.depth, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds = {};
        ds.cpuDescriptor = host.depth_handle();
        ds.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
        ds.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
        ds.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
        ds.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
        list4->BeginRenderPass(0, nullptr, &ds, D3D12_RENDER_PASS_FLAG_NONE);
        list4->EndRenderPass();
        host.barrier(host.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
        host.end();
        check(audit_snapshot().events.renderPassDepth == before.events.renderPassDepth + 1,
              "real depth render pass observed without decoding its DSV");
        list4->Release();
    } else puts("SKIP: render-pass audit API unavailable");
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options = {};
    ID3D12GraphicsCommandList7* list7 = nullptr;
    if ((g_depthAuditHooks & 0x100) &&
        SUCCEEDED(host.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options))) &&
        options.EnhancedBarriersSupported &&
        SUCCEEDED(host.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList7), (void**)&list7))) {
        before = audit_snapshot(); host.begin();
        D3D12_GLOBAL_BARRIER barrier = {};
        barrier.SyncBefore = barrier.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        barrier.AccessBefore = barrier.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
        D3D12_BARRIER_GROUP group = {};
        group.Type = D3D12_BARRIER_TYPE_GLOBAL; group.NumBarriers = 1; group.pGlobalBarriers = &barrier;
        list7->Barrier(1, &group); host.end();
        check(audit_snapshot().events.enhanced == before.events.enhanced + 1,
              "real enhanced barrier observed without legacy state inference");
        list7->Release();
    } else puts("SKIP: enhanced-barrier GPU support unavailable");

    // Force one reviewable log snapshot after meaningful GPU work (production logs throttle).
    { NativeDepthAuditLock lock; g_depthAuditReported = false; }
    native_depth_audit_report(host.device, host.width, host.height, true);
    check(g_depthAuditMetadataErrors == 0 && SUCCEEDED(host.device->GetDeviceRemovedReason()),
          "depth diagnostics metadata and real GPU remain healthy");
}

static void write_frame_slot(ErmcFrameHeader* slot, UINT width = 64, UINT height = 40,
    float mcDepth = 0.5f, uint64_t pose = 1) {
    using namespace mb;
    uint32_t seq = slot->seq | 1; slot->seq = seq; compiler_barrier();
    slot->width = width; slot->height = height; slot->poseId = pose;
    slot->flags = ERMC_FRAME_HAND; slot->mcNear = 0.05f; slot->mcFar = 1000.0f; slot->fovYDeg = 60;
    const UINT layer = width * height * 4;
    uint8_t* base = (uint8_t*)slot + ERMC_FRAME_HDR;
    memset(base, 0, layer * 4);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        UINT i = (y * width + x) * 4;
        base[i + 1] = base[i + 3] = 128;  // half-transparent green world
        *(float*)(base + layer + i) = mcDepth;
        if (x < width/8 && y >= height*7/8) { base[layer * 2 + i + 2] = 255; base[layer * 2 + i + 3] = 255; }
        if (x >= width*3/8 && x < width*5/8 && y >= height*3/8 && y < height*5/8) {
            base[layer * 3 + i] = 255; base[layer * 3 + i + 3] = 255;
        }
    }
    compiler_barrier(); slot->seq = seq + 1;
}
static void create_frame_file() {
    using namespace mb;
    if (!ipc_ensure_dir()) ExitProcess(2);
    HANDLE f = CreateFileW(ipc_path(L"frames.shm").c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) ExitProcess(2);
    HANDLE mapping = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, ERMC_FRAMES_FILE_SIZE, nullptr);
    if (!mapping) ExitProcess(2);
    uint8_t* mem = (uint8_t*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, ERMC_FRAMES_FILE_SIZE);
    if (!mem) ExitProcess(2);
    ErmcFramesHeader* h = (ErmcFramesHeader*)mem;
    h->magic = ERMC_FRAMES_MAGIC; h->version = ERMC_FRAMES_VERSION;
    ErmcFrameHeader* slot = (ErmcFrameHeader*)(mem + 0x1000);
    write_frame_slot(slot);
    UnmapViewOfFile(mem); CloseHandle(mapping); CloseHandle(f);
}

// Actual rasterized host depth and compositor pixel readback. The synthetic host is an
// authoritative scene adapter: it owns creation state, DSV heap and shader projection.
// Production must provide equivalent evidence; this is not an Elden Ring adapter.
struct DepthRasterFixture {
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    void init(Synthetic& host) {
        const char* shader = R"(
cbuffer C : register(b0) { float depth; };
struct V { float4 p:SV_Position; };
V VS(uint id:SV_VertexID) { V o; float2 uv=float2((id<<1)&2,id&2);
    o.p=float4(uv*float2(2,-2)+float2(-1,1),depth,1); return o; }
float PS(V i):SV_Depth { return depth; }
)";
        ID3DBlob *vs=nullptr,*ps=nullptr,*serialized=nullptr;
        require(D3DCompile(shader,strlen(shader),"depth-raster",nullptr,nullptr,"VS","vs_5_0",0,0,&vs,nullptr),"depth fixture VS");
        require(D3DCompile(shader,strlen(shader),"depth-raster",nullptr,nullptr,"PS","ps_5_0",0,0,&ps,nullptr),"depth fixture PS");
        D3D12_ROOT_PARAMETER parameter={}; parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.Num32BitValues=1;
        D3D12_ROOT_SIGNATURE_DESC signature={}; signature.NumParameters=1; signature.pParameters=&parameter;
        require(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr),"depth fixture root serialize");
        require(host.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
            __uuidof(ID3D12RootSignature),(void**)&root),"depth fixture root");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc={}; desc.pRootSignature=root;
        desc.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; desc.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable=TRUE;
        desc.DepthStencilState.DepthEnable=TRUE; desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        desc.DepthStencilState.StencilReadMask=desc.DepthStencilState.StencilWriteMask=0xff;
        desc.DSVFormat=DXGI_FORMAT_D32_FLOAT; desc.SampleDesc.Count=1; desc.SampleMask=UINT_MAX;
        desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        require(host.device->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),(void**)&pso),"depth fixture pipeline");
        vs->Release(); ps->Release(); serialized->Release();
    }
    void draw(Synthetic& host, const mb::NativeDepthProjection& projection) {
        host.list->SetPipelineState(pso); host.list->SetGraphicsRootSignature(root);
        host.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT viewport={0,0,(float)host.width,(float)host.height,0,1}; host.list->RSSetViewports(1,&viewport);
        // Equivalent to an enemy in front of the house on the left and behind it on the right.
        for(int i=0;i<2;++i) {
            D3D12_RECT rectangle={(LONG)(i*host.width/2),0,(LONG)((i+1)*host.width/2),(LONG)host.height};
            host.list->RSSetScissorRects(1,&rectangle);
            float distance=i?1.0f:0.02f, d=projection.a+projection.b/distance;
            host.list->SetGraphicsRoot32BitConstants(0,1,&d,0); host.list->DrawInstanced(3,1,0,0);
        }
    }
    void shutdown(){ mb::safe_release(pso); mb::safe_release(root); }
};
static mb::NativeSceneDepthCertificate depth_certificate(Synthetic& host, bool reversed, bool infinite=false) {
    using namespace mb;
    NativeSceneDepthCertificate c; c.poseId=1; c.width=host.width; c.height=host.height;
    c.stageId=testState.stageId; c.hostLife=testHeader.hostLife;
    const float n=0.01f,f=1000.0f, sy=1.0f/std::tan(60.0f*3.14159265f/360.0f);
    c.projection[0]=sy/((float)host.width/host.height); c.projection[5]=sy; c.projection[11]=1;
    c.projection[10]=reversed?(infinite?0.0f:-n/(f-n)):(infinite?1.0f:f/(f-n));
    c.projection[14]=reversed?(infinite?n:n*f/(f-n)):(infinite?-n:-n*f/(f-n));
    return c;
}
static mb::DepthExecuteFn depthDriverExecute = nullptr;
static void STDMETHODCALLTYPE depth_execute_then_reuse(ID3D12CommandQueue* queue, UINT count,
    ID3D12CommandList* const* lists) {
    depthDriverExecute(queue,count,lists);
    for(UINT i=0;lists&&i<count;++i) {
        ID3D12GraphicsCommandList* list=nullptr;
        if(lists[i]&&SUCCEEDED(lists[i]->QueryInterface(__uuidof(ID3D12GraphicsCommandList),(void**)&list))) {
            mb::depth_capture_reset(list); list->Release();
        }
    }
}
static void test_native_depth_occlusion(Synthetic& host) {
    using namespace mb;
    puts("=== Native certified D3D12 occlusion GPU tests ===");
    testState.flags=ERMC_STATE_CAMERA_VALID|ERMC_STATE_PLAYER_VALID;
    testControl.flags=ERMC_CTRL_COMPOSITE|ERMC_CTRL_NO_RELIGHT|ERMC_CTRL_DEBUG_DEPTH;
    native_depth_audit_request(true); compositor_poll(); compositor_enable_native_depth(true);
    host.wait(); safe_release(host.depth); host.make_depth();
    check(compositor_register_depth_heap(host.dsv),"register authoritative DSV heap identity");
    check(compositor_register_depth_resource(host.depth,D3D12_RESOURCE_STATE_COMMON),"register creation-time resource state");
    check(!compositor_register_depth_resource(host.depth,D3D12_RESOURCE_STATE_DEPTH_WRITE),"state cannot be repaired by re-registering a resource");
    host.depth_view(host.depth,0);
    DepthRasterFixture fixture; fixture.init(host);
    auto render=[&](NativeSceneDepthCertificate c, bool certify=true, bool copyDescriptor=false) {
        host.begin(); host.barrier(host.depth,host.depthState,D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto ds=host.depth_handle(copyDescriptor?1:0);
        if(copyDescriptor) host.device->CopyDescriptorsSimple(1,ds,host.depth_handle(0),D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        host.list->OMSetRenderTargets(0,nullptr,FALSE,&ds);
        NativeDepthProjection p; check(native_depth_projection(c.projection,p),"actual shader projection supported");
        host.list->ClearDepthStencilView(ds,D3D12_CLEAR_FLAG_DEPTH,p.reversed?0.0f:1.0f,0,0,nullptr);
        fixture.draw(host,p);
        if(certify) check(compositor_certify_scene_depth(host.list,host.depth,c),"scene certificate recorded with observed matching DSV");
        host.barrier(host.depth,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_DEPTH_READ);
        host.end(); host.depthState=D3D12_RESOURCE_STATE_DEPTH_READ;
        host.clear_host(); require(host.sc->Present(0,0),"depth occlusion Present");
    };
    auto pixels_correct=[&]() {
        return near_pixel(capturedPixels,host.width,48,120,51,102,153)&&
            near_pixel(capturedPixels,host.width,208,120,25,179,76)&&
            near_pixel(capturedPixels,host.width,8,8,255,0,0)&&
            near_pixel(capturedPixels,host.width,128,80,0,0,255);
    };
    auto fresh_generation=[&]() {
        // Some DXGI driver lists were created before optional capture was enabled.
        // Their opaque submissions poison state. Positive cases therefore use actual
        // fresh resources; we NEVER repair the old state using an observed StateAfter.
        host.wait(); depth_clear_native_ledger(); depth_release_snapshots(); safe_release(host.depth);
        host.make_depth();
        check(compositor_register_depth_heap(host.dsv)&&compositor_register_depth_resource(host.depth,D3D12_RESOURCE_STATE_COMMON),
            "fresh GPU resource generation after opaque DXGI work");
        host.depth_view(host.depth,0);
    };
    render(depth_certificate(host,true));
    check(capturedUseDepth==1 && capturedDebugDepth==0 && pixels_correct(),
        "reverse-Z: foreground enemy occludes house, background enemy does not; HUD/hand preserved");
    fresh_generation(); render(depth_certificate(host,false),true,true);
    check(capturedUseDepth==1 && pixels_correct(),"standard perspective + copied DSV provenance produce correct occlusion");
    fresh_generation(); render(depth_certificate(host,true,true));
    check(capturedUseDepth==1 && pixels_correct(),"infinite-far reverse-Z GPU occlusion");
    fresh_generation(); render(depth_certificate(host,false,true));
    check(capturedUseDepth==1 && pixels_correct(),"infinite-far standard-Z GPU occlusion");
    auto rightHanded=depth_certificate(host,true); rightHanded.projection[10]*=-1; rightHanded.projection[11]=-1;
    fresh_generation();
    render(rightHanded);
    check(capturedUseDepth==1 && pixels_correct(),"right-handed perspective depth coefficients produce correct GPU occlusion");
    fresh_generation(); depthDriverExecute=g_depthRealExecute; g_depthRealExecute=&depth_execute_then_reuse;
    render(depth_certificate(host,true)); g_depthRealExecute=depthDriverExecute;
    check(capturedUseDepth==1 && pixels_correct(),"legal post-submit metadata Reset cannot erase submitted scene evidence");
    host.clear_host(); require(host.sc->Present(0,0),"no fresh scene Present");
    check(capturedUseDepth==0,"previous scene certificate is not reused on another Present");
    fresh_generation(); auto wrongPose=depth_certificate(host,true); wrongPose.poseId=2; render(wrongPose);
    check(capturedUseDepth==0,"same resource, wrong camera pose falls back");
    fresh_generation(); auto wrongLife=depth_certificate(host,true); ++wrongLife.hostLife; render(wrongLife);
    check(capturedUseDepth==0,"respawn invalidates scene lifetime provenance");
    fresh_generation(); auto wrongZone=depth_certificate(host,true); ++wrongZone.stageId; render(wrongZone);
    check(capturedUseDepth==0,"different zone scene falls back");
    fresh_generation(); auto differentFov=depth_certificate(host,true);
    differentFov.projection[5]=1.0f/std::tan(70.0f*3.14159265f/360.0f);
    differentFov.projection[0]=differentFov.projection[5]/((float)host.width/host.height);
    render(differentFov);
    check(capturedUseDepth==0,"actual host FOV different from uploaded Minecraft frame falls back");
    auto wrongProjection=depth_certificate(host,true); wrongProjection.projection[0]*=0.7f;
    check(!native_depth_projection_matches(NativeDepthProjection{},60,host.width,host.height),"invalid projection compatibility rejected");
    host.begin(); auto ds=host.depth_handle(); host.list->OMSetRenderTargets(0,nullptr,FALSE,&ds);
    check(!compositor_certify_scene_depth(host.list,host.depth,wrongProjection),"aspect mismatch cannot certify scene"); host.end();
    auto jittered=depth_certificate(host,true); jittered.projection[8]=0.001f;
    NativeDepthProjection p;
    check(!native_depth_projection(jittered.projection,p),"unknown temporal jitter/oblique projection rejected");
    float orthographic[16]={}; orthographic[0]=orthographic[5]=orthographic[10]=orthographic[15]=1;
    check(!native_depth_projection(orthographic,p),"orthographic shadow depth rejected");
    auto restrictedViewport=depth_certificate(host,true); restrictedViewport.viewportMinDepth=0.2f;
    host.begin();ds=host.depth_handle();host.list->OMSetRenderTargets(0,nullptr,FALSE,&ds);
    check(!compositor_certify_scene_depth(host.list,host.depth,restrictedViewport),"nonstandard depth viewport cannot certify scene");host.end();
    fresh_generation();
    D3D12_DESCRIPTOR_HEAP_DESC unknownHeapDesc={};unknownHeapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;unknownHeapDesc.NumDescriptors=1;
    ID3D12DescriptorHeap* unknownHeap=nullptr;
    require(host.device->CreateDescriptorHeap(&unknownHeapDesc,__uuidof(ID3D12DescriptorHeap),(void**)&unknownHeap),"unregistered source DSV heap");
    D3D12_DEPTH_STENCIL_VIEW_DESC unknownView={};unknownView.Format=DXGI_FORMAT_D32_FLOAT;
    unknownView.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    auto unknownHandle=unknownHeap->GetCPUDescriptorHandleForHeapStart();
    host.device->CreateDepthStencilView(host.depth,&unknownView,unknownHandle);
    host.device->CopyDescriptorsSimple(1,host.depth_handle(1),unknownHandle,D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    host.begin();ds=host.depth_handle(1);host.list->OMSetRenderTargets(0,nullptr,FALSE,&ds);
    check(!compositor_certify_scene_depth(host.list,host.depth,depth_certificate(host,true)),
        "copy from unknown heap erases destination provenance instead of retaining an old mapping");host.end();unknownHeap->Release();
    fresh_generation();
    host.begin();host.barrier(host.depth,host.depthState,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ds=host.depth_handle();host.list->OMSetRenderTargets(0,nullptr,FALSE,&ds);
    host.list->ClearDepthStencilView(ds,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
    check(compositor_certify_scene_depth(host.list,host.depth,depth_certificate(host,true)),"unsubmitted scene certificate recorded");
    require(host.list->Close(),"unsubmitted certificate list close");
    host.clear_host();require(host.sc->Present(0,0),"unsubmitted certificate fallback Present");
    check(capturedUseDepth==0,"closed but unsubmitted scene certificate never authorizes GPU sampling");
    fresh_generation(); render(depth_certificate(host,true),false);
    check(capturedUseDepth==0,"full resolution depth without scene certificate remains fallback");
    ID3D12CommandList* unknown[]={host.prehookList}; host.queue->ExecuteCommandLists(1,unknown); host.wait();
    render(depth_certificate(host,true));
    check(capturedUseDepth==0,"unknown submission poisons state; later StateAfter cannot repair it");
    // Resize forces full reacquisition; fresh creation evidence restores the GPU path.
    require(host.sc->ResizeBuffers(2,320,192,DXGI_FORMAT_UNKNOWN,0),"occlusion resize");
    host.width=320; host.height=192;
    host.clear_host(); require(host.sc->Present(0,0),"resize warmup fallback");
    check(capturedUseDepth==0 && g_depthResources.empty(),"resize cannot reuse old depth, descriptors or certificates");
    host.wait(); safe_release(host.depth); host.make_depth();
    check(compositor_register_depth_heap(host.dsv)&&compositor_register_depth_resource(host.depth,D3D12_RESOURCE_STATE_COMMON),
        "new resize generation accepts fresh creation and heap evidence");
    host.depth_view(host.depth,0); render(depth_certificate(host,true));
    check(capturedUseDepth==1 && pixels_correct(),"resized actual host scene occludes Minecraft using fresh GPU snapshot");
    // Typed, DENY_SHADER_RESOURCE source: copy it, never create a forbidden host SRV.
    host.wait(); depth_clear_native_ledger(); depth_release_snapshots(); safe_release(host.depth);
    host.depth=host.depth_resource(true); host.depthState=D3D12_RESOURCE_STATE_COMMON;
    check(compositor_register_depth_heap(host.dsv)&&compositor_register_depth_resource(host.depth,D3D12_RESOURCE_STATE_COMMON),
        "typed deny-SRV source has authoritative creation evidence");
    host.depth_view(host.depth,0); render(depth_certificate(host,true));
    check(capturedUseDepth==1 && pixels_correct(),"DENY_SHADER_RESOURCE host depth is copied to owned typeless SRV, never sampled directly");
    // Isolate fenced lifetime from any opaque work submitted inside the driver's DXGI
    // Present. Unknown submissions intentionally poison the state ledger.
    host.wait(); depth_clear_native_ledger(); depth_release_snapshots(); safe_release(host.depth);
    host.depth=host.depth_resource(true);host.depthState=D3D12_RESOURCE_STATE_COMMON;
    check(compositor_register_depth_heap(host.dsv)&&compositor_register_depth_resource(host.depth,D3D12_RESOURCE_STATE_COMMON),
        "pending GPU test begins with a fresh authoritative resource generation");
    host.depth_view(host.depth,0);
    bool sourceDestroyed=false;
    auto marker=new ResourceDeathMarker(&sourceDestroyed);
    require(host.depth->SetPrivateDataInterface(kNativeProbeGuid,marker),"fenced source lifetime marker"); marker->Release();
    // Hold the real GPU queue behind an unsignaled fence. Call the actual compositor
    // submission without DXGI Present (the driver may wait for swapchain latency there).
    // The fixture temporarily suppresses synchronous pixel readback.
    auto c=depth_certificate(host,true); NativeDepthProjection projection; native_depth_projection(c.projection,projection);
    printf("Lifetime before recording: actual=0x%x tracked=0x%x known=%d\n",host.depthState,
        g_depthResources[host.depth].state,g_depthResources[host.depth].known);
    host.begin(); host.barrier(host.depth,host.depthState,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ds=host.depth_handle(); host.list->OMSetRenderTargets(0,nullptr,FALSE,&ds);
    host.list->ClearDepthStencilView(ds,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr); fixture.draw(host,projection);
    check(compositor_certify_scene_depth(host.list,host.depth,c),"pending GPU lifetime scene certified");
    host.barrier(host.depth,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_DEPTH_READ); host.end();
    host.depthState=D3D12_RESOURCE_STATE_DEPTH_READ; host.clear_host();
    ID3D12Fence* gate=nullptr;
    require(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),(void**)&gate),"GPU lifetime gate");
    require(host.queue->Wait(gate,1),"GPU queue gate wait");
    std::thread gateTimeout([gate] { Sleep(1000); gate->Signal(1); });
    captureReadback=false; UINT submittedRing=g_ringPos%kRing;
    composite(host.sc,0); captureReadback=true;
    bool gpuPending=g_fence->GetCompletedValue()<g_ring[submittedRing].fence;
    printf("Lifetime snapshot selection: %s\n",g_depthFallbackReason);
    depth_clear_native_ledger(); safe_release(host.depth);
    printf("Lifetime evidence: pending=%d, destroyed=%d, source=%d, useDepth=%.0f, ring=%u, submitted=%llu completed=%llu\n",
        gpuPending,sourceDestroyed,(bool)g_depthRings[submittedRing].sourceUntilFence,capturedUseDepth,submittedRing,
        (unsigned long long)g_ring[submittedRing].fence,(unsigned long long)g_fence->GetCompletedValue());
    check(gpuPending && !sourceDestroyed && g_depthRings[submittedRing].sourceUntilFence,
        "source remains alive after host/ledger release while actual GPU fence is pending");
    require(gate->Signal(1),"release GPU lifetime gate"); host.wait();
    gateTimeout.join(); gate->Release();
    depth_release_snapshots();
    check(sourceDestroyed,"source retires after real GPU completion; no leaked source reference");
    // Reinitialize only after a new authoritative creation/lifetime generation.
    host.wait(); depth_clear_native_ledger();
    check(g_depthResources.empty() && !g_depthScene.resource,"ledger generation drops stale scene and state identities");
    check(g_depthMetadataObjects.empty(),"all DLL-owned COM private interfaces removed before unload");
    compositor_enable_native_depth(false); native_depth_audit_request(false); fixture.shutdown();
    testControl.flags=ERMC_CTRL_COMPOSITE|ERMC_CTRL_NO_RELIGHT;
    require(host.sc->ResizeBuffers(2,256,160,DXGI_FORMAT_UNKNOWN,0),"restore fixture size");
    host.width=256;host.height=160;host.make_depth();host.clear_depth();
    host.clear_host(); require(host.sc->Present(0,0),"restored fixture Present");
    puts("=== Native occlusion GPU tests complete ===");
}

// ReShade exports raw depth as an ordinary R32_FLOAT texture, not a certified DSV.
// Exercise the real snapshot/composite shaders with that contract and no old ledger.
static void test_reshade_depth_occlusion(Synthetic& host) {
    using namespace mb;
    puts("=== ReShade raw R32_FLOAT GPU tests ===");
    host.wait(); compositor_enable_native_depth(false); native_depth_audit_request(false);
    compositor_poll(); depth_clear_native_ledger(); depth_release_snapshots();
    check(!g_nativeSceneEnabled && !g_depthScene.resource && g_depthResources.empty(),
        "ReShade tests cannot fall through to an old strict scene certificate");
    check(g_srvCpuHeap && g_srvCpuHeap->GetDesc().Flags == D3D12_DESCRIPTOR_HEAP_FLAG_NONE &&
        g_srvHeap && (g_srvHeap->GetDesc().Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
        "snapshot descriptors have a CPU-only staging source and a separate shader-visible heap");
    testState.flags = ERMC_STATE_CAMERA_VALID | ERMC_STATE_PLAYER_VALID;
    testState.nearZ = 0.01f; testState.farZ = 1000.0f;
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_NO_RELIGHT;
    testHeader.debugFlags = 0;
    constexpr auto readState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto d = tex_desc(host.width, host.height, DXGI_FORMAT_R32_FLOAT);
    auto hp = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    ID3D12Resource* source = nullptr;
    require(host.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&source), "ReShade raw texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT64 bytes = 0;
    host.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = 1;
    bd.DepthOrArraySize = bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto uploadHeap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    ID3D12Resource* upload = nullptr;
    require(host.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&upload), "ReShade depth upload");
    auto raw = [](float z) { return (0.01f * 1000.0f / z - 0.01f) / (1000.0f - 0.01f); };
    auto write_depth = [&](bool swapped, D3D12_RESOURCE_STATES before, float nearDistance = 0.02f, float farDistance = 1.0f) {
        uint8_t* mapped = nullptr; require(upload->Map(0, nullptr, (void**)&mapped), "ReShade upload map");
        for (UINT y = 0; y < host.height; ++y) for (UINT x = 0; x < host.width; ++x)
            ((float*)(mapped + fp.Offset + y * fp.Footprint.RowPitch))[x] = raw(((x < host.width/2) != swapped) ? nearDistance : farDistance);
        upload->Unmap(0, nullptr);
        host.begin();
        if (before != D3D12_RESOURCE_STATE_COPY_DEST) host.barrier(source, before, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = source; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        host.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        host.barrier(source, D3D12_RESOURCE_STATE_COPY_DEST, readState); host.end();
    };
    auto render = [&](ID3D12Resource* depth, uint64_t generation) {
        host.clear_host(); g_reshadeDepth = depth; g_reshadeDevice = host.device; g_reshadeGeneration = generation;
        capturedUseDepth = -1; capturedPixels.clear();
        composite(host.sc, 0);
        g_reshadeDepth = nullptr; g_reshadeDevice = nullptr; g_reshadeGeneration = 0;
        check(capturedPixels.size() == host.width * host.height * 4, "ReShade frame actually submitted and read back");
    };
    auto pixels = [&](bool useDepth, bool swapped = false, bool suppressWorld = false) {
        if (capturedPixels.size() != host.width * host.height * 4) return false;
        bool leftHidden = suppressWorld || (useDepth && !swapped), rightHidden = suppressWorld || (useDepth && swapped);
        return near_pixel(capturedPixels, host.width, 48, 120, leftHidden ? 51 : 25, leftHidden ? 102 : 179, leftHidden ? 153 : 76) &&
            near_pixel(capturedPixels, host.width, 208, 120, rightHidden ? 51 : 25, rightHidden ? 102 : 179, rightHidden ? 153 : 76) &&
            near_pixel(capturedPixels, host.width, 8, 8, 255, 0, 0) && near_pixel(capturedPixels, host.width, 128, 80, 0, 0, 255);
    };
    write_depth(false, D3D12_RESOURCE_STATE_COPY_DEST); render(source, 1);
    check(capturedUseDepth == 1 && capturedDebugDepth == 0 && pixels(true),
        "ReShade near host hides MC, far host shows MC; HUD and hand survive real shader readback");
    // Re-upload from the promised restored state, swapping occlusion repeatedly through ring wrap.
    for (UINT i = 0; i < kRing + 1; ++i) {
        bool swapped = (i % 2) == 0; write_depth(swapped, readState); render(source, i + 2);
        check(capturedUseDepth == 1 && pixels(true, swapped), "ReShade source read-state round trip and ring snapshot refresh");
    }
    render(nullptr, 10);
    check(capturedUseDepth == 0 && pixels(false), "missing ReShade depth uses actual no-depth shader fallback");
    render(source, 0);
    check(capturedUseDepth == 0 && pixels(false), "zero ReShade generation cannot reuse a prior snapshot");
    ID3D12Resource* unsupported = nullptr; d.Format = DXGI_FORMAT_R16_FLOAT;
    require(host.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, readState, nullptr,
        __uuidof(ID3D12Resource), (void**)&unsupported), "unsupported ReShade texture");
    render(unsupported, 11);
    check(capturedUseDepth == 0 && pixels(false), "unsupported ReShade format falls back without sampling stale depth");
    safe_release(unsupported);
    testControl.flags |= ERMC_CTRL_NO_DEPTH_TEST; render(source, 12);
    check(capturedUseDepth == 0 && pixels(false), "explicit no-depth control bypasses even valid ReShade depth");
    testControl.flags &= ~ERMC_CTRL_NO_DEPTH_TEST;
    testState.flags |= ERMC_STATE_HOST_BUSY; render(source, 12);
    check(capturedUseDepth == 0 && pixels(false), "busy host suppresses ReShade depth");
    testState.flags &= ~ERMC_STATE_HOST_BUSY;

    // Shared aspect predicate is compiled both in this compositor and the public API adapter.
    check(reshade_aspect_matches(960, 540, 1920, 1080) && reshade_aspect_matches(1919, 1080, 1920, 1080) &&
        !reshade_aspect_matches(1024, 768, 1920, 1080) && !reshade_aspect_matches(1920, 1088, 1920, 1080) &&
        !reshade_aspect_matches(0, 1080, 1920, 1080),
        "depth input aspect accepts scaled/rounded frames and rejects different frusta or empty dimensions");
    testControl.poseLag = 0;
    compositor_note_applied_pose(2); render(source, 21);
    check(g_uploadedPose == 1 && g_compositeTargetPose == 2 && capturedUseDepth == 0 && pixels(false, false, true),
        "mismatched ReShade pose suppresses only world; real HUD and hand pixels survive");
    testControl.flags |= ERMC_CTRL_NO_DEPTH_TEST; render(source, 22);
    check(capturedUseDepth == 0 && pixels(false), "intentional no-depth still shows world during pose mismatch");
    testControl.flags &= ~ERMC_CTRL_NO_DEPTH_TEST; render(nullptr, 23);
    check(capturedUseDepth == 0 && pixels(false), "missing ReShade depth keeps legacy world fallback during pose mismatch");
    compositor_note_applied_pose(1); advancePoseDuringComposite = 2; render(source, 24);
    check(g_compositeTargetPose == 1 && pose_for_present(0) == 2 && capturedUseDepth == 1 && pixels(true),
        "camera advance after frame selection cannot change the latched pose or its depth decision");
    compositor_note_applied_pose(1);
    write_frame_slot(slot_header(0), 64, 64); render(source, 25);
    check(capturedUseDepth == 0 && pixels(false, false, true),
        "incompatible MC aspect suppresses world without suppressing HUD or hand");
    testControl.flags |= ERMC_CTRL_NO_DEPTH_TEST; render(source, 26);
    check(capturedUseDepth == 0 && pixels(false), "no-depth bypass remains explicit for incompatible MC aspect");
    testControl.flags &= ~ERMC_CTRL_NO_DEPTH_TEST;
    write_frame_slot(slot_header(0)); render(source, 27);
    check(capturedUseDepth == 1 && pixels(true), "matching scaled MC aspect resumes world and depth immediately");

    auto mc_raw = [](float z) { return (1000.0f - 0.05f * 1000.0f / z) / (1000.0f - 0.05f); };
    write_frame_slot(slot_header(0), 64, 40, mc_raw(300.0f));
    write_depth(false, readState, 299.0f, 301.0f); render(source, 28);
    check(capturedUseDepth == 1 && pixels(true),
        "300m MC is hidden by 299m rock and visible before 301m rock; long-distance bias cannot leak a metre");
    write_frame_slot(slot_header(0), 64, 40, mc_raw(10.0f));
    write_depth(false, readState, 9.96f, 11.0f); render(source, 29);
    check(capturedUseDepth == 1 && pixels(false), "small near-contact tolerance remains to avoid surface flicker");
    write_frame_slot(slot_header(0)); write_depth(false, readState); render(source, 30);
    check(capturedUseDepth == 1 && pixels(true), "ordinary close occlusion restored after long-distance regression");

    // Clear earlier snapshot refs only after completion; then hold the real queue pending.
    host.wait(); depth_release_snapshots(); host.clear_host();
    bool destroyed = false; auto marker = new ResourceDeathMarker(&destroyed);
    require(source->SetPrivateDataInterface(kNativeProbeGuid, marker), "ReShade source lifetime marker"); marker->Release();
    ID3D12Fence* gate = nullptr;
    require(host.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&gate), "ReShade queue gate");
    require(host.queue->Wait(gate, 1), "ReShade pending GPU wait");
    std::thread gateTimeout([gate] { Sleep(2000); gate->Signal(1); });
    captureReadback = false; g_reshadeDepth = source; g_reshadeDevice = host.device; g_reshadeGeneration = 20;
    UINT initialRingPos = g_ringPos, firstRing = g_ringPos % kRing;
    for (UINT i = 0; i < kRing; ++i) composite(host.sc, 0);
    UINT fullRingPos = g_ringPos; auto* pendingSnapshot = g_depthRings[firstRing].snapshot;
    check(fullRingPos == initialRingPos + kRing && pendingSnapshot && capturedUseDepth == 1 &&
        g_depthRings[firstRing].sourceUntilFence == source,
        "ReShade gate holds three actual snapshot submissions, not fallback or skipped frames");
    uint64_t pendingFence = g_ring[firstRing].fence;
    composite(host.sc, 0); // would overwrite the first slot if its fence were ignored
    bool pending = g_fence->GetCompletedValue() < pendingFence;
    check(pending && g_ringPos == fullRingPos && g_depthRings[firstRing].snapshot == pendingSnapshot,
        "ReShade pending snapshot cannot be overwritten when the GPU ring is full");
    g_reshadeDepth = nullptr; g_reshadeDevice = nullptr; g_reshadeGeneration = 0; safe_release(source);
    check(pending && !destroyed && g_depthRings[firstRing].sourceUntilFence,
        "ReShade source remains alive after producer release until snapshot fence completes");
    require(gate->Signal(1), "release ReShade queue gate"); host.wait(); gateTimeout.join(); safe_release(gate);
    captureReadback = true; depth_release_snapshots();
    check(destroyed, "ReShade source retires after GPU completion without leaked snapshot references");
    safe_release(upload);
    check(!g_nativeSceneEnabled && !g_depthScene.resource && g_depthResources.empty(),
        "ReShade GPU tests completed without strict certificate or native resource ledger");
    puts("=== ReShade raw GPU tests complete ===");
}

static void test_passive_composition(Synthetic& host) {
    using namespace mb;
    puts("=== Passive companion GPU tests ===");
    testState.flags = ERMC_STATE_CAMERA_VALID | ERMC_STATE_PLAYER_VALID;
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_PASSIVE_COMPOSITE | ERMC_CTRL_NO_RELIGHT;
    testControl.mcFrame = 100;
    auto* slot = slot_header(0);
    write_frame_slot(slot, 64, 40, .5f, 100);
    slot->flags = 1; // real producer's world-only contract; old GUI bytes intentionally remain
    compositor_note_applied_pose(1);
    host.clear_host(); capturedPixels.clear(); composite(host.sc, 0);
    check(capturedPixels.size() == host.width * host.height * 4 &&
          near_pixel(capturedPixels, host.width, 8, 8, 25, 179, 76) &&
          near_pixel(capturedPixels, host.width, 128, 80, 25, 179, 76) &&
          g_uploadedPose == 100 && g_compositeTargetPose == 100 && compositor_world_active(true) &&
          !compositor_world_active(false),
          "passive GPU frame uses exact published pose, draws world, suppresses stale HUD/hand and certifies only passive mode");
    testControl.mcFrame = 101;
    write_frame_slot(slot, 64, 40, .5f, 101); slot->flags = 1;
    replaceSlotPoseDuringComposite = 102;
    UINT before = g_ringPos;
    host.clear_host(); capturedPixels.clear(); composite(host.sc, 0);
    check(g_ringPos == before && capturedPixels.empty() && g_uploadedPose == 100 &&
          !compositor_world_active(true),
          "slot overwritten after selection cannot draw old passive pixels or certify an invisible companion, even without depth");
    write_frame_slot(slot);
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_NO_RELIGHT;
    host.clear_host(); capturedPixels.clear(); composite(host.sc, 0);
    check(capturedPixels.size() == host.width * host.height * 4 &&
          near_pixel(capturedPixels, host.width, 8, 8, 255, 0, 0) &&
          near_pixel(capturedPixels, host.width, 128, 80, 0, 0, 255) &&
          compositor_world_active(false) && !compositor_world_active(true),
          "return from passive to Minecraft restores normal hand/HUD and active-mode submission proof");
}

static void write_notice_frame(ErmcFrameHeader* slot, uint64_t pose, uint32_t ttlMs = 500) {
    using namespace mb;
    write_frame_slot(slot, 64, 40, .5f, pose);
    uint32_t seq = slot->seq | 1u; slot->seq = seq; compiler_barrier();
    slot->flags = 1u | 2u | kFrameNotice;
    slot->frameId = pose;
    *(uint32_t*)((uint8_t*)slot + kNoticeTtlOffset) = ttlMs;
    const size_t bytes = 64 * 40 * 4;
    uint8_t* gui = (uint8_t*)slot + ERMC_FRAME_HDR + bytes * 2;
    memset(gui, 0, bytes); // freshly cleared notice plane; old red HUD is removed
    for (UINT y = 30; y < 38; ++y) for (UINT x = 16; x < 48; ++x) {
        size_t i = (y * 64 + x) * 4;
        gui[i + 1] = gui[i + 2] = gui[i + 3] = 255; // yellow notice; rows bottom-up
    }
    compiler_barrier(); slot->seq = seq + 1u;
}

static void test_recovery_notice(Synthetic& host) {
    using namespace mb;
    puts("=== Bounded recovery notice GPU tests ===");
    testState.flags = ERMC_STATE_CAMERA_VALID | ERMC_STATE_PLAYER_VALID;
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_PASSIVE_COMPOSITE | ERMC_CTRL_NO_RELIGHT;
    testControl.mcFrame = 400;
    auto* slot = slot_header(0);
    noticeTestNow = GetTickCount64() + 1000;
    reset_notice_leases();
    write_frame_slot(slot, 64, 40, .5f, 400); slot->flags = 1;
    host.clear_host(); capturedPixels.clear(); composite(host.sc, 0); host.wait();
    // Check both pixel suppression and the two-plane CPU transfer directly.
    auto& next = g_ring[g_ringPos % kRing];
    memset(next.uploadPtr + g_fp[2].Offset, 0xCC, g_fp[2].Footprint.RowPitch * g_texH);
    memset(next.uploadPtr + g_fp[3].Offset, 0xDD, g_fp[3].Footprint.RowPitch * g_texH);
    write_frame_slot(slot, 64, 40, .5f, 400); slot->flags = 1;
    host.clear_host(); composite(host.sc, 0);
    check(near_pixel(capturedPixels, host.width, 8, 8, 25, 179, 76) &&
          near_pixel(capturedPixels, host.width, 128, 80, 25, 179, 76) &&
          next.uploadPtr[g_fp[2].Offset] == 0xCC && next.uploadPtr[g_fp[3].Offset] == 0xDD,
          "ordinary passive has no old hearts/chat/hand pixels and transfers only color/R32");
    testControl.mcFrame = 401;
    write_notice_frame(slot, 401);
    host.clear_host(); composite(host.sc, 0);
    FrameUploadStamp original = g_uploadedFrame;
    uint64_t deadline = g_noticeLeases[0].deadlineMs;
    check(near_pixel(capturedPixels, host.width, 128, 20, 255, 255, 0) &&
          near_pixel(capturedPixels, host.width, 8, 8, 25, 179, 76) &&
          near_pixel(capturedPixels, host.width, 128, 80, 25, 179, 76) &&
          compositor_world_active(true) && original.noticeTtlMs == 500 && !g_texHand,
          "fresh cleared notice is visible with exact world/pose, no stale HUD or hand");
    noticeTestNow += 499;
    UINT before = g_ringPos;
    host.clear_host(); composite(host.sc, 0);
    check(g_ringPos == before + 1 && same_frame_upload(original, g_uploadedFrame) &&
          g_noticeLeases[0].deadlineMs == deadline && near_pixel(capturedPixels, host.width, 128, 20, 255, 255, 0),
          "cached duplicate notice remains visible before fixed deadline without renewal");
    ++noticeTestNow;
    host.clear_host(); composite(host.sc, 0);
    check(near_pixel(capturedPixels, host.width, 128, 20, 25, 179, 76) && compositor_world_active(true) &&
          g_noticeLeases[0].deadlineMs == deadline,
          "notice expires at exact TTL while valid passive world continues");
    forget_uploaded_frame(); ++noticeTestNow;
    host.clear_host(); composite(host.sc, 0);
    check(near_pixel(capturedPixels, host.width, 128, 20, 25, 179, 76) && g_noticeLeases[0].deadlineMs == deadline,
          "upload invalidation and duplicate resubmission cannot resurrect consumed notice");
    auto reject = [&](const char* name) {
        UINT start = g_ringPos; host.clear_host(); capturedPixels.clear(); composite(host.sc, 0);
        check(g_ringPos == start && capturedPixels.empty() && !compositor_world_active(true), name);
    };
    testControl.mcFrame = 402; // older-pose notice must not be used
    reject("wrong pose notice draws nothing and supplies no world proof");
    for (uint32_t flags : {2u | kFrameNotice, 1u | kFrameNotice, 3u, 15u, 11u | 16u}) {
        write_notice_frame(slot, 402); slot->flags = flags;
        reject("notice-only/missing-GUI/old-GUI/hand/unknown flags reject passive frame");
    }
    for (uint32_t ttl : {0u, 8001u, 0xFFFFFFFFu}) {
        write_notice_frame(slot, 402, ttl);
        reject("out-of-range notice TTL rejects passive frame");
    }
    write_notice_frame(slot, 402); slot->seq |= 1u;
    reject("torn notice publication cannot reuse old GUI or world proof");
    write_notice_frame(slot, 402); changeNoticeTtlAfterSelection = true;
    reject("notice metadata change after selection invalidates fullstamp");
    // The same seqlock generation with altered metadata remains poisoned.
    reject("same-seq notice metadata tampering cannot mint a new deadline");
    write_notice_frame(slot, 402); tearNoticeBeforeSubmission = true;
    reject("notice torn after command recording is rejected before submission");
    write_notice_frame(slot, 402, 8000);
    host.clear_host(); composite(host.sc, 0);
    check(near_pixel(capturedPixels, host.width, 128, 20, 255, 255, 0) && compositor_world_active(true),
          "fresh generation with maximum allowed TTL resumes notice and world");
    // A notice may draw while the depth/aspect contract suppresses world; the
    // resulting banner cannot advertise an actually submitted world layer.
    host.wait(); write_notice_frame(slot, 403); testControl.mcFrame = 403;
    testState.flags = ERMC_STATE_PLAYER_VALID;
    reject("notice cannot bypass missing valid passive camera");
    testState.flags = ERMC_STATE_CAMERA_VALID | ERMC_STATE_PLAYER_VALID | ERMC_STATE_HOST_BUSY;
    reject("notice cannot bypass host busy/death/loading state");
    testState.flags = ERMC_STATE_CAMERA_VALID | ERMC_STATE_PLAYER_VALID;
    // Fill a real gated GPU ring, then first observe another notice while busy.
    host.clear_host(); host.wait();
    ID3D12Fence* gate = nullptr;
    require(host.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&gate), "notice queue gate");
    require(host.queue->Wait(gate, 1), "hold notice GPU queue");
    HANDLE gateReleased = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!gateReleased) ExitProcess(2);
    std::thread timeout([gate, gateReleased] { WaitForSingleObject(gateReleased, 10000); gate->Signal(1); });
    captureReadback = false;
    write_notice_frame(slot, 403);
    before = g_ringPos;
    for (int i = 0; i < kRing; ++i) composite(host.sc, 0);
    check(g_ringPos == before + kRing, "notice test fills three actual pending GPU submissions");
    testControl.mcFrame = 404; write_notice_frame(slot, 404, 10);
    before = g_ringPos; composite(host.sc, 0);
    uint64_t busyDeadline = g_noticeLeases[0].deadlineMs;
    noticeTestNow += 10;
    for (int i = 0; i < 5; ++i) composite(host.sc, 0);
    check(g_ringPos == before && !compositor_world_active(true) && g_noticeLeases[0].deadlineMs == busyDeadline,
          "backpressure never submits unsafe world proof or renews first-observed notice deadline");
    require(gate->Signal(1), "release notice queue gate");
    SetEvent(gateReleased); host.wait(); timeout.join(); CloseHandle(gateReleased); safe_release(gate);
    captureReadback = true; host.clear_host(); composite(host.sc, 0);
    check(near_pixel(capturedPixels, host.width, 128, 20, 25, 179, 76) && compositor_world_active(true),
          "notice expired in backpressure remains suppressed when queue resumes");
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_NO_RELIGHT;
    noticeTestNow = 0;
    write_frame_slot(slot); compositor_note_applied_pose(1);
    host.clear_host(); composite(host.sc, 0);
    check(near_pixel(capturedPixels, host.width, 8, 8, 255, 0, 0) &&
          near_pixel(capturedPixels, host.width, 128, 80, 0, 0, 255) && compositor_world_active(false),
          "return to active restores ordinary fresh hand/HUD and active world proof");
    puts("=== Bounded recovery notice GPU tests complete ===");
}

static double elapsed_ms(const LARGE_INTEGER& start) {
    LARGE_INTEGER end, frequency;
    QueryPerformanceCounter(&end); QueryPerformanceFrequency(&frequency);
    return double(end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
}
static void test_upload_backpressure(Synthetic& host) {
    using namespace mb;
    puts("=== Nonblocking upload GPU tests ===");
    testState.flags = ERMC_STATE_CAMERA_VALID | ERMC_STATE_PLAYER_VALID;
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_NO_RELIGHT;
    auto* slot = slot_header(0);
    write_frame_slot(slot, 64, 40, .5f, 300); compositor_note_applied_pose(300);
    host.clear_host(); composite(host.sc, 0); host.wait();
    ID3D12Fence* gate = nullptr;
    require(host.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&gate), "upload queue gate");
    require(host.queue->Wait(gate, 1), "hold upload GPU queue");
    std::thread timeout([gate] { Sleep(2000); gate->Signal(1); });
    captureReadback = false;
    UINT before = g_ringPos;
    for (int i = 0; i < kRing; ++i) composite(host.sc, 0);
    check(g_ringPos == before + kRing, "three real compositor submissions fill the gated GPU ring");
    before = g_ringPos;
    auto* oldUpload = g_ring[before % kRing].upload;
    auto* oldTexture = g_tex[0];
    auto oldStamp = g_uploadedFrame;
    const uint64_t oldFence = g_ring[before % kRing].fence;
    LARGE_INTEGER start; QueryPerformanceCounter(&start);
    for (int i = 0; i < 5; ++i) composite(host.sc, 0);
    double busyMs = elapsed_ms(start);
    printf("GPU_RING_BUSY five_calls_ms=%.4f pending=%d\n", busyMs, g_fence->GetCompletedValue() < oldFence);
    check(busyMs < 25 && g_ringPos == before && g_ring[before % kRing].upload == oldUpload &&
        g_fence->GetCompletedValue() < oldFence && !compositor_world_active(false),
        "full ring returns without GPU wait, overwrite or unsafe body submission proof");
    write_frame_slot(slot, 80, 50, .5f, 300);
    QueryPerformanceCounter(&start); composite(host.sc, 0);
    double resizeMs = elapsed_ms(start);
    printf("GPU_PENDING_RESIZE one_call_ms=%.4f size=%ux%u failed=%d\n", resizeMs, g_texW, g_texH, g_failed);
    check(resizeMs < 25 && g_texW == 64 && g_texH == 40 && g_tex[0] == oldTexture &&
        g_ring[before % kRing].upload == oldUpload && g_ringPos == before && !g_failed &&
        g_uploadedFrame.slot == oldStamp.slot && g_uploadedFrame.seq == oldStamp.seq && !compositor_world_active(false),
        "pending resolution change retains in-use textures/uploads and retries without waiting or disabling compositor");
    require(gate->Signal(1), "release upload queue"); host.wait(); timeout.join(); safe_release(gate);
    captureReadback = true;
    host.clear_host(); composite(host.sc, 0);
    check(g_texW == 80 && g_texH == 50 && !g_failed && compositor_world_active(false) &&
        near_pixel(capturedPixels, host.width, 208, 120, 25, 179, 76),
        "GPU completion permits deferred texture recreation and correct fresh pixels");
    // A new generation in progress must never certify the previous publication.
    before = g_ringPos; ++slot->seq; capturedPixels.clear();
    host.clear_host(); composite(host.sc, 0);
    check(g_ringPos == before && !compositor_world_active(false),
        "in-progress replacement cannot draw old geometry or authorize invisible body");
    ++slot->seq;
    write_frame_slot(slot); compositor_note_applied_pose(1);
    host.clear_host(); composite(host.sc, 0);
}

#if ERMC_COMPOSITOR_NONBLOCKING
static int test_mapping_nonblocking() {
    using namespace mb;
    wchar_t temp[MAX_PATH]; if (!GetTempPathW(MAX_PATH, temp)) return 2;
    testDirectory = std::wstring(temp) + L"ERMC_Mapping_Harness_" + std::to_wstring(GetCurrentProcessId());
    create_frame_file();
    testHeader.mcPid = 7; testHeader.mcStartMs = 1; testHeader.mcHeartbeat = 1;
    testControl.flags = ERMC_CTRL_COMPOSITE;
    composite(nullptr, 0); ++testHeader.mcHeartbeat;
    unsigned before = mappingCalls.load();
    LARGE_INTEGER start; QueryPerformanceCounter(&start);
    composite(nullptr, 0);
    double firstMs = elapsed_ms(start);
    check(!g_frames && !g_sc && mappingCalls.load() == before && g_framesRequested,
        "native Present requests mapping without any directory/file/driver I/O");
    mappingGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    mappingEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker([] { frames_poll(); });
    check(WaitForSingleObject(mappingEntered, 1000) == WAIT_OBJECT_0, "mapping worker reached isolated disk gate");
    QueryPerformanceCounter(&start);
    for (int i = 0; i < 1000; ++i) composite(nullptr, 0);
    double busyMs = elapsed_ms(start);
    check(!g_frames && !g_sc && busyMs < 25 && mappingCalls.load() == before + 1,
        "1000 Presents remain bounded while worker is blocked in mapping I/O");
    SetEvent(mappingGate); worker.join();
    check(g_frames && frames_validate_layout(), "worker safely adopts the completed view under renderLock");
    printf("DISK_MAPPING first_present_ms=%.4f thousand_pending_calls_ms=%.4f\n", firstMs, busyMs);
    CloseHandle(mappingGate); CloseHandle(mappingEntered); mappingGate = mappingEntered = nullptr;
    if (g_frames) UnmapViewOfFile(g_frames); g_frames = nullptr;
    DeleteFileW(ipc_path(L"frames.shm").c_str()); RemoveDirectoryW(testDirectory.c_str());
    return failures ? 1 : 0;
}
#endif

// CPU-only coverage of the production slot selector and cached mapping guard. The
// pagefile-backed fixture creates no files, window, device, hooks or game process.
static void test_frame_layout_contract() {
    using namespace mb;
    check(ERMC_FRAMES_VERSION == 3, "QHD stride has a distinct local frame protocol version");
    check(ERMC_VERSION == 1 && ERMC_SHM_SIZE == 8u * 1024u * 1024u,
        "QHD leaves the control protocol version and size unchanged");
    constexpr uint64_t qhdSlotBytes = 0x100ull + 2560ull * 1441ull * 16ull;
    check(ERMC_FRAME_SLOT_SIZE == qhdSlotBytes &&
        ERMC_FRAMES_FILE_SIZE == 0x1000ull + 3ull * qhdSlotBytes,
        "all three QHD slots reserve four full layers including the extra client row");
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, ERMC_FRAMES_FILE_SIZE, nullptr);
    if (!mapping) { check(false, "create pagefile frame fixture"); return; }
    g_frames = (uint8_t*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, ERMC_FRAMES_FILE_SIZE);
    CloseHandle(mapping);
    if (!g_frames) { check(false, "map pagefile frame fixture"); return; }
    auto* header = (ErmcFramesHeader*)g_frames;
    header->magic = ERMC_FRAMES_MAGIC; header->version = ERMC_FRAMES_VERSION;
    check(frames_open(), "matching cached frame mapping is accepted");
    const UINT dimensions[][2] = {{2560, 1440}, {2560, 1441}, {1920, 1080}};
    for (const auto& size : dimensions) {
        for (uint32_t i = 0; i < ERMC_FRAME_SLOTS; ++i) {
            auto* slot = slot_header(i);
            slot->seq = 2; slot->width = size[0]; slot->height = size[1];
            slot->poseId = 10 + i;
            const uint64_t payloadEnd = (uint8_t*)slot - g_frames +
                ERMC_FRAME_HDR + (uint64_t)size[0] * size[1] * 16;
            check(payloadEnd <= ERMC_FRAMES_FILE_SIZE &&
                ERMC_FRAME_HDR + (uint64_t)size[0] * size[1] * 16 <= ERMC_FRAME_SLOT_SIZE,
                "four-layer QHD payload stays inside its slot and the mapped file");
            check(pick_slot(10 + i) == slot,
                "production selector accepts QHD, one extra client row and existing 1080p");
        }
    }
    for (uint32_t i = 0; i < ERMC_FRAME_SLOTS; ++i) slot_header(i)->seq = 1;
    auto* slot = slot_header(ERMC_FRAME_SLOTS - 1);
    slot->seq = 2; slot->poseId = 30;
    const UINT invalid[][2] = {{2561, 1440}, {2560, 1442}, {0, 1440},
        {2560, 0}, {0xffffffffu, 0xffffffffu}};
    for (const auto& size : invalid) {
        slot->width = size[0]; slot->height = size[1];
        check(!pick_slot(30), "production selector rejects dimensions outside QHD slot capacity");
    }
    slot->width = 2560; slot->height = 1441; slot->seq = 3;
    check(!pick_slot(30), "production selector still rejects an in-progress publication");
    g_haveFrame = true; g_texHand = true;
    g_uploadedFrame = {slot, 2, 30}; g_uploadedPose = 30;
    header->version = 2;
    g_lastMapAttempt = now_ms();
    check(!frames_open() && !g_frames,
        "a producer restart with the old stride invalidates an already cached mapping");
    check(!g_haveFrame && !g_texHand && !g_uploadedFrame.slot && !g_uploadedPose,
        "layout rejection discards cached textures and upload identity before a mapping can reuse them");
    // The old implementation keeps the view: release it after the expected red failure.
    if (g_frames) { UnmapViewOfFile(g_frames); g_frames = nullptr; }
    g_lastMapAttempt = 0;

    // Wrong magic and unknown future layouts also fail closed on cached views.
    for (uint32_t version : {ERMC_FRAMES_VERSION, 4u}) {
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, ERMC_FRAMES_FILE_SIZE, nullptr);
        if (!mapping) { check(false, "create rejected-layout fixture"); return; }
        g_frames = (uint8_t*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, ERMC_FRAMES_FILE_SIZE);
        CloseHandle(mapping);
        if (!g_frames) { check(false, "map rejected-layout fixture"); return; }
        header = (ErmcFramesHeader*)g_frames;
        header->magic = version == ERMC_FRAMES_VERSION ? 0 : ERMC_FRAMES_MAGIC;
        header->version = version;
        check(!frames_open() && !g_frames, "invalid magic or unknown frame layout cannot reach slot selection");
        if (g_frames) { UnmapViewOfFile(g_frames); g_frames = nullptr; }
    }
}

// Real D3D12 upload/composition/readback at the requested display size. The
// producer's third slot also exercises the end of the enlarged shared mapping.
static int test_qhd_gpu() {
    using namespace mb;
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    testDirectory = std::wstring(temp) + L"ERMC_QHD_Harness_" + std::to_wstring(GetCurrentProcessId());
    ID3D12Debug* debug = nullptr;
    bool debugLayer = SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void**)&debug));
    if (debugLayer) { debug->EnableDebugLayer(); debug->Release(); }
    printf("D3D12 debug layer: %s\n", debugLayer ? "enabled" : "unavailable");
    Synthetic host;
    host.width = 2560; host.height = 1440;
    host.init(); captureHost = &host;
    ID3D12InfoQueue* info = nullptr;
    host.device->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&info);
    check(MH_Initialize() == MH_OK, "QHD MinHook initialization");
    check(compositor_init(), "QHD compositor hook initialization");
    if (!g_nativeHooked) return 2;
    testHeader.debugFlags = 1;
    require(host.sc->Present(0, 0), "QHD queue discovery Present");
    host.clear_host(); require(host.sc->Present(0, 0), "QHD initial test-pattern Present");
    check(g_sc && g_bbW == 2560 && g_bbH == 1440, "actual QHD swapchain acquired");
    create_frame_file(); g_lastMapAttempt = 0;
    if (!frames_open()) { check(false, "QHD frame mapping opened"); return 2; }
    for (uint32_t i = 0; i < ERMC_FRAME_SLOTS; ++i) slot_header(i)->seq = 1;
    testHeader.debugFlags = 0; testHeader.mcPid = 7;
    testHeader.mcStartMs = 1; testHeader.mcHeartbeat = 1;
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_NO_RELIGHT | ERMC_CTRL_NO_DEPTH_TEST;
    testState.nearZ = 0.05f; testState.farZ = 10000;
    require(host.sc->Present(0, 0), "QHD consumer baseline Present");
    auto* slot = slot_header(2);
    for (UINT height : {1440u, 1441u}) {
        const uint64_t pose = height;
        write_frame_slot(slot, 2560, height, 0.5f, pose);
        slot->frameId = pose;
        auto* frames = (ErmcFramesHeader*)g_frames;
        frames->latestSlot = 2; frames->latestFrameId = pose;
        compositor_note_applied_pose(pose);
        host.clear_host();
        capturedPixels.clear();
        require(host.sc->Present(0, 0), "QHD four-layer Present");
        host.wait();
        printf("QHD GPU case: producer=2560x%u, display=2560x1440, slot=2\n", height);
        check(g_uploadedFrame.slot == slot && g_uploadedFrame.frameId == pose &&
            g_texW == 2560 && g_texH == height && g_layerBytes == (uint64_t)2560 * height * 4,
            "third QHD slot was actually uploaded with all source rows");
        bool layers = true;
        for (int i = 0; i < 4; ++i) {
            if (!g_tex[i]) { layers = false; continue; }
            const auto desc = g_tex[i]->GetDesc();
            layers &= desc.Width == 2560 && desc.Height == height;
        }
        check(layers, "GPU owns full-size world, depth, HUD and hand textures");
        const bool readback = capturedPixels.size() == (size_t)2560 * 1440 * 4;
        check(readback, "all 2560x1440 output pixels were read back from the GPU");
        if (readback) {
            check(near_pixel(capturedPixels, 2560, 2000, 1080, 25, 179, 76),
                "QHD world premultiplied-alpha composition matches expected pixels");
            check(near_pixel(capturedPixels, 2560, 80, 72, 255, 0, 0),
                "QHD HUD keeps its top-left orientation and red pixels");
            check(near_pixel(capturedPixels, 2560, 1280, 720, 0, 0, 255),
                "QHD hand layer keeps its center position and blue pixels");
            check(near_pixel(capturedPixels, 2560, 2559, 1439, 25, 179, 76),
                "QHD final display row and column contain the expected world pixels");
        }
        check(SUCCEEDED(host.device->GetDeviceRemovedReason()), "QHD GPU device remains healthy");
    }

    // Full QHD passive color/depth, with stale hand/HUD bytes still in the slot.
    // The producer declares only world; output must stay identical after removing
    // the unused GUI upload. Also exercise an actual R32 host-depth snapshot.
    testState.flags = ERMC_STATE_CAMERA_VALID | ERMC_STATE_PLAYER_VALID;
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_PASSIVE_COMPOSITE | ERMC_CTRL_NO_RELIGHT;
    testControl.mcFrame = 1441;
    ++slot->seq; compiler_barrier(); slot->flags = 1; compiler_barrier(); ++slot->seq;
    auto d = tex_desc(2560, 1440, DXGI_FORMAT_R32_FLOAT);
    auto hp = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    ID3D12Resource* rawDepth = nullptr;
    require(host.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rawDepth), "QHD raw depth");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT64 depthBytes = 0;
    host.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &depthBytes);
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = depthBytes; bd.Height = 1;
    bd.DepthOrArraySize = bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto up = heap_props(D3D12_HEAP_TYPE_UPLOAD); ID3D12Resource* depthUpload = nullptr;
    require(host.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&depthUpload), "QHD raw depth upload");
    uint8_t* mapped = nullptr; require(depthUpload->Map(0, nullptr, (void**)&mapped), "QHD raw depth map");
    // Near occluder in the left half; far depth in the right half, reversed-Z.
    for (UINT y = 0; y < 1440; ++y) for (UINT x = 0; x < 2560; ++x)
        ((float*)(mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch))[x] = x < 1280 ? 1.0f : 0.0f;
    depthUpload->Unmap(0, nullptr);
    host.begin(); D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = rawDepth; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = depthUpload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    host.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    constexpr auto readState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    host.barrier(rawDepth, D3D12_RESOURCE_STATE_COPY_DEST, readState); host.end();
    g_reshadeDepth = rawDepth; g_reshadeDevice = host.device; g_reshadeGeneration = 1;
    host.clear_host(); capturedPixels.clear(); composite(host.sc, 0);
    check(capturedUseDepth == 1 && compositor_world_active(true) && g_texW == 2560 && g_texH == 1441 &&
        near_pixel(capturedPixels, 2560, 640, 1080, 51, 102, 153) &&
        near_pixel(capturedPixels, 2560, 2000, 1080, 25, 179, 76) &&
        near_pixel(capturedPixels, 2560, 80, 72, 51, 102, 153) &&
        near_pixel(capturedPixels, 2560, 1400, 720, 25, 179, 76),
        "QHD passive R32 occlusion preserves near/far pixels and suppresses stale hand/HUD without GUI transfer");
    g_reshadeDepth = nullptr; g_reshadeDevice = nullptr; g_reshadeGeneration = 0;
    host.wait(); depth_release_snapshots(); safe_release(rawDepth); safe_release(depthUpload);
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_NO_RELIGHT | ERMC_CTRL_NO_DEPTH_TEST;

    // Keep a separate producer view while the production reader rejects and
    // unmaps its view. Restoring the ABI with reused IDs must upload fresh pixels.
    HANDLE file = CreateFileW(ipc_path(L"frames.shm").c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE mapping = file == INVALID_HANDLE_VALUE ? nullptr :
        CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, ERMC_FRAMES_FILE_SIZE, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    auto* producer = mapping ? (uint8_t*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, ERMC_FRAMES_FILE_SIZE) : nullptr;
    if (mapping) CloseHandle(mapping);
    check(producer != nullptr, "QHD restart fixture holds an independent producer mapping");
    if (producer) {
        auto* producerHeader = (ErmcFramesHeader*)producer;
        producerHeader->version = 2; compiler_barrier();
        const uint32_t beforeReject = g_ringPos;
        host.clear_host(); require(host.sc->Present(0, 0), "QHD incompatible-layout Present");
        check(!g_frames && !g_haveFrame && !g_uploadedFrame.slot && g_ringPos == beforeReject,
            "QHD incompatible cached layout submits no old frame and discards upload reuse");
        auto* restarted = (ErmcFrameHeader*)(producer + 0x1000 + 2 * ERMC_FRAME_SLOT_SIZE);
        const uint32_t reusedSeq = restarted->seq;
        restarted->seq = reusedSeq | 1; compiler_barrier();
        auto* world = (uint8_t*)restarted + ERMC_FRAME_HDR;
        for (size_t i = 0; i < (size_t)restarted->width * restarted->height; ++i) {
            world[4*i] = world[4*i+1] = 0; world[4*i+2] = world[4*i+3] = 255;
        }
        compiler_barrier(); restarted->seq = reusedSeq;
        producerHeader->version = ERMC_FRAMES_VERSION; compiler_barrier();
        g_lastMapAttempt = 0;
#if ERMC_COMPOSITOR_NONBLOCKING
        compositor_poll(); // remapping/restart is worker-owned too
#endif
        host.clear_host(); capturedPixels.clear();
        require(host.sc->Present(0, 0), "QHD compatible-layout recovery Present");
        host.wait();
        check(g_ringPos > beforeReject && g_uploadedFrame.frameId == 1441 &&
            capturedPixels.size() == (size_t)2560 * 1440 * 4 &&
            near_pixel(capturedPixels, 2560, 2000, 1080, 255, 0, 0),
            "QHD recovery uploads fresh pixels despite reused frame and sequence IDs");
        UnmapViewOfFile(producer);
    }
    host.wait();
    if (info) {
        UINT errors = 0;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T length = 0; info->GetMessage(i, nullptr, &length);
            std::vector<uint8_t> message(length);
            auto* m = (D3D12_MESSAGE*)message.data();
            if (SUCCEEDED(info->GetMessage(i, m, &length)) &&
                (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                printf("D3D12 QHD validation error: %s\n", m->pDescription); ++errors;
            }
        }
        check(errors == 0, "QHD D3D12 debug-layer validation"); info->Release();
    }
    host.wait(); compositor_detach(); compositor_shutdown();
    check(MH_Uninitialize() == MH_OK, "QHD MinHook shutdown");
    host.shutdown();
    DeleteFileW(ipc_path(L"frames.shm").c_str()); RemoveDirectoryW(testDirectory.c_str());
    printf("QHD GPU result: %s (%d failures). Synthetic only; no game access.\n",
        failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    // Fatal HRESULT paths use ExitProcess, which does not flush CRT pipe buffers.
    // Preserve the actual failing GPU operation in noninteractive test logs.
    setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace mb;
    const bool noticeOnly = argc == 2 && strcmp(argv[1], "--notice-gpu") == 0;
#if ERMC_COMPOSITOR_NONBLOCKING
    if (argc == 2 && strcmp(argv[1], "--mapping-contract") == 0) return test_mapping_nonblocking();
#endif
    if (argc == 2 && strcmp(argv[1], "--frame-contract") == 0) {
        test_frame_layout_contract();
        printf("Native frame layout result: %s (%d failures). CPU-only, no game access.\n",
            failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && strcmp(argv[1], "--qhd-gpu") == 0) return test_qhd_gpu();
    test_consumer_liveness();
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    testDirectory = std::wstring(temp) + L"ERMC_Compositor_Harness_" + std::to_wstring(GetCurrentProcessId());
    ID3D12Debug* debug = nullptr;
    bool debugLayer = SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void**)&debug));
    if (debugLayer) { debug->EnableDebugLayer(); debug->Release(); }
    printf("D3D12 debug layer: %s\n", debugLayer ? "enabled" : "unavailable");
    Synthetic host;
    host.init();
    captureHost = &host;
    ID3D12InfoQueue* info = nullptr;
    host.device->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&info);
    check(MH_Initialize() == MH_OK, "MinHook initialization");
    check(compositor_init(), "dummy-object hook discovery / installation");
    if (!g_nativeHooked) return 2;
    bool optionalIdle = g_depthAuditHooks == 0;
    for (int i = kNativeRequiredHookCount; i < kNativeHookCount; ++i)
        optionalIdle &= !g_nativeHookMade[i] && !g_nativeHookEnabled[i];
    check(optionalIdle, "ordinary startup does not patch optional descriptor/depth/list hooks");
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_DEBUG_DEPTH;
    require(host.sc->Present(0, 0), "idle stale-control Present");
    check(!g_sc && !g_pso && !g_probeActive && !g_depthAuditEnabled && g_lastMapAttempt == 0,
          "no MC PID ignores stale control without probing, mapping or GPU initialization");
    testControl.flags = ERMC_CTRL_COMPOSITE;
    testHeader.mcPid = 7; testHeader.mcStartMs = 1; testHeader.mcHeartbeat = 1;
    require(host.sc->Present(0, 0), "consumer baseline Present");
    ++testHeader.mcHeartbeat;
    require(host.sc->Present(0, 0), "live consumer without frames Present");
    check(!g_sc && !g_pso && !g_probeActive,
          "live consumer without frames does not claim swapchain, probe or allocate pipeline");
    testControl.flags = 0;
    testHeader.debugFlags = 1;
    UINT before = g_ringPos;
    host.sc->Present(0, DXGI_PRESENT_TEST);
    check(g_ringPos == before && !g_sc, "DXGI_PRESENT_TEST does not render or claim swapchain");
    host.sc->Present(0, 0);  // register real backbuffers, never choose an arbitrary queue
    check(!g_sc && g_probeCount == 2, "late injection waits for evidence of backbuffer submission");
    submittedExecute = (NativeExecute)g_nativeOrig[kSlotExecute];
    g_nativeOrig[kSlotExecute] = (void*)&execute_then_reuse_metadata;
    UINT index = host.clear_host();
    g_nativeOrig[kSlotExecute] = (void*)submittedExecute;
    require(host.sc->Present(0, 0), "test-pattern Present");
    check(g_sc && native_identity(g_queue) == native_identity(host.queue),
          "late-injection queue association survives metadata reuse immediately after submission");
    auto p = capturedPixels;
    if (p.empty()) { puts("FATAL compositor did not submit a frame"); return 2; }
    check(near_pixel(p, host.width, 8, 8, 112, 71, 61), "test-pattern shader changes backbuffer pixels");
    check(near_pixel(p, host.width, 160, 120, 51, 102, 153), "test pattern leaves unrelated pixels intact");

    NativeListTag sentinel = {0x123456789abcdef0ull, 0};
    require(host.list->SetPrivateData(kNativeListGuid, sizeof(sentinel), &sentinel), "idle metadata sentinel");
    g_nativePerfEnabled.store(true);
    g_nativeProbeCalls.store(0); g_nativeMetadataCalls.store(0);
    host.clear_host();
    NativeListTag idleTag = {}; UINT idleBytes = sizeof(idleTag);
    require(host.list->GetPrivateData(kNativeListGuid, &idleBytes, &idleTag), "idle metadata readback");
    check(!g_probeActive && idleBytes == sizeof(idleTag) && idleTag.epoch == sentinel.epoch &&
          g_nativeProbeCalls == 0 && g_nativeMetadataCalls == 0 && g_nativeHookCalls[kSlotBarrier] > 0,
          "real Reset/Barrier/Execute calls leave idle probe metadata intact and enter no probe locks");
    g_nativePerfEnabled.store(false);

    create_frame_file();
    g_lastMapAttempt = 0;  // only the fixture file changed; do not wait for production retry cadence
#if ERMC_COMPOSITOR_NONBLOCKING
    compositor_poll(); // production worker, not Present, opens the fixture's file
#endif
    compositor_note_applied_pose(1);
    testState.nearZ = 0.05f; testState.farZ = 10000;
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_NO_RELIGHT | ERMC_CTRL_DEBUG_DEPTH;
    testHeader.debugFlags = 0;
    index = host.clear_host(); require(host.sc->Present(0, 0), "Minecraft-layers Present");
    p = capturedPixels;
    check(near_pixel(p, host.width, 160, 120, 25, 179, 76), "mapped world + Minecraft depth / premultiplied alpha");
    check(near_pixel(p, host.width, 8, 8, 255, 0, 0), "HUD layer orientation and color");
    check(near_pixel(p, host.width, 128, 80, 0, 0, 255), "hand layer orientation and color");
    check(g_depthBound == -1 && capturedUseDepth == 0 && capturedDebugDepth == 0 && compositor_active(),
          "native host-depth fallback remains disabled while composition is active");
    testControl.flags = ERMC_CTRL_COMPOSITE;
    DXGI_PRESENT_PARAMETERS present = {};
    index = host.clear_host(); require(host.sc->Present1(0, 0, &present), "relighting Present1");
    p = capturedPixels;
    check(g_down1 && g_down2 && g_ringPos >= 3, "original relighting/downsample passes execute via Present1");
    check(!g_depthAuditEnabled && audit_snapshot().submissions == 0,
          "ordinary composition leaves depth diagnostics collection off");

    if (!noticeOnly) {
        test_depth_audit(host);
        test_native_depth_occlusion(host);
        test_reshade_depth_occlusion(host);
    }
    test_passive_composition(host);
    test_recovery_notice(host);
    test_upload_backpressure(host);
    uint64_t depthEpoch = g_depthAuditEpoch;

    require(host.sc->ResizeBuffers(2, 320, 192, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
    check(g_depthAuditEpoch != depthEpoch && audit_snapshot().events.binds == 0,
          "resize invalidates submitted depth observations");
    host.width = 320; host.height = 192;
    host.wait(); safe_release(host.depth); host.make_depth(); host.clear_depth();
    index = host.clear_host(); require(host.sc->Present(0, 0), "post-resize Present");
    check(g_bbW == 320 && g_bbH == 192 && g_bbCount == 2, "backbuffer/scene resources reacquired after resize");
    check(capturedUseDepth == 0 && capturedDebugDepth == 0 && g_depthResources.empty(),
          "replacement host depth after resize preserves fallback");
    UINT masks[] = {1, 1}; IUnknown* queues[] = {host.queue, host.queue};
    require(host.sc->ResizeBuffers1(2, 256, 160, DXGI_FORMAT_UNKNOWN, 0, masks, queues), "ResizeBuffers1");
    host.width = 256; host.height = 160;
    index = host.clear_host(); require(host.sc->Present(0, 0), "post-ResizeBuffers1 Present");
    check(g_bbW == 256 && native_identity(g_queue) == native_identity(host.queue), "ResizeBuffers1 keeps explicit queue association");

    HWND extra = CreateWindowExW(0, L"ERMC_Synthetic_Compositor", L"", WS_OVERLAPPED, 0, 0, 64, 64,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    IDXGISwapChain3* second = host.create_swapchain(extra);
    ID3D12CommandQueue* captured = nullptr; UINT size = sizeof(captured);
    require(second->GetPrivateData(kNativeQueueGuid, &size, &captured), "factory-captured private queue");
    check(native_identity(captured) == native_identity(host.queue), "CreateSwapChainForHwnd captures actual pDevice command queue");
    safe_release(captured); second->Release(); DestroyWindow(extra);

    UINT renderedBeforeDisconnect = g_ringPos;
    uint64_t fenceBeforeDisconnect = g_fenceNext;
    testHeader.mcPid = 0;  // retain stale COMPOSITE + DEBUG_DEPTH and an already uploaded frame
    require(host.sc->Present(0, 0), "disconnected consumer Present");
    compositor_poll();
    optionalIdle = g_depthAuditHooks == 0;
    for (int i = kNativeRequiredHookCount; i < kNativeHookCount; ++i) optionalIdle &= !g_nativeHookEnabled[i];
    check(g_ringPos == renderedBeforeDisconnect && g_fenceNext == fenceBeforeDisconnect &&
          !g_depthAuditEnabled && optionalIdle,
          "disconnect stops stale-frame GPU submissions and worker disables optional audit hooks");
    testHeader.mcPid = 7; testHeader.mcStartMs = 2; testHeader.mcHeartbeat = 0;
    testControl.flags = ERMC_CTRL_COMPOSITE | ERMC_CTRL_NO_RELIGHT;
    require(host.sc->Present(0, 0), "reconnected consumer baseline Present");
    host.clear_host(); require(host.sc->Present(0, 0), "reconnected consumer frame Present");
    check(g_ringPos > renderedBeforeDisconnect && capturedUseDepth == 0,
          "fresh producer session resumes native composition with depth still disabled");

    host.wait();
    if (info) {
        UINT errors = 0;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T length = 0; info->GetMessage(i, nullptr, &length);
            std::vector<uint8_t> message(length);
            auto* m = (D3D12_MESSAGE*)message.data();
            if (SUCCEEDED(info->GetMessage(i, m, &length)) &&
                (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                printf("D3D12 validation error: %s\n", m->pDescription); ++errors;
            }
        }
        check(errors == 0, "D3D12 debug-layer validation"); info->Release();
    }
    compositor_detach();
    check(g_inflight == 0, "detours drained before shutdown");
    compositor_shutdown();
    check(MH_Uninitialize() == MH_OK, "MinHook shutdown");
    host.shutdown();
    DeleteFileW(ipc_path(L"frames.shm").c_str()); RemoveDirectoryW(testDirectory.c_str());
    printf("Synthetic compositor result: %s (%d failures). No game-runtime validation.\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
