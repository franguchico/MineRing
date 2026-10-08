#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

// Versioned local ABI. All pointers are borrowed for the duration of the call.
// Render is called after ReShade submitted its immediate list on this exact queue;
// the exported R32F depth is in combined shader-resource state, backbuffer in PRESENT.
enum ErmcDepthEvent : unsigned { ErmcDepthRender = 0, ErmcDepthResize = 1 };
using ErmcDepthCallback = void (*)(unsigned version, unsigned event, IDXGISwapChain3*,
    ID3D12CommandQueue*, ID3D12Device*, ID3D12Resource*, unsigned long long generation);
