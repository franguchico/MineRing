// ReShade 6.8.0 public API adapter. GenericDepth chooses the host scene heuristically.
// A dedicated raw-depth pass avoids borrowing GenericDepth's private state/layout.
#include "../third_party/reshade/include/reshade.hpp"
#include "reshade_bridge_api.h"
#include <cstdio>
#define ERMC_RESHADE_ASPECT_ONLY
#include "compositor_reshade.h"
#undef ERMC_RESHADE_ASPECT_ONLY

// Validate the DEPTH input, not ErmcRawDepth: the export target is always resized to
// the backbuffer, even when GenericDepth picked an incompatible source viewport.
static bool input_depth_aspect_valid(reshade::api::effect_runtime* rt) {
    using namespace reshade::api;
    auto variable = rt->find_texture_variable("ErmcDepth.fx", "ErmcHostDepth");
    resource_view view = {};
    if (variable != 0) rt->get_texture_binding(variable, &view, nullptr);
    if (view == 0) return false;
    auto* device = rt->get_device();
    auto source = device->get_resource_from_view(view), backbuffer = rt->get_current_back_buffer();
    if (source == 0 || backbuffer == 0) return false;
    auto desc = device->get_resource_desc(source), target = device->get_resource_desc(backbuffer);
    auto srv = device->get_resource_view_desc(view);
    if (desc.type != resource_type::texture_2d || target.type != resource_type::texture_2d ||
        srv.type != resource_view_type::texture_2d || desc.texture.samples != 1 ||
        desc.texture.depth_or_layers != 1 || !desc.texture.width || !desc.texture.height || srv.texture.first_layer != 0 ||
        srv.texture.first_level >= desc.texture.levels || srv.texture.first_level >= 32) return false;
    uint32_t width = desc.texture.width >> srv.texture.first_level;
    uint32_t height = desc.texture.height >> srv.texture.first_level;
    return reshade_aspect_matches(width ? width : 1, height ? height : 1,
        target.texture.width, target.texture.height);
}

struct __declspec(uuid("c9ab4584-0a33-49bf-86a1-b7b6f0a8c202")) Frame {
    bool bracket = false, exported = false;
    unsigned long long generation = 0;
};
static ErmcDepthCallback bridge() {
    static ErmcDepthCallback callback = nullptr;
    static ULONGLONG last = 0;
    if (callback) return callback;
    if (GetTickCount64() - last < 1000) return nullptr;
    last = GetTickCount64();
    HMODULE modules[1024]; DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return nullptr;
    for (DWORD i = 0; i < (bytes < sizeof(modules) ? bytes : sizeof(modules))/sizeof(HMODULE); ++i) {
        auto fn = GetProcAddress(modules[i], "erb_reshade_depth_v2");
        if (!fn) continue;
        // Native bridge hot reload is disabled. Pin its code for callback lifetime;
        // core shutdown marks its entry points stopped before freeing GPU resources.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(fn), &pinned)) continue;
        callback = reinterpret_cast<ErmcDepthCallback>(fn);
        reshade::log::message(reshade::log::level::info, "ERMC: connected depth adapter to bridge");
        break;
    }
    return callback;
}
static void init(reshade::api::effect_runtime* rt) { rt->create_private_data<Frame>(); }
static void destroy(reshade::api::effect_runtime* rt) { rt->destroy_private_data<Frame>(); }
static void begin(reshade::api::effect_runtime* rt, reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view) {
    auto* f = rt->get_private_data<Frame>();
    if (f) { f->bracket = true; f->exported = false; }
}
static void finish(reshade::api::effect_runtime* rt, reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view) {
    if (auto* f = rt->get_private_data<Frame>()) f->bracket = false;
}
static void technique(reshade::api::effect_runtime* rt, reshade::api::effect_technique tech,
    reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view) {
    auto* f = rt->get_private_data<Frame>();
    if (!f || !f->bracket || tech != rt->find_technique("ErmcDepth.fx", "ErmcDepthExport")) return;
    auto ready = rt->find_uniform_variable("ErmcDepth.fx", "ErmcDepthReady");
    bool available = false;
    if (ready != 0) rt->get_uniform_value_bool(ready, &available, 1);
    f->exported = available && input_depth_aspect_valid(rt);
}
static void present(reshade::api::effect_runtime* rt) {
    auto* f = rt->get_private_data<Frame>();
    if (!f || rt->get_device()->get_api() != reshade::api::device_api::d3d12) return;
    bool exported = f->exported;
    f->exported = false; f->bracket = false;
    auto fn = bridge();
    if (!fn) return;
    ID3D12Resource* source = nullptr;
    if (exported) {
        auto variable = rt->find_texture_variable("ErmcDepth.fx", "ErmcRawDepth");
        reshade::api::resource_view view = {};
        if (variable != 0) rt->get_texture_binding(variable, &view, nullptr);
        if (view != 0) source = reinterpret_cast<ID3D12Resource*>(rt->get_device()->get_resource_from_view(view).handle);
    }
    if (f->generation % 300 == 0) {
        auto tech = rt->find_technique("ErmcDepth.fx", "ErmcDepthExport");
        char message[256];
        auto desc = source ? source->GetDesc() : D3D12_RESOURCE_DESC{};
        std::snprintf(message, sizeof(message), "ERMC depth: technique=%llu enabled=%d exported=%d source=%p size=%llux%u format=%u",
            tech.handle, tech != 0 && rt->get_technique_state(tech), exported, source, desc.Width, desc.Height, unsigned(desc.Format));
        reshade::log::message(reshade::log::level::info, message);
    }
    // At reshade_present the overlay/effects and final PRESENT transitions are recorded.
    // Flush those before submitting our own list. No native bindings touch ReShade's list.
    auto* queue = rt->get_command_queue();
    queue->flush_immediate_command_list();
    fn(2, ErmcDepthRender, reinterpret_cast<IDXGISwapChain3*>(rt->get_native()),
        reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()),
        reinterpret_cast<ID3D12Device*>(rt->get_device()->get_native()), source, ++f->generation);
}
static void resize(reshade::api::swapchain* sc, bool) {
    if (sc->get_device()->get_api() != reshade::api::device_api::d3d12) return;
    if (auto fn = bridge()) fn(2, ErmcDepthResize, reinterpret_cast<IDXGISwapChain3*>(sc->get_native()), nullptr, nullptr, nullptr, 0);
}
extern "C" __declspec(dllexport) const char* NAME = "EldenMinecraft Depth";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Experimental host-depth occlusion for the offline Minecraft bridge.";
extern "C" __declspec(dllexport) unsigned ermc_depth_adapter_version() { return 2; }
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        if (!reshade::register_addon(module)) return FALSE;
        reshade::register_event<reshade::addon_event::init_effect_runtime>(init);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(destroy);
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(begin);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(finish);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(technique);
        reshade::register_event<reshade::addon_event::reshade_present>(present);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(resize);
    } else if (reason == DLL_PROCESS_DETACH) reshade::unregister_addon(module);
    return TRUE;
}
