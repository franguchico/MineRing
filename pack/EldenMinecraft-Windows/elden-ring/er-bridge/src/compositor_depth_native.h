// Included inside namespace mb, after the existing audit types/functions.
#pragma once

static const GUID kDepthHeapOwnerGuid = {0x731e1b21,0x17eb,0x4f73,{0xa1,0x78,0x93,0x42,0x4f,0xa0,0x0d,0x31}};
static const GUID kDepthRecordingGuid = {0x731e1b21,0x17eb,0x4f73,{0xa1,0x78,0x93,0x42,0x4f,0xa0,0x0d,0x32}};
static SRWLOCK g_depthNativeLock = SRWLOCK_INIT;
// Serializes ALL observed host submissions against snapshot submission. CPU recording
// remains parallel. Never acquire this lock from the compositor's nested Execute call.
static SRWLOCK g_depthSubmissionLock = SRWLOCK_INIT;
static uint64_t g_depthNativeEpoch = 1, g_depthNativeSerial = 0;
static const size_t kDepthLedgerLimit = 128, kDepthEventLimit = 4096;

struct DepthNativeLock {
    DepthNativeLock() { AcquireSRWLockExclusive(&g_depthNativeLock); }
    ~DepthNativeLock() { ReleaseSRWLockExclusive(&g_depthNativeLock); }
};
template<class T> static std::shared_ptr<T> depth_owned(T* p) {
    if (!p) return {};
    p->AddRef(); return std::shared_ptr<T>(p, [](T* q) { q->Release(); });
}
struct DepthView {
    std::shared_ptr<ID3D12Resource> resource;
    D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
    uint64_t generation = 0;
};
struct DepthHeap {
    uint64_t epoch = 0;
    SIZE_T base = 0;
    UINT count = 0, stride = 0;
    void* device = nullptr;
    std::map<SIZE_T,DepthView> views;
    bool contains(SIZE_T handle) const {
        return handle >= base && stride && (handle-base)%stride == 0 && (handle-base)/stride < count;
    }
};
enum class DepthEventType { Barrier, Bind, Clear, Scene, Invalidate };
struct DepthEvent {
    DepthEventType type = DepthEventType::Invalidate;
    std::shared_ptr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON, after = D3D12_RESOURCE_STATE_COMMON;
    bool full = false;
    NativeSceneDepthCertificate certificate;
};
struct DepthRecording {
    uint64_t epoch = 0;
    bool reset = false, closed = false, invalid = false;
    std::vector<DepthEvent> events;
    std::shared_ptr<ID3D12Resource> bound;
};
template<class T> class DepthMetadata final : public IUnknown {
    volatile LONG refs = 1;
public:
    std::shared_ptr<T> value;
    explicit DepthMetadata(std::shared_ptr<T> p) : value(std::move(p)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&refs); if (!r) delete this; return r;
    }
};
// Retain only explicitly tracked objects, bounded by the ledger limit. This prevents
// descriptor heap address reuse and lets shutdown REMOVE every private-data owner whose
// IUnknown vtable lives in this DLL before the core can be unloaded.
static std::map<ID3D12Object*,std::shared_ptr<ID3D12Object>> g_depthMetadataObjects;
template<class T> static std::shared_ptr<T> depth_metadata(ID3D12Object* object, REFGUID key) {
    if (!g_depthMetadataObjects.count(object)) return {}; // never cast another core's private data
    DepthMetadata<T>* owner = nullptr;
    UINT size = sizeof(owner);
    // GetPrivateData adds a COM reference to interfaces stored with SetPrivateDataInterface.
    if (FAILED(object->GetPrivateData(key,&size,&owner)) || !owner) return {};
    auto result = size == sizeof(owner) ? owner->value : std::shared_ptr<T>{};
    owner->Release(); return result;
}
template<class T> static bool depth_store(ID3D12Object* object, REFGUID key, std::shared_ptr<T> value) {
    if (!g_depthMetadataObjects.count(object)) {
        if (g_depthMetadataObjects.size() >= kDepthLedgerLimit*2) return false;
        g_depthMetadataObjects.emplace(object,depth_owned(object));
    }
    auto* owner = new DepthMetadata<T>(std::move(value));
    HRESULT hr = object->SetPrivateDataInterface(key,owner); owner->Release(); return SUCCEEDED(hr);
}
struct DepthResource {
    std::shared_ptr<ID3D12Resource> owner;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool known = false, initialized = false;
};
struct DepthScene {
    std::shared_ptr<ID3D12Resource> resource;
    NativeSceneDepthCertificate certificate;
    uint64_t serial = 0;
};
static std::vector<std::weak_ptr<DepthHeap>> g_depthHeaps;
static std::map<ID3D12Resource*,DepthResource> g_depthResources;
static DepthScene g_depthScene;
static const char* g_depthFallbackReason = "no authoritative scene adapter certificate";

static bool depth_capture_active() {
    return g_nativeSceneEnabled.load(std::memory_order_relaxed) && native_depth_audit_active();
}
static void depth_clear_native_ledger() {
    DepthNativeLock lock;
    ++g_depthNativeEpoch;
    for (auto& weak : g_depthHeaps) if (auto heap = weak.lock()) heap->views.clear();
    g_depthHeaps.clear(); g_depthResources.clear(); g_depthScene = {};
    for (auto& item : g_depthMetadataObjects) {
        item.first->SetPrivateDataInterface(kDepthHeapOwnerGuid,nullptr);
        item.first->SetPrivateDataInterface(kDepthRecordingGuid,nullptr);
    }
    g_depthMetadataObjects.clear();
    g_depthFallbackReason = "generation invalidated; creation and scene evidence required";
}
void compositor_enable_native_depth(bool enabled) {
    g_nativeSceneEnabled.store(enabled,std::memory_order_relaxed);
}
bool compositor_register_depth_heap(ID3D12DescriptorHeap* heap) {
    if (!heap || !depth_capture_active()) return false;
    const auto desc = heap->GetDesc();
    if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || !desc.NumDescriptors) return false;
    ID3D12Device* dev = nullptr;
    if (FAILED(heap->GetDevice(__uuidof(ID3D12Device),(void**)&dev))) return false;
    auto data = std::make_shared<DepthHeap>();
    data->base = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    data->stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    data->count = desc.NumDescriptors; data->device = native_identity(dev); dev->Release();
    DepthNativeLock lock;
    auto existing = depth_metadata<DepthHeap>(heap,kDepthHeapOwnerGuid);
    if (existing && existing->epoch == g_depthNativeEpoch) return true;
    g_depthHeaps.erase(std::remove_if(g_depthHeaps.begin(),g_depthHeaps.end(),
        [](auto& w) { return w.expired(); }),g_depthHeaps.end());
    if (g_depthHeaps.size() >= kDepthLedgerLimit || !data->base || !data->stride) return false;
    data->epoch = g_depthNativeEpoch;
    if (!depth_store(heap,kDepthHeapOwnerGuid,data)) return false;
    // A retained heap generation owns descriptor provenance. Reset/shutdown removes its
    // private-data owner before releasing the heap, so no address-reuse ABA is possible.
    g_depthHeaps.push_back(data); return true;
}
static std::shared_ptr<DepthHeap> depth_heap_for(ID3D12Device* device, SIZE_T handle) {
    void* identity = native_identity(device);
    std::shared_ptr<DepthHeap> result;
    for (auto& weak : g_depthHeaps) if (auto heap = weak.lock()) {
        if (heap->epoch != g_depthNativeEpoch || heap->device != identity || !heap->contains(handle)) continue;
        if (result) return {}; // ambiguous overlapping heap ranges are never decoded/guessed
        result = heap;
    }
    return result;
}
bool compositor_register_depth_resource(ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
    if (!resource || !depth_capture_active()) return false;
    auto d = resource->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 ||
        d.DepthOrArraySize != 1 || d.MipLevels != 1 || !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
        (d.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) return false;
    // First functional implementation is deliberately restricted to the single-plane
    // 32-bit formats exercised by the GPU harness. Stencil planes need separate states.
    if (d.Format != DXGI_FORMAT_R32_TYPELESS && d.Format != DXGI_FORMAT_D32_FLOAT) return false;
    ID3D12Device* dev=nullptr;
    if (FAILED(resource->GetDevice(__uuidof(ID3D12Device),(void**)&dev))) return false;
    bool selected=native_identity(dev)==native_identity(g_depthAuditDevice); dev->Release();
    if (!selected) return false;
    DepthNativeLock lock;
    if (g_depthResources.count(resource)) return false; // cannot repair unknown state by re-registering
    if (g_depthResources.size() >= kDepthLedgerLimit) return false;
    DepthResource entry; entry.owner = depth_owned(resource); entry.state = state; entry.known = true;
    g_depthResources.emplace(resource,std::move(entry)); return true;
}
static void depth_capture_view(ID3D12Device* dev, ID3D12Resource* resource,
                               const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (!depth_capture_active()) return;
    DepthNativeLock lock;
    auto heap = depth_heap_for(dev,handle.ptr);
    if (!heap) return;
    heap->views.erase(handle.ptr);
    if (!resource || !g_depthResources.count(resource) || !desc ||
        desc->ViewDimension != D3D12_DSV_DIMENSION_TEXTURE2D || desc->Texture2D.MipSlice != 0 ||
        desc->Flags != D3D12_DSV_FLAG_NONE) return;
    DepthView view; view.resource = depth_owned(resource); view.desc = *desc;
    view.generation = ++g_depthNativeSerial; heap->views[handle.ptr] = std::move(view);
}
static void depth_capture_copies(ID3D12Device* dev, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,
    const UINT* sizesD, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* sizesS,
    D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (!depth_capture_active() || type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || !destinations || !sources) return;
    const UINT stride = dev->GetDescriptorHandleIncrementSize(type);
    DepthNativeLock lock;
    std::vector<DepthView> values; std::vector<SIZE_T> slots;
    for (UINT range=0; range<ns; ++range) {
        UINT count = sizesS ? sizesS[range] : 1;
        if (count > kDepthEventLimit-values.size()) {
            for (auto& weak : g_depthHeaps) if (auto heap=weak.lock()) heap->views.clear();
            return;
        }
        for (UINT i=0; i<count; ++i) {
            auto h = sources[range].ptr+(SIZE_T)i*stride; auto heap = depth_heap_for(dev,h);
            values.push_back(heap && heap->views.count(h) ? heap->views.at(h) : DepthView{});
        }
    }
    for (UINT range=0; range<nd; ++range) {
        UINT count = sizesD ? sizesD[range] : 1;
        if (count > kDepthEventLimit-slots.size()) {
            for (auto& weak : g_depthHeaps) if (auto heap=weak.lock()) heap->views.clear();
            return;
        }
        for (UINT i=0; i<count; ++i) slots.push_back(destinations[range].ptr+(SIZE_T)i*stride);
    }
    for (size_t i=0; i<slots.size(); ++i) if (auto heap = depth_heap_for(dev,slots[i])) {
        heap->views.erase(slots[i]);
        if (values.size() == slots.size() && values[i].resource) {
            values[i].generation = ++g_depthNativeSerial; heap->views[slots[i]] = values[i];
        }
    }
}
static std::shared_ptr<DepthRecording> depth_recording(ID3D12GraphicsCommandList* list) {
    auto rec = depth_metadata<DepthRecording>(list,kDepthRecordingGuid);
    return rec && rec->epoch == g_depthNativeEpoch ? rec : nullptr;
}
static void depth_append(const std::shared_ptr<DepthRecording>& rec, DepthEvent event) {
    if (!rec) return;
    if (rec->events.size() >= kDepthEventLimit) { rec->invalid = true; return; }
    rec->events.push_back(std::move(event));
}
static void depth_capture_reset(ID3D12GraphicsCommandList* list) {
    if (!depth_capture_active()) return;
    DepthNativeLock lock;
    auto rec = std::make_shared<DepthRecording>(); rec->epoch = g_depthNativeEpoch; rec->reset = true;
    depth_store(list,kDepthRecordingGuid,rec);
}
static void depth_capture_close(ID3D12GraphicsCommandList* list) {
    if (!depth_capture_active()) return;
    DepthNativeLock lock;
    if (auto rec = depth_recording(list)) rec->closed = true;
}
static void depth_capture_invalid(ID3D12GraphicsCommandList* list) {
    if (!depth_capture_active()) return;
    DepthNativeLock lock;
    if (auto rec = depth_recording(list)) rec->invalid = true;
}
static void depth_capture_barriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    if (!depth_capture_active() || !barriers) return;
    DepthNativeLock lock;
    auto rec = depth_recording(list); if (!rec) return;
    for (UINT i=0; i<count; ++i) {
        auto& b = barriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) { rec->invalid = true; continue; }
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) continue;
        if (!g_depthResources.count(b.Transition.pResource)) continue;
        if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
            (b.Transition.Subresource != 0 && b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) {
            rec->invalid = true; continue;
        }
        DepthEvent event; event.type = DepthEventType::Barrier; event.resource = depth_owned(b.Transition.pResource);
        event.before = b.Transition.StateBefore; event.after = b.Transition.StateAfter;
        depth_append(rec,std::move(event));
    }
}
static std::shared_ptr<ID3D12Resource> depth_resolve(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    ID3D12Device* dev = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device),(void**)&dev))) return {};
    auto heap = depth_heap_for(dev,h.ptr); dev->Release();
    return heap && heap->views.count(h.ptr) ? heap->views.at(h.ptr).resource : nullptr;
}
static void depth_capture_bind(ID3D12GraphicsCommandList* list, const D3D12_CPU_DESCRIPTOR_HANDLE* h) {
    if (!depth_capture_active()) return;
    DepthNativeLock lock;
    auto rec = depth_recording(list); if (!rec) return;
    rec->bound = h ? depth_resolve(list,*h) : nullptr;
    DepthEvent e; e.type = DepthEventType::Bind; e.resource = rec->bound;
    depth_append(rec,std::move(e));
}
static void depth_capture_clear(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE h,
    D3D12_CLEAR_FLAGS flags, UINT rectangles) {
    if (!depth_capture_active()) return;
    DepthNativeLock lock;
    auto rec = depth_recording(list); if (!rec) return;
    auto resource = depth_resolve(list,h);
    if (!resource) { rec->invalid = true; return; }
    DepthEvent e; e.type = DepthEventType::Clear; e.resource = resource;
    e.full = (flags&D3D12_CLEAR_FLAG_DEPTH) && rectangles == 0;
    depth_append(rec,std::move(e));
}
bool compositor_certify_scene_depth(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                                    const NativeSceneDepthCertificate& certificate) {
    if (!list || !resource || !depth_capture_active() || !g_nativeSceneEnabled || !certificate.poseId) return false;
    if (!certificate.width || !certificate.height || certificate.viewportX != 0 || certificate.viewportY != 0 ||
        certificate.viewportMinDepth != 0 || certificate.viewportMaxDepth != 1) return false;
    NativeDepthProjection projection;
    if (!native_depth_projection(certificate.projection,projection)) return false;
    auto desc = resource->GetDesc();
    if (desc.Width != certificate.width || desc.Height != certificate.height ||
        std::abs(projection.aspect-(float)certificate.width/certificate.height) > 0.0002f) return false;
    DepthNativeLock lock;
    auto rec = depth_recording(list);
    if (!rec || rec->closed || rec->invalid || rec->bound.get() != resource || !g_depthResources.count(resource)) return false;
    DepthEvent event; event.type = DepthEventType::Scene; event.resource = depth_owned(resource);
    event.certificate = certificate; depth_append(rec,std::move(event)); return !rec->invalid;
}

static thread_local bool g_depthSubmitting = false;
static thread_local std::vector<DepthRecording> g_depthSubmitted;
static thread_local bool g_depthSubmitUnknown = false, g_depthSubmitHost = false;
using DepthExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static DepthExecuteFn g_depthRealExecute = nullptr;
static void depth_begin_submission(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (!depth_capture_active() || !g_depthRealExecute) return;
    ID3D12Device* device=nullptr;
    if (FAILED(queue->GetDevice(__uuidof(ID3D12Device),(void**)&device))) return;
    bool selected=native_identity(device)==native_identity(g_depthAuditDevice); device->Release();
    if (!selected) return;
    AcquireSRWLockExclusive(&g_depthSubmissionLock);
    g_depthSubmitting = true; g_depthSubmitted.clear(); g_depthSubmitUnknown = false;
    DepthNativeLock lock;
    g_depthSubmitHost = native_identity(queue) == native_identity(g_depthAuditQueue);
    for (UINT i=0; i<count; ++i) {
        ID3D12GraphicsCommandList* list = nullptr;
        if (!lists || !lists[i] || FAILED(lists[i]->QueryInterface(__uuidof(ID3D12GraphicsCommandList),(void**)&list))) {
            g_depthSubmitUnknown = true; continue;
        }
        auto rec = depth_recording(list);
        if (!rec || !rec->reset || !rec->closed || rec->invalid || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            g_depthSubmitUnknown = true;
        else g_depthSubmitted.push_back(*rec); // capture BEFORE driver call / legal metadata Reset
        list->Release();
    }
}
static void depth_invalidate_submitted(const char* reason) {
    for (auto& item : g_depthResources) { item.second.known = false; item.second.initialized = false; }
    g_depthScene = {}; g_depthFallbackReason = reason;
}
static void depth_commit_submission(ID3D12CommandQueue* queue) {
    DepthNativeLock lock;
    ID3D12Device* dev = nullptr;
    const bool deviceHealthy = SUCCEEDED(queue->GetDevice(__uuidof(ID3D12Device),(void**)&dev)) &&
        SUCCEEDED(dev->GetDeviceRemovedReason());
    safe_release(dev);
    if (!deviceHealthy || g_depthSubmitUnknown) {
        depth_invalidate_submitted("unknown/incomplete submission or removed device"); return;
    }
    for (const auto& rec : g_depthSubmitted) for (const auto& event : rec.events) {
        if (rec.epoch != g_depthNativeEpoch) { depth_invalidate_submitted("submission crossed a ledger generation"); return; }
        auto found = g_depthResources.find(event.resource.get());
        if (found == g_depthResources.end()) continue;
        auto& state = found->second;
        if (!g_depthSubmitHost) {
            state.known = false; state.initialized = false; g_depthScene = {};
            g_depthFallbackReason = "depth used on another queue; synchronization unproven"; continue;
        }
        switch(event.type) {
        case DepthEventType::Barrier:
            if (!state.known || state.state != event.before) {
                state.known = false; g_depthScene = {}; g_depthFallbackReason = "unknown or mismatched StateBefore";
            } else state.state = event.after;
            break;
        case DepthEventType::Bind: break;
        case DepthEventType::Clear:
            if (g_depthScene.resource == event.resource) g_depthScene = {};
            if (!state.known || state.state != D3D12_RESOURCE_STATE_DEPTH_WRITE) state.known = false;
            else if (event.full) state.initialized = true;
            break;
        case DepthEventType::Scene:
            if (state.known && state.initialized && state.state == D3D12_RESOURCE_STATE_DEPTH_WRITE) {
                if (g_depthScene.resource && g_depthScene.certificate.poseId == event.certificate.poseId &&
                    g_depthScene.resource != event.resource) {
                    depth_invalidate_submitted("ambiguous scene certificates");
                } else {
                    g_depthScene.resource = event.resource; g_depthScene.certificate = event.certificate;
                    g_depthScene.serial = ++g_depthNativeSerial;
                }
            } else g_depthFallbackReason = "scene lacks initialized, known depth state";
            break;
        default: depth_invalidate_submitted("unsupported depth operation"); break;
        }
    }
}
static void STDMETHODCALLTYPE depth_execute_forward(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    g_depthRealExecute(queue,count,lists);
    if (!g_depthSubmitting) return;
    // Commit after the real call. Keep submission serialization until the tracked GPU
    // order is committed. Never treat a recording as execution or inspect reused metadata.
    depth_commit_submission(queue); g_depthSubmitted.clear(); g_depthSubmitting = false;
    ReleaseSRWLockExclusive(&g_depthSubmissionLock);
}
static void depth_attach_execute() {
    if (g_depthRealExecute) return;
    g_depthRealExecute = (DepthExecuteFn)g_nativeOrig[kSlotExecute];
    InterlockedExchangePointer(&g_nativeOrig[kSlotExecute],(void*)&depth_execute_forward);
}

struct NativeDepthRing {
    ID3D12Resource* snapshot = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    // Explicit raw owned COM reference: if shutdown cannot wait for GPU completion,
    // intentionally retain it across DLL unload instead of running a shared_ptr destructor.
    ID3D12Resource* sourceUntilFence = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
static NativeDepthRing g_depthRings[kRing];
static bool g_nativeDepthSubmitted = false;
static DXGI_FORMAT depth_copy_format(DXGI_FORMAT f) {
    switch(f) {
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
static DXGI_FORMAT depth_sample_format(DXGI_FORMAT f) {
    switch(f) {
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
static void depth_release_snapshots() {
    for (auto& r : g_depthRings) {
        safe_release(r.snapshot); safe_release(r.heap); safe_release(r.sourceUntilFence); r.format = DXGI_FORMAT_UNKNOWN;
    }
    g_nativeDepthSubmitted = false;
}
static D3D12_GPU_DESCRIPTOR_HANDLE depth_table_gpu(UINT ring, int table) {
    auto h = g_depthRings[ring].heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)table*kTableSize*g_srvInc; return h;
}
struct NativeDepthPresentScope {
    bool locked = false;
    explicit NativeDepthPresentScope(bool requested) {
        if (requested) { AcquireSRWLockExclusive(&g_depthSubmissionLock); locked = true; }
    }
    ~NativeDepthPresentScope() { if (locked) ReleaseSRWLockExclusive(&g_depthSubmissionLock); }
};
static bool depth_prepare_snapshot(UINT ring, uint64_t pose, float fov, Params& params) {
    auto& r = g_depthRings[ring];
    // Caller already waited for this ring's compositor fence.
    safe_release(r.sourceUntilFence); g_nativeDepthSubmitted = false;
    DepthNativeLock lock;
    auto scene = g_depthScene;
    // A certificate is single-frame. Repeated Presents or stale Minecraft poses cannot
    // reuse a previous scene's depth, even when the resource identity remains unchanged.
    g_depthScene = {};
    auto found = g_depthResources.find(scene.resource.get());
    auto* st = shm_state();
    if (!scene.resource || found == g_depthResources.end() || !found->second.known || !found->second.initialized) return false;
    const auto& cert = scene.certificate;
    NativeDepthProjection projection;
    if (!pose || cert.poseId != pose || cert.width != g_bbW || cert.height != g_bbH ||
        cert.stageId != st->stageId || cert.hostLife != shm_header()->hostLife ||
        !(st->flags&ERMC_STATE_CAMERA_VALID) || (st->flags&(ERMC_STATE_HOST_BUSY|ERMC_STATE_PLAYER_DEAD)) ||
        !native_depth_projection(cert.projection,projection) ||
        !native_depth_projection_matches(projection,fov,g_bbW,g_bbH)) {
        g_depthFallbackReason = "scene/pose/projection/resize/life mismatch"; return false;
    }
    auto desc = scene.resource->GetDesc();
    DXGI_FORMAT format = depth_copy_format(desc.Format);
    if (format == DXGI_FORMAT_UNKNOWN || desc.Width != g_bbW || desc.Height != g_bbH ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1) {
        g_depthFallbackReason = "unsupported depth format, array, mip or MSAA"; return false;
    }
    if (!r.snapshot || r.format != format || r.snapshot->GetDesc().Width != g_bbW || r.snapshot->GetDesc().Height != g_bbH) {
        safe_release(r.snapshot);
        auto d = tex_desc(g_bbW,g_bbH,format); auto hp = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(g_dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr,__uuidof(ID3D12Resource),(void**)&r.snapshot))) return false;
        r.format = format;
    }
    if (!r.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kTableSize*kTableCount; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dev->CreateDescriptorHeap(&hd,__uuidof(ID3D12DescriptorHeap),(void**)&r.heap))) return false;
    }
    auto dest = r.heap->GetCPUDescriptorHandleForHeapStart();
    g_dev->CopyDescriptorsSimple(kTableSize*kTableCount,dest,g_srvCpuHeap->GetCPUDescriptorHandleForHeapStart(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dest.ptr += (SIZE_T)kSrvHostDepth*g_srvInc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = depth_sample_format(format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    g_dev->CreateShaderResourceView(r.snapshot,&srv,dest);
    // Snapshot copy reads only a proven state, on the actual presenting queue. Restore
    // exactly that state so the host's already recorded next-frame barriers stay valid.
    auto before = found->second.state;
    transition(scene.resource.get(),before,D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(r.snapshot,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->CopyResource(r.snapshot,scene.resource.get());
    transition(r.snapshot,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(scene.resource.get(),D3D12_RESOURCE_STATE_COPY_SOURCE,before);
    r.sourceUntilFence = scene.resource.get(); r.sourceUntilFence->AddRef();
    params.pad0 = projection.a; params.pad1 = projection.b; params.pad2 = 1;
    params.hostNear = projection.nearZ;
    params.hostFar = std::isfinite(projection.farZ) ? projection.farZ : 1e8f;
    params.hostReversed = projection.reversed ? 1.0f : 0.0f;
    params.useDepth = 1; params.debugView = 0;
    g_depthFallbackReason = "certified scene snapshot active"; g_nativeDepthSubmitted = true;
    return true;
}
