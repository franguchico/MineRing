"""Compile and execute the production copy_slot body with CPU-only upload fixtures.

Run from an MSVC developer prompt, or with clang++/g++ on PATH. No full native
build, D3D12 device, window, IPC file or game startup. Both backend branches run.
This verifies upload reuse/seqlock behavior; actual GPU submission needs the
separate compositor harness after the main agent's coordinated native build.
"""
from pathlib import Path
import os
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get("ERMC_UPLOAD_SOURCE", root / "src/compositor.cpp")).read_text(encoding="utf-8")
compiler = next((path for name in ("cl.exe", "cl", "clang++", "g++")
                 if (path := shutil.which(name))), None)
if compiler is None and os.environ.get("VCToolsInstallDir"):
    vc_compiler = Path(os.environ["VCToolsInstallDir"]) / "bin/Hostx64/x64/cl.exe"
    if vc_compiler.is_file():
        compiler = str(vc_compiler)
if compiler is None:
    raise SystemExit("Run from an MSVC developer prompt, or put clang++/g++ on PATH")


def definition(start):
    begin = source.index(start)
    brace = source.index("{", begin)
    nesting = 1
    end = brace + 1
    while nesting:
        nesting += (source[end] == "{") - (source[end] == "}")
        end += 1
    if source[end:end + 1] == ";":
        end += 1
    return source[begin:end]


production = definition("struct FrameUploadStamp {") + "\n"
production += "static FrameUploadStamp g_uploadedFrame;\n"
for helper in ("static bool capture_frame_stamp(", "static bool same_frame_upload(",
               "static bool uploaded_frame_matches("):
    if helper in source:
        production += definition(helper) + "\n"
production += definition("static bool copy_slot(") + "\n"
production += definition("static void commit_uploaded_frame(") + "\n"
if "static bool frame_fence_ready(" in source:
    production += definition("static bool frame_fence_ready(") + "\n"
    production += definition("static bool frame_resources_idle(") + "\n"
fixture = r'''
#include "bridge_protocol.h"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <algorithm>
using UINT = uint32_t;
struct RingSlot { uint8_t* uploadPtr; uint64_t fence = 0; };
#if ERMC_UPLOAD_IDENTITY_TEST
struct FakeFence { uint64_t completed = 0; unsigned polls = 0; uint64_t GetCompletedValue() { ++polls; return completed; } };
static FakeFence* g_fence = nullptr;
static bool g_failed = false;
static RingSlot g_ring[3]{};
#endif
struct PlacedFootprint { uint64_t Offset; struct { UINT RowPitch; } Footprint; };
static PlacedFootprint g_fp[4];
static UINT g_texW = 3, g_texH = 2;
static uint64_t g_layerBytes = 24;
static bool g_haveFrame = false;
static bool g_texHand = false;
#if ERMC_NATIVE_WINDOWS
static uint64_t g_uploadedPose = 0;
static float g_uploadedFov = 0, g_uploadedNear = 0, g_uploadedFar = 0;
#endif
static size_t copiedBytes = 0, copyCalls = 0;
static ErmcFrameHeader* rewriteDuringCopy = nullptr;
static void compiler_barrier() { std::atomic_signal_fence(std::memory_order_seq_cst); }
static void* traced_copy(void* dst, const void* src, size_t bytes) {
    copiedBytes += bytes; ++copyCalls;
    if (rewriteDuringCopy) {
        rewriteDuringCopy->seq = rewriteDuringCopy->seq + 2;
        rewriteDuringCopy = nullptr;
    }
    return std::memcpy(dst, src, bytes);
}
#define memcpy traced_copy
#include "production_upload.inc"
#undef memcpy
struct FrameFixture {
    ErmcFrameHeader header{};
    uint8_t padding[ERMC_FRAME_HDR - sizeof(ErmcFrameHeader)]{};
    uint8_t pixels[512]{};
};
static_assert(offsetof(FrameFixture, pixels) == ERMC_FRAME_HDR, "Real slot payload offset");
int main() {
    FrameFixture slot;
    auto& h = slot.header;
    h.seq = 2; h.frameId = 7; h.poseId = 7; h.width = 3; h.height = 2;
    h.flags = ERMC_FRAME_HAND; h.mcNear = 0.05f; h.mcFar = 500; h.fovYDeg = 60;
    for (size_t i = 0; i < sizeof(slot.pixels); ++i) slot.pixels[i] = uint8_t(i);
    uint8_t upload[256]; std::memset(upload, 0xcd, sizeof(upload));
    for (UINT i = 0; i < 4; ++i) g_fp[i] = {uint64_t(i) * 48, {16}};
    RingSlot ring{upload};
    bool hand = false;
    FrameUploadStamp captured{};
    assert(copy_slot(&h, ring, &hand, &captured) && hand);
    assert(captured.slot == &h && captured.seq == 2 && captured.frameId == 7);
    assert(copiedBytes == 96 && copyCalls == 8);
    for (UINT layer = 0; layer < 4; ++layer) {
        for (UINT row = 0; row < 2; ++row) {
            auto* dst = upload + g_fp[layer].Offset + row * 16;
            assert(std::memcmp(dst, slot.pixels + layer * 24 + row * 12, 12) == 0);
            for (UINT x = 12; x < 16; ++x) assert(dst[x] == 0xcd);
        }
        for (UINT x = 32; x < 48; ++x) assert(upload[layer * 48 + x] == 0xcd);
    }
#if ERMC_NATIVE_WINDOWS
    assert(g_uploadedPose == 0 && captured.poseId == 7 && captured.fov == 60 && captured.farZ == 500);
#endif
    // The compositor commits this stamp only after successful submission + Signal.
    commit_uploaded_frame(captured, hand);
    copiedBytes = copyCalls = 0;
    for (int i = 0; i < 100000; ++i) assert(!copy_slot(&h, ring, &hand, &captured));
    assert(copiedBytes == 0 && copyCalls == 0);
    assert(g_uploadedFrame.seq == 2 && g_uploadedFrame.frameId == 7);

    // Same frame ID after a publication/restart is new work, even with the same pose.
    h.seq = 4; h.flags = 0; h.mcFar = 750; slot.pixels[0] = 99;
    assert(copy_slot(&h, ring, &hand, &captured) && !hand);
    assert(copiedBytes == 72 && captured.seq == 4 && upload[0] == 99);
    assert(g_uploadedFrame.seq == 2);  // copy alone does not commit reusable state
    commit_uploaded_frame(captured, hand);
#if ERMC_NATIVE_WINDOWS
    assert(g_uploadedFar == 750);
#endif
    copiedBytes = copyCalls = 0;
    assert(!copy_slot(&h, ring, &hand, &captured));
    assert(copiedBytes == 0);

    h.frameId = 8;
    assert(copy_slot(&h, ring, &hand, &captured));
    commit_uploaded_frame(captured, hand);
    FrameFixture another = slot;
    assert(copy_slot(&another.header, ring, &hand, &captured));
    assert(captured.slot == &another.header);

    // Slot mutation during the real memcpy path must not stage a successful stamp.
    captured = {}; h.seq = 6; h.poseId = 9;
    rewriteDuringCopy = &h;
    assert(!copy_slot(&h, ring, &hand, &captured));
    assert(captured.slot == nullptr && g_uploadedFrame.seq == 4);
#if ERMC_NATIVE_WINDOWS
    assert(g_uploadedPose == 7);
#endif
    assert(copy_slot(&h, ring, &hand, &captured));
    assert(captured.seq == 8);
    commit_uploaded_frame(captured, hand);

    // Cold/expired producer or recreated textures must force an upload of identical IDs.
    g_haveFrame = false;
    assert(copy_slot(&h, ring, &hand, &captured));
    g_haveFrame = true; g_uploadedFrame = {};
    assert(copy_slot(&h, ring, &hand, &captured));
    commit_uploaded_frame(captured, hand);

    h.seq = 9; copiedBytes = copyCalls = 0;
    assert(!copy_slot(&h, ring, &hand, &captured) && copiedBytes == 0);
    h.seq = 10; h.width = 4;
    assert(!copy_slot(&h, ring, &hand, &captured) && copiedBytes == 0);
    h.width = 3; h.height = 3;
    assert(!copy_slot(&h, ring, &hand, &captured) && copiedBytes == 0);

    // Packed rows take the whole-layer fast path as well.
    h.height = 2; g_haveFrame = false;
    for (UINT i = 0; i < 4; ++i) g_fp[i] = {uint64_t(i) * 24, {12}};
    assert(copy_slot(&h, ring, &hand, &captured));
    assert(copiedBytes == 72 && copyCalls == 3);
    // Exact passive frame matching is checked inside the seqlock copy, including
    // a slot replaced between selection and copying, with no depth-buffer fallback.
    h.seq = 12; h.flags = 1; h.frameId = 20; h.poseId = 20;
    FrameUploadStamp pending{};
    const auto previous = g_uploadedFrame;
    assert(!copy_slot(&h, ring, &hand, &pending, 19));
    assert(pending.slot == nullptr && g_uploadedFrame.frameId == previous.frameId);
    h.flags = 3;
    assert(!copy_slot(&h, ring, &hand, &pending, 20));
    h.flags = 1;
    assert(copy_slot(&h, ring, &hand, &pending, 20));
    assert(pending.poseId == 20 && g_uploadedFrame.poseId == previous.poseId);
#if ERMC_NATIVE_WINDOWS
    assert(g_uploadedPose == previous.poseId); // failed allocator/reset/close: never commit candidate
#endif
    rewriteDuringCopy = &h;
    pending = {};
    assert(!copy_slot(&h, ring, &hand, &pending, 20));
    assert(pending.slot == nullptr && g_uploadedFrame.poseId == previous.poseId);
    assert(copy_slot(&h, ring, &hand, &pending, 20));
    commit_uploaded_frame(pending, hand); // successful submission only
    assert(g_uploadedFrame.poseId == 20 && g_haveFrame && !g_texHand);
#if ERMC_NATIVE_WINDOWS
    assert(g_uploadedPose == 20 && g_uploadedFar == h.mcFar);
#endif
    // Every identity field used by projection, mode, dimensions or depth belongs
    // to the uploaded publication. Do not conflate duplicates and torn reads.
#if ERMC_UPLOAD_IDENTITY_TEST
    assert(frame_fence_ready(0) && !frame_fence_ready(1));
    FakeFence fake; g_fence = &fake;
    g_ring[0].fence = 2; g_ring[1].fence = 5; g_ring[2].fence = 3;
    fake.completed = 4; assert(!frame_resources_idle());
    fake.completed = 5; assert(frame_resources_idle());
    fake.completed = UINT64_MAX; assert(!frame_fence_ready(5) && g_failed);
    assert(fake.polls == 3); g_fence = nullptr; g_failed = false;
    puts("PASS: fence polling rejects pending/latest-ring and removed-device sentinel; no wait API in fixture");
    assert(uploaded_frame_matches(&h, 20));
    FrameUploadStamp picked{};
    assert(capture_frame_stamp(&h, &picked));
    ++h.seq;
    assert(!uploaded_frame_matches(&h, 20));
    assert(!copy_slot(&h, ring, &hand, &pending, 20, &picked));
    ++h.seq;
    assert(!uploaded_frame_matches(&h, 20));
    assert(copy_slot(&h, ring, &hand, &pending, 20));
    commit_uploaded_frame(pending, hand);
    copiedBytes = copyCalls = 0;
    h.mcNear += .01f;
    assert(!uploaded_frame_matches(&h, 20));
    assert(copy_slot(&h, ring, &hand, &pending, 20) && copiedBytes == 48);
    commit_uploaded_frame(pending, hand);
    h.mcFar += 1; assert(!uploaded_frame_matches(&h, 20));
    h.mcFar -= 1; h.fovYDeg += 1; assert(!uploaded_frame_matches(&h, 20));
    h.fovYDeg -= 1; h.aspect = 1.5f; assert(!uploaded_frame_matches(&h, 20));
    assert(copy_slot(&h, ring, &hand, &pending, 20)); commit_uploaded_frame(pending, hand);
    h.poseId = 21; assert(!uploaded_frame_matches(&h, 20));
    h.poseId = 20; h.flags = 3; assert(!uploaded_frame_matches(&h, 20));
    h.flags = 1; h.width = 0xffffffffu;
    assert(!capture_frame_stamp(&h, &picked));
    h.width = 3; h.height = ERMC_FRAME_MAX_H + 1;
    assert(!capture_frame_stamp(&h, &picked));
    puts("PASS: complete upload identity; odd/new publication cannot authorize reuse; bounded malformed header rejection");
#endif
    // Real full-size CPU copies through production code, with byte counts and
    // median timings. Both branches preserve the full float32 depth plane.
    const UINT qw = 2560, qh = 1440;
    g_texW = qw; g_texH = qh; g_layerBytes = uint64_t(qw) * qh * 4;
    std::vector<uint8_t> publication(ERMC_FRAME_HDR + size_t(g_layerBytes) * 4, 0x5a);
    auto* q = reinterpret_cast<ErmcFrameHeader*>(publication.data());
    q->seq = 2; q->width = qw; q->height = qh; q->flags = 1;
    q->frameId = 50; q->poseId = 50; q->fovYDeg = 60; q->mcNear = .05f; q->mcFar = 1000;
    std::vector<uint8_t> destination(size_t(g_layerBytes) * 4, 0xcd);
    RingSlot large{destination.data()};
    for (UINT i = 0; i < 4; ++i) g_fp[i] = {g_layerBytes * i, {qw * 4}};
    for (int passive = 0; passive < 2; ++passive) {
        std::vector<double> times;
        copiedBytes = copyCalls = 0;
        if (passive) std::memset(destination.data() + 2 * g_layerBytes, 0xcd, size_t(g_layerBytes) * 2);
        for (int i = 0; i < 31; ++i) {
            g_haveFrame = false; q->seq += 2; ++q->frameId;
            auto start = std::chrono::steady_clock::now();
            assert(copy_slot(q, large, &hand, &pending, passive ? 50 : 0));
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        printf("QHD CPU native=%d passive=%d bytes/copy=%zu calls/copy=%zu median_ms=%.4f max_ms=%.4f\n",
            ERMC_NATIVE_WINDOWS, passive, copiedBytes / 31, copyCalls / 31, times[15], times.back());
        assert(std::memcmp(destination.data() + g_layerBytes, publication.data() + ERMC_FRAME_HDR + g_layerBytes,
            size_t(g_layerBytes)) == 0); // depth is copied losslessly
#if ERMC_UPLOAD_IDENTITY_TEST
        if (passive) {
            assert(copiedBytes == 31 * 2 * g_layerBytes && copyCalls == 31 * 2);
            for (size_t i = size_t(2 * g_layerBytes); i < destination.size(); ++i) assert(destination[i] == 0xcd);
        }
#endif
    }
    std::puts("PASS: production upload copy: 100000 duplicate presentations copy 0 bytes; "
        "republication, frame/slot identity, producer loss, texture reset, seqlock races, "
        "hand transitions, row padding and packed rows");
}
'''

for native in (0, 1):
    work = Path(os.environ.get("ERMC_UPLOAD_WORKDIR", root / "build-upload-contract")) / str(native)
    work.mkdir(parents=True, exist_ok=True)
    (work / "production_upload.inc").write_text(production, encoding="utf-8")
    test = work / "upload_contract.cpp"
    test.write_text(fixture, encoding="utf-8")
    binary = work / ("upload_contract.exe" if os.name == "nt" else "upload_contract")
    if Path(compiler).stem.lower() == "cl":
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/O2", "/W4", "/WX",
                   f"/DERMC_UPLOAD_IDENTITY_TEST={int('static bool capture_frame_stamp(' in source)}",
                   f"/DERMC_NATIVE_WINDOWS={native}", f"/I{root / 'include'}",
                   f"/Fo{work / 'upload_contract.obj'}", f"/Fe{binary}", str(test)]
    else:
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-O2", f"-DERMC_UPLOAD_IDENTITY_TEST={int('static bool capture_frame_stamp(' in source)}",
                   f"-DERMC_NATIVE_WINDOWS={native}", "-I", str(root / "include"),
                   "-o", str(binary), str(test)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(binary)], cwd=work, check=True)
