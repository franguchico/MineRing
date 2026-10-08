#pragma once
#include <cmath>
#include <cstdint>

namespace mb {
struct NativeDepthProjection {
    float a = 0, b = 0, nearZ = 0, farZ = 0, fovY = 0, aspect = 0;
    bool reversed = false;
};
// Reject orthographic, jittered/oblique, scaled-w and otherwise unsupported matrices.
// Infinite-far perspective is supported. Depth is linearized using actual coefficients.
inline bool native_depth_projection(const float* m, NativeDepthProjection& p) {
    if (!m) return false;
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false;
    const int zero[] = {1,2,3,4,6,7,8,9,12,13,15};
    for (int i : zero) if (std::abs(m[i]) > 1e-5f) return false;
    if (m[0] <= 0 || m[5] <= 0 || std::abs(std::abs(m[11])-1.0f) > 1e-5f) return false;
    p.a = m[10] / m[11]; p.b = m[14];
    if (std::abs(p.b) < 1e-8f) return false;
    float d0 = std::abs(p.a) < 1e-8f ? INFINITY : p.b / -p.a;
    float d1 = std::abs(1-p.a) < 1e-8f ? INFINITY : p.b / (1-p.a);
    if (!(d0 > 0 && d1 > 0) || d0 == d1) return false;
    p.reversed = d0 > d1;
    p.nearZ = p.reversed ? d1 : d0; p.farZ = p.reversed ? d0 : d1;
    p.fovY = 2.0f * std::atan(1.0f/m[5]) * 180.0f/3.14159265359f;
    p.aspect = m[5]/m[0];
    return p.nearZ >= 0.001f && p.nearZ <= 10 && p.farZ > p.nearZ &&
           p.fovY > 5 && p.fovY < 170 && std::isfinite(p.aspect) && p.aspect > 0;
}
inline bool native_depth_projection_matches(const NativeDepthProjection& p, float fovY, uint32_t w, uint32_t h) {
    return w && h && std::isfinite(fovY) && std::abs(p.fovY-fovY) <= 0.02f &&
           std::abs(p.aspect-(float)w/h) <= 0.0002f;
}
} // namespace mb
