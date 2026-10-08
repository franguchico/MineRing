// Shared with the public-API adapter; tolerate pixel rounding, not a different frustum.
static bool reshade_aspect_matches(uint64_t width, uint64_t height, uint64_t targetWidth, uint64_t targetHeight) {
    if (!width || !height || !targetWidth || !targetHeight) return false;
    double actual = double(width) * double(targetHeight), expected = double(targetWidth) * double(height);
    double delta = actual > expected ? actual - expected : expected - actual;
    return delta <= expected * 0.001;
}

#ifndef ERMC_RESHADE_ASPECT_ONLY
// Included after the native backend. Invoked only under g_renderLock on ReShade's
// presenting thread. Unlike the strict certificate path this uses GenericDepth's
// heuristic selection and the final ER camera's conventional reversed-Z projection.
static ID3D12Resource* g_reshadeDepth = nullptr;
static ID3D12Device* g_reshadeDevice = nullptr;
static uint64_t g_reshadeGeneration = 0;
static bool reshade_adapter_loaded() {
    HMODULE module = GetModuleHandleW(L"ErmcDepth.addon64");
    return module && GetProcAddress(module, "ermc_depth_adapter_version");
}
static bool reshade_depth_available() {
    auto* source = g_reshadeDepth;
    if (!source || !g_reshadeGeneration) return false;
    auto* st = shm_state();
    if (!(st->flags & ERMC_STATE_CAMERA_VALID) || (st->flags & (ERMC_STATE_HOST_BUSY | ERMC_STATE_PLAYER_DEAD))) return false;
    auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Format != DXGI_FORMAT_R32_FLOAT ||
        desc.Width != g_bbW || desc.Height != g_bbH || desc.DepthOrArraySize != 1 ||
        desc.MipLevels != 1 || desc.SampleDesc.Count != 1) return false;
    // ReShade hooks Resource::GetDevice to return its proxy, whereas the native
    // swapchain/queue expose the original device. Compare the producer's public
    // api::device::get_native() identity; the resource came from that same API
    // device's texture binding in this callback, not a global guessed pointer.
    if (!g_reshadeDevice || native_identity(g_reshadeDevice) != native_identity(g_dev)) return false;
    return true;
}
// Caller has validated the borrowed source, pose and aspect under g_renderLock.
static bool reshade_prepare_snapshot(UINT ring, Params& params, const ErmcControl& ctrl) {
    auto* source = g_reshadeDepth;
    auto& r = g_depthRings[ring]; // caller has waited for this ring's GPU fence
    safe_release(r.sourceUntilFence);
    if (!r.snapshot || r.format != DXGI_FORMAT_R32_FLOAT || r.snapshot->GetDesc().Width != g_bbW || r.snapshot->GetDesc().Height != g_bbH) {
        safe_release(r.snapshot);
        auto d = tex_desc(g_bbW, g_bbH, DXGI_FORMAT_R32_FLOAT);
        auto hp = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr, __uuidof(ID3D12Resource), (void**)&r.snapshot))) return false;
        r.format = DXGI_FORMAT_R32_FLOAT;
    }
    if (!r.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kTableSize*kTableCount; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&r.heap))) return false;
    }
    auto dest = r.heap->GetCPUDescriptorHandleForHeapStart();
    g_dev->CopyDescriptorsSimple(kTableSize*kTableCount, dest, g_srvCpuHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dest.ptr += SIZE_T(kSrvHostDepth)*g_srvInc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    g_dev->CreateShaderResourceView(r.snapshot, &srv, dest);
    constexpr auto readState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition(source, readState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(r.snapshot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->CopyResource(r.snapshot, source);
    transition(r.snapshot, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, readState);
    r.sourceUntilFence = source; source->AddRef();
    params.useDepth = 1.0f;
    params.debugView = (ctrl.flags & ERMC_CTRL_DEBUG_DEPTH) ? 1.0f : 0.0f;
    params.pad2 = 0.0f;
    static uint64_t last = 0;
    if (now_ms() - last > 5000) {
        last = now_ms();
        log("depth: ReShade raw scene snapshot %p %ux%u frame %llu near %.5f far %.2f reversed %.0f (experimental GenericDepth selection)",
            source, g_bbW, g_bbH, (unsigned long long)g_reshadeGeneration, params.hostNear, params.hostFar, params.hostReversed);
    }
    return true;
}
#endif
