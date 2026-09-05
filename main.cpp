#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <d3d11.h> // native interop for the GPU preview processor (mpr_preview.inc, DX11 only)

// ---------------------------------------------------------------------------
// Target ReShade addon API version.
//
// This is the version we REGISTER as at runtime (see register_addon_compat)
// AND the version the `#if RESHADE_API_VERSION >= N` guards below compile against.
//   - Lower  = loads on more / older ReShade hosts, but fewer API features.
//   - Higher = more API features, but requires a newer ReShade host.
//
// The SDK header (deps/reshade) advertises a higher version, but we deliberately
// pin ourselves so ONE binary loads across every game we ship to. v13 is the
// floor that enables everything this addon actually uses:
//   v11 reshade_set_current_preset_path event
//   v12 get_reshade_base_path
//   v13 get/set_config_value (settings persistence)
// ...while still loading on any ReShade 6.x host. Override at build time for
// legacy 5.x hosts: cmake ... -DMPR_TARGET_API_VERSION=8
// ---------------------------------------------------------------------------
#ifndef MPR_TARGET_API_VERSION
#define MPR_TARGET_API_VERSION 13
#endif
#undef RESHADE_API_VERSION
#define RESHADE_API_VERSION MPR_TARGET_API_VERSION

// Local implementation of register_addon to ensure RESHADE_API_VERSION 8 is used 
// (the one in reshade.hpp is inline and might have captured its own default version)
static bool register_addon_compat(HMODULE addon_module, HMODULE reshade_module) {
    if (reshade_module == nullptr) return false;
    const auto func = reinterpret_cast<bool(*)(void *, uint32_t)>(GetProcAddress(reshade_module, "ReShadeRegisterAddon"));
    if (func == nullptr || !func(addon_module, RESHADE_API_VERSION)) return false;
#if defined(IMGUI_VERSION_NUM)
    const auto imgui_func = reinterpret_cast<const imgui_function_table *(*)(uint32_t)>(GetProcAddress(reshade_module, "ReShadeGetImGuiFunctionTable"));
    if (imgui_func != nullptr) {
        imgui_function_table_instance() = imgui_func(IMGUI_VERSION_NUM);
    }
#endif
    return true;
}
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdarg>   // enc_fail's varargs reporting (mpr_capture.inc)
#include <new>       // std::bad_alloc — acquire_buf degrades to a dropped frame instead of unwinding
#include <atomic>
#include <ctime>
#include <chrono>
#include <cmath>
#include <algorithm>

using namespace reshade::api;


// ---------------------------------------------------------------------------
// Unity build. The addon body is split into topical fragments, #included below in
// declaration order (each fragment relies on the ones before it — order matters).
// This is a pure file-layout split: the preprocessed translation unit is identical
// to the former single-file main.cpp. See CLAUDE.md > 'Source layout'.
// ---------------------------------------------------------------------------
#include "mpr_record_pass.h"
#include "mpr_theme.inc"
#include "mpr_format_util.inc"
#include "mpr_state.inc"
#include "mpr_hotkeys.inc"
#include "mpr_config.inc"
#include "mpr_capture.inc"
#include "mpr_livert.inc"
#include "mpr_fxlayer.inc"
#include "mpr_native.inc"
#include "mpr_preview.inc"
#include "mpr_waveform.inc"
#include "mpr_tiles.inc"
#include "mpr_shot.inc"
#include "mpr_control.inc"
#include "mpr_osd.inc"
#include "mpr_ui.inc"

extern "C" __declspec(dllexport) const char* NAME = "MultipassRecorder";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Multipass synchronized capture";
extern "C" __declspec(dllexport) bool AddonInit(HMODULE a, HMODULE r) {
    if (!register_addon_compat(a, r)) return false;

    // ---- Tier 1: UNIVERSAL passes (ReShade .fx textures — work on ANY game, no profile) ----
    // depth: linearized by the addon's own compute shader from the raw depth buffer that
    // MultipassRecorder.fx reflects into texDepthRaw. r32_float internally, so stills/EXR and the
    // Viewfinder carry full precision; BITS_10 because NVENC's p010le is the only path that lets
    // any of that survive into video (8-bit would throw it away again — the old .fx's whole bug).
    g_passes[0].init("depth", "", "rgba", 4, SRC_NATIVE_DEPTH);
    g_passes[0].enc_chroma = CHROMA_LUMA;                          // depth is grayscale -> luma encode
    g_passes[0].enc_bits   = BITS_10;
    g_passes[1].init("normals", "", "rgba", 4, SRC_NATIVE_NORMALS); // reconstructed from depth by the addon
    // world: the game's own frame, copied by the addon at begin_effects — BEFORE ReShade's effect
    // chain, so it is a clean plate (the game's HUD is still in it; that's the game's own drawing).
    // Default-off. For linear HDR, promote the engine's HDR scene buffer (Buffers tab).
    g_passes[2].init("world",   "",                  "rgba", 4, SRC_NATIVE_WORLD);

    // ---- Tier 2: ENGINE passes (raw engine RTs — fingerprints come from the game profile) ----
    // Native engine G-buffer normals (cleaner/full-res vs the shader ones, RG-encoded). Fingerprint
    // is supplied by the game profile below; reassignable at runtime via the RT Browser.
    g_passes[3].init("normals_native", "", "rgba", 4, SRC_ENGINE_RT);

    // Seed the hotkey table from its defaults (F9 = record, everything else unbound). Done here
    // rather than in load_addon_config so the record key works even on a host without the v13
    // config API; the ini overlays this on init_effect_runtime.
    hotkeys_apply_defaults();

    // Apply the per-game profile: detect the host exe and seed the engine-pass fingerprint.
    // load_addon_config (on init_effect_runtime) later overlays any saved per-game ini.
    detect_game_profile();
    g_passes[3].rt_fmt     = g_profile.nrm_fmt;
    g_passes[3].rt_min_w   = g_profile.nrm_min_w;
    g_passes[3].rt_ordinal = g_profile.nrm_ordinal;

    // Custom slots: empty promotable passes, filled by "Promote to pass" in the RT Browser.
    for (int i = 0; i < N_CUSTOM; ++i) {
        RecordPass& c = g_passes[N_FIXED + i];
        c.init(CUSTOM_NAMES[i], "", "rgba", 4, SRC_ENGINE_RT);
        c.is_custom = true;
        c.promoted  = false;
        c.enabled   = false;
    }
    {
        char msg[192];
        // _TRUNCATE: g_exe_name is up to 127 chars of whatever the host exe is called, which with
        // the profile name overruns this buffer — and sprintf_s ABORTS THE GAME on overflow.
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "MultipassRecorder: game profile '%s' (exe '%s')", g_profile.name, g_exe_name);
        reshade::log::message(reshade::log::level::info, msg);
    }

    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
#if RESHADE_API_VERSION >= 11
    reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(on_set_current_preset_path);
#endif
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::register_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
    // Fires once per frame right before the visible effect chain — where FX-layer passes render
    // their technique into their own target (mpr_fxlayer.inc). Once a frame is nothing, and the
    // handler early-outs before touching the device when no FX pass wants a layer this frame.
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_reshade_begin_effects);
    // Fires once per frame right AFTER the visible effect chain, before ReShade draws any UI —
    // the one moment the back buffer holds the graded game image with no menu, no OSD and no
    // composition guides in it. That is where a still grabs the screen (mpr_shot.inc); it
    // early-outs immediately on every frame that isn't a shot frame.
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_reshade_finish_effects);
    // Fires after effects finish (re)compiling: (re)enable our Setup technique so the addon owns
    // its on-state and depth/normals/world are never black for a forgotten checkbox.
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reshade_reloaded_effects);
    // Scene hooks (bind_render_targets_and_depth_stencil, draw, draw_indexed — drive the live RT
    // registry / Buffers gallery) and push_descriptors (compute-UAV discovery). Registered ONCE,
    // here, and never touched again until AddonUninit: these used to be hooked/unhooked from the
    // present thread to track g_live_enum, which is a real crash on D3D12. reshade's addon_event_list
    // is a bare std::vector<void*> and invoke_addon_event iterates it with zero synchronization, so
    // registering (push_back — can REALLOCATE) or unregistering (erase) while a title's worker
    // threads are recording command lists that fire these very events walks a freed buffer into a
    // garbage function pointer. draw/draw_indexed do fire on every draw call, which is what the
    // toggling was for — each hook body early-outs on g_live_enum instead (mpr_livert.inc).
    reshade::register_event<reshade::addon_event::push_descriptors>(on_scene_push_desc);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_scene_bind_rts);
    reshade::register_event<reshade::addon_event::draw>(on_scene_draw);
    reshade::register_event<reshade::addon_event::draw_indexed>(on_scene_draw_indexed);
    g_push_desc_hooked = g_scene_hooks_hooked = true; // permanently: nothing toggles these now
    // Evict live-RT registry slots when the game frees a resource (avoids dangling preview SRVs).
    reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource_evict);
    // ONE tabbed settings window (Record | Buffers | Overlays | Setup); the floating Viewfinder
    // is drawn from on_reshade_overlay. Tied to the ReShade "Home" toggle (no separate hotkey),
    // position/collapse persisted by ReShade.
    reshade::register_overlay(MPR_MAIN_TITLE, on_draw_settings);
    return true;
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE a, HMODULE r) {
    // A real stop first, then rendezvous with BOTH background finishers: the stop joiner owns the
    // worker handles (joining a std::thread twice is UB — terminate() inside the host game), and the
    // audio finalizer waits up to 15s on FFmpeg from inside this DLL. Neither may still be running
    // when we unload. This is also why the flag write below takes q_mutex like every other site: an
    // unsynchronized store races the worker's wait predicate, and the lost wakeup hangs the game on
    // exit — the exact path taken when quitting mid-take.
    stop_recording_async();
    stop_join_wait();
    for (auto& p : g_passes) {
        if (!p.worker.joinable()) continue;
        { std::lock_guard<std::mutex> lk(p.q_mutex); p.worker_stop = true; }
        p.q_cv.notify_one();
        p.worker.join();
    }
    audio_finalize_wait();
    shot_shutdown(); // drain + join the still writer for the same reason: no thread outlives the DLL
    // All FFmpeg children have now exited cleanly; release the tether job. (On an
    // abnormal game exit this never runs, and the OS closing the handle is exactly
    // what kills the orphaned audio encoder.)
    if (g_ffmpeg_job) { CloseHandle(g_ffmpeg_job); g_ffmpeg_job = nullptr; }
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
#if RESHADE_API_VERSION >= 11
    reshade::unregister_event<reshade::addon_event::reshade_set_current_preset_path>(on_set_current_preset_path);
#endif
    reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::unregister_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
    reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_reshade_begin_effects);
    reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(on_reshade_finish_effects);
    reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reshade_reloaded_effects);
    // Unconditional, matching the unconditional registration in AddonInit. Unregistering is only
    // safe here because the runtime is tearing the addon down — never mid-session (see above).
    reshade::unregister_event<reshade::addon_event::push_descriptors>(on_scene_push_desc);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_scene_bind_rts);
    reshade::unregister_event<reshade::addon_event::draw>(on_scene_draw);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(on_scene_draw_indexed);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource_evict);
    reshade::unregister_overlay(MPR_MAIN_TITLE, on_draw_settings);
    reshade::unregister_addon(a, r);
}
