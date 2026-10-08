#pragma once
#include <d3d12.h>
#include <stdint.h>

namespace mb {
// An ENGINE adapter must identify the final main-camera scene pass and the actual
// projection used by that pass. Dimensions, a clear value, a resource name, a user
// depth index and an observed StateAfter are deliberately NOT scene certificates.
// These calls use only public COM APIs; they never decode CPU descriptor memory.
// Register creation-time resource state and heap identity before creating its DSV.
// Pre-injection resources require an adapter with equivalent authoritative evidence.
struct NativeSceneDepthCertificate {
    uint64_t poseId = 0;
    uint32_t stageId = 0, hostLife = 0;
    UINT width = 0, height = 0;
    // This implementation supports a full-screen viewport with D3D depth range [0,1].
    // The adapter must copy the actual viewport, not assume it from resource dimensions.
    float viewportX = 0, viewportY = 0, viewportMinDepth = 0, viewportMaxDepth = 1;
    // Row-vector perspective projection: clip.z = z*m[10]+m[14], clip.w=z*m[11].
    // Must be the matrix consumed by the host scene shader, not a guessed camera matrix.
    float projection[16] = {};
};
bool compositor_register_depth_heap(ID3D12DescriptorHeap* heap);
bool compositor_register_depth_resource(ID3D12Resource* resource, D3D12_RESOURCE_STATES creationState);
bool compositor_certify_scene_depth(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                                    const NativeSceneDepthCertificate& certificate);
void compositor_enable_native_depth(bool enabled);
} // namespace mb
