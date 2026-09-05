// === MultipassRecorder — mpr_fxlayer.inc ===
// FX-layer passes — render ONE ReShade effect technique into a pass's own private render target,
// independent of the visible effect chain, so it can be recorded as its own capture pass.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.
//
// WHY this exists. Effects like MXAO/RTGI have a "Debug View" that renders their raw internals
// (ambient occlusion, GI, normals) to the screen. Grabbing the screen after that technique ran
// would work, but then the debug view is ALSO what the player and the `world` pass see — you could
// never record a clean beauty and an AO matte in the same take. ReShade's public
// effect_runtime::render_technique lets an addon run any loaded technique into a render target of
// its own choosing, so instead each FX pass gets a private target and the game view is untouched.
//
// HOW it hangs together, per frame, on reshade_begin_effects (fires right BEFORE the visible chain,
// so the back buffer is still the clean game image — no ReShade effects, no overlay, no HUD):
//
//     seed     : copy the back buffer into p.fx_rt   (or clear it to white / black)
//     override : set the effect's debug uniform to this pass's value, remembering the old one
//     render   : rt->render_technique(tech, cmd, p.fx_rtv)
//     restore  : write the user's uniform value back
//
// on_reshade_present then copies p.fx_rt into staging exactly like any other source — the whole
// capture/worker/FFmpeg half of the addon is untouched.
//
// Load-bearing runtime behaviour this depends on (all in the pinned v6.5.1 SDK source):
//   - render_technique copies the render target we hand it into the effect's color texture before
//     the first pass, so the technique reads OUR target as ReShade::BackBuffer and writes back into
//     it. Hence the seed step, and hence fx_rt MUST match the back buffer's size and format.
//   - Called from inside a present call it always uses effect permutation 0 — no new permutation,
//     no effect recompile — again provided our target matches the back buffer.
//   - set_uniform_value only writes the effect's uniform storage (it does not dirty or save the
//     preset), and render_technique uploads that storage to the constant buffer as its first step.
//     So set -> render -> restore is airtight: the visible chain runs after us and re-uploads.
//   - Our own render_technique call RE-FIRES reshade_begin_effects. The re-entrancy guard below is
//     not optional; without it this recurses forever.
//   - A technique whose effect has no GPU resources yet is queued for creation and the call
//     no-ops, so this also works on techniques that are switched OFF in the ReShade UI — it just
//     takes a frame or two to warm up.
//
// KNOWN LIMITATION: we hand render_technique a single render target view, so it uses the same one
// for both the plain and the SRGBWriteEnabled slots (the visible chain gets two views of the back
// buffer, one of each). A pass that declares `SRGBWriteEnabled = true` therefore writes linear
// where the game view would have written sRGB-encoded, and that pass's layer comes out darker than
// the same effect looks on screen. Supporting both would mean creating fx_rt TYPELESS so a second,
// sRGB-typed view is legal in D3D11 — worth doing if a real effect trips over it, but the debug
// views this feature exists for (AO/GI/normals) don't use SRGBWriteEnabled.

// Re-entrancy guard for the point above: true while WE are inside render_technique.
static bool g_fx_in_render = false;

// Defined in mpr_native.inc, which is included AFTER this file because it depends on
// fx_beat_due / fx_format_supported / fx_ffmpeg_fmt below. The native `world` plate is copied
// from this file's hook (see on_reshade_begin_effects) because that is the one moment the back
// buffer holds the clean game image — no effects, no overlay, no OSD.
static void native_world_tick(device* dev, command_list* cmd, resource back, const resource_desc& bd);

// ---- Format support ---------------------------------------------------------------------
// An FX layer inherits the back buffer's pixel format, and the encode path understands exactly
// two layouts: 8-bit 4-channel (fed to FFmpeg raw) and packed r11g11b10_float (unpacked to
// rgb48le by the worker via cap_format). Anything else would be silently mis-strided or
// mis-coloured, so it's reported instead of recorded.
static bool fx_format_supported(format f) {
    switch (f) {
        case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb: case format::r8g8b8a8_typeless:
        case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb:
        case format::r11g11b10_float:
            return true;
        default:
            return false;
    }
}
// Raw input pix_fmt for FFmpeg. (Irrelevant for r11g11b10_float — the worker overrides it with
// rgb48le once cap_format says the source is packed HDR.)
static const char* fx_ffmpeg_fmt(format f) {
    switch (f) {
        case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb: return "bgra";
        default:                                                       return "rgba";
    }
}

// Case-insensitive substring test, used to spot a "Debug View"-ish uniform among an effect's
// controls. Hand-rolled ASCII rather than <cctype>/StrStrIA to keep the unity build's include set
// and locale behaviour unchanged.
static bool mpr_icontains(const char* hay, const char* needle) {
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; };
    if (hay == nullptr || needle == nullptr || !*needle) return false;
    for (const char* h = hay; *h; ++h) {
        const char* a = h; const char* b = needle;
        while (*a && *b && lower(*a) == lower(*b)) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

// ---- Technique / pass lookup ------------------------------------------------------------
// Resolved by NAME every frame, never cached: effect_technique handles are invalidated whenever
// effects reload (a preset change, a shader edit, the Reload button), and a stale handle is a
// dangling pointer into the runtime.
static effect_technique fx_find_technique(effect_runtime* rt, const RecordPass& p) {
    if (!p.fx_tech[0]) return { 0 };
    effect_technique t = rt->find_technique(p.fx_effect[0] ? p.fx_effect : nullptr, p.fx_tech);
    if (t == 0) t = rt->find_technique(nullptr, p.fx_tech); // effect file renamed/moved: match on name alone
    return t;
}

// Is this technique already captured by a pass? Used by the Effects gallery so "+ Add as pass"
// can't create two passes rendering the same technique (which would also collide on the output
// filename, since FX passes are named after their technique).
static RecordPass* find_fx_pass(const char* effect_name, const char* tech_name) {
    for (int i = 0; i < N_CUSTOM; ++i) {
        RecordPass& c = g_passes[N_FIXED + i];
        if (c.source != SRC_FX_LAYER || !c.promoted) continue;
        if (strcmp(c.fx_tech, tech_name) == 0 && strcmp(c.fx_effect, effect_name) == 0) return &c;
    }
    return nullptr;
}

// A capture pass's filename stem. Promoted FX passes are named after their technique
// ("<session>_MXAO.mp4"), which is far more useful in an NLE bin than the slot name it happens to
// occupy ("<session>_rt3.mp4"). Slot names remain the ini keys, so this affects output only.
static void pass_file_tag(const RecordPass& p, char* out, size_t outsz) {
    const char* src = (p.source == SRC_FX_LAYER && p.fx_tech[0]) ? p.fx_tech : p.name;
    size_t k = 0;
    for (size_t i = 0; src[i] && k + 1 < outsz; ++i) {
        const char c = src[i];
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';
        out[k++] = safe ? c : '_';
    }
    out[k] = '\0';
    if (k == 0) strcpy_s(out, outsz, p.name);
}

// ---- Private layer target ----------------------------------------------------------------
// Per-pass latch for a target that could not be BUILT, as distinct from a back buffer format we
// refuse to encode (fx_fmt_ok already latches that one). Without it the hook re-attempted a
// back-buffer-sized allocation, and wrote a line to ReShade.log, on every single frame for the
// rest of the session — under exactly the VRAM pressure that made it fail in the first place.
// Parallel to g_passes rather than a RecordPass field because it is a runtime symptom, not pass
// configuration: it never persists, and it re-arms whenever fx_destroy runs (i.e. when the size
// or format genuinely changes), so a transient failure isn't permanent either.
static bool g_fx_rt_failed[N_FIXED + N_CUSTOM] = {};
static bool& fx_rt_failed(const RecordPass& p) { return g_fx_rt_failed[&p - g_passes]; }

// Report a layer-target failure the way every other reactive warning is reported: after it has
// happened, naming the pass, in the log and in the Effects tab. Called only from the (re)build
// path, so it fires once per build rather than once per frame.
static void fx_rt_report(reshade::log::level lvl, const char* fmt, ...) {
    char msg[320]; // _TRUNCATE: sprintf_s ABORTS on overflow and pass/technique names are arbitrary
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    reshade::log::message(lvl, msg);
    g_fx_report = msg + sizeof("MultipassRecorder: ") - 1;
}

static void fx_destroy(device* dev, RecordPass& p) {
    if (p.fx_srv.handle) { dev->destroy_resource_view(p.fx_srv); p.fx_srv = { 0 }; }
    if (p.fx_rtv.handle) { dev->destroy_resource_view(p.fx_rtv); p.fx_rtv = { 0 }; }
    if (p.fx_rt.handle)  { dev->destroy_resource(p.fx_rt);       p.fx_rt  = { 0 }; }
    p.fx_w = p.fx_h = 0;
    p.fx_fmt = format::unknown;
    p.fx_fmt_ok = true;
    fx_rt_failed(p) = false;
}
static void fx_free_all(device* dev) { for (auto& p : g_passes) fx_destroy(dev, p); }

// (Re)create the layer target to match the back buffer. Returns false if the format is one the
// encode path can't write, or if the target couldn't be built — both latched (fx_fmt_ok /
// fx_rt_failed) so neither is re-attempted every frame.
static bool fx_ensure_rt(device* dev, RecordPass& p, uint32_t w, uint32_t h, format f) {
    // The cached answer is only usable when the target is COMPLETE. Testing fx_rt alone let a
    // half-built one (resource created, RTV creation failed) report success from the second frame
    // on — and the seed step then handed clear_render_target_view a null RTV, whose null check is
    // a debug-only assert in the runtime, so a release build passed nullptr straight into
    // ID3D11DeviceContext::ClearRenderTargetView and killed the game.
    if (p.fx_w == w && p.fx_h == h && p.fx_fmt == f &&
        ((p.fx_rt.handle != 0 && p.fx_rtv.handle != 0) || !p.fx_fmt_ok || fx_rt_failed(p)))
        return p.fx_fmt_ok && !fx_rt_failed(p);

    fx_destroy(dev, p);
    p.fx_w = w; p.fx_h = h; p.fx_fmt = f;
    p.fx_fmt_ok = fx_format_supported(f);
    p.ffmpeg_fmt = fx_ffmpeg_fmt(f);
    if (!p.fx_fmt_ok) return false;

    const resource_desc d(w, h, 1, 1, f, 1, memory_heap::gpu_only,
                          resource_usage::render_target | resource_usage::shader_resource |
                          resource_usage::copy_source  | resource_usage::copy_dest);
    if (!dev->create_resource(d, nullptr, resource_usage::render_target, &p.fx_rt)) {
        p.fx_rt = { 0 };
        fx_rt_failed(p) = true;
        fx_rt_report(reshade::log::level::error,
                     "MultipassRecorder: pass '%s' can't record - couldn't allocate its %ux%u layer target (out of video memory?).",
                     p.name, w, h);
        return false;
    }
    const resource_view_desc vd(f);
    dev->create_resource_view(p.fx_rt, resource_usage::render_target, vd, &p.fx_rtv);
    if (p.fx_rtv.handle == 0) {
        // A target with no render-target view isn't half-usable: both the seed clear and
        // render_technique take the view, so keep nothing rather than cache a resource that can
        // only ever hand the runtime a null one.
        fx_destroy(dev, p);
        p.fx_w = w; p.fx_h = h; p.fx_fmt = f;
        fx_rt_failed(p) = true;
        fx_rt_report(reshade::log::level::error,
                     "MultipassRecorder: pass '%s' can't record - the driver wouldn't give its %s layer target a render-target view.",
                     p.name, probe_format_name(f));
        return false;
    }
    if (!dev->create_resource_view(p.fx_rt, resource_usage::shader_resource, vd, &p.fx_srv)) {
        // Not fatal — the pass records fine. It just has no thumbnail and no Viewfinder image,
        // which on its own reads as "the layer isn't rendering", so say which it is.
        p.fx_srv = { 0 };
        fx_rt_report(reshade::log::level::warning,
                     "MultipassRecorder: pass '%s' records, but its layer target can't be sampled for preview (no shader-resource view).",
                     p.name);
    }
    return true;
}

// ---- Debug-View uniform override ---------------------------------------------------------
// The saved value of the overridden uniform, so it can be put back the moment our render is done.
struct FxUniformSave {
    effect_uniform_variable var  = { 0 };
    format                  base = format::unknown;
    int32_t  i = 0; uint32_t u = 0; float f = 0.0f; bool b = false;
};

// Set the pass's debug uniform to its recorded value. Returns false (and leaves `s` empty) when
// there's no override, or the uniform can't be reached at all — which is the normal state under
// ReShade's Performance Mode, where uniforms are compiled away into constants.
static bool fx_dbg_push(effect_runtime* rt, const RecordPass& p, FxUniformSave& s) {
    if (!p.fx_dbg_on || !p.fx_dbg_var[0]) return false;
    effect_uniform_variable v = rt->find_uniform_variable(p.fx_effect[0] ? p.fx_effect : nullptr, p.fx_dbg_var);
    if (v == 0) v = rt->find_uniform_variable(nullptr, p.fx_dbg_var);
    if (v == 0) return false;
    s.var = v;
    rt->get_uniform_variable_type(v, &s.base);
    switch (s.base) {
        case format::r32_sint:  { rt->get_uniform_value_int  (v, &s.i, 1); const int32_t  nv = p.fx_dbg_val;            rt->set_uniform_value_int  (v, &nv, 1); break; }
        case format::r32_uint:  { rt->get_uniform_value_uint (v, &s.u, 1); const uint32_t nv = (uint32_t)p.fx_dbg_val;  rt->set_uniform_value_uint (v, &nv, 1); break; }
        case format::r32_float: { rt->get_uniform_value_float(v, &s.f, 1); const float    nv = (float)p.fx_dbg_val;     rt->set_uniform_value_float(v, &nv, 1); break; }
        default:                { rt->get_uniform_value_bool (v, &s.b, 1); const bool     nv = p.fx_dbg_val != 0;       rt->set_uniform_value_bool (v, &nv, 1); break; }
    }
    return true;
}
static void fx_dbg_pop(effect_runtime* rt, const FxUniformSave& s) {
    if (s.var == 0) return;
    switch (s.base) {
        case format::r32_sint:  rt->set_uniform_value_int  (s.var, &s.i, 1); break;
        case format::r32_uint:  rt->set_uniform_value_uint (s.var, &s.u, 1); break;
        case format::r32_float: rt->set_uniform_value_float(s.var, &s.f, 1); break;
        default:                rt->set_uniform_value_bool (s.var, &s.b, 1); break;
    }
}

// ---- When a layer renders ----------------------------------------------------------------
// This hook runs EARLIER in the frame than on_reshade_present, which is where the capture cadence
// actually gets decided, so while recording it has to predict that decision: a layer rendered one
// beat late would be out of register with every other pass (the AO of frame N next to the beauty of
// frame N+1 — visible on a fast pan). Adding the last frame's duration to `now` means that whenever
// present is about to declare a beat, this already declared it a fraction of a millisecond earlier.
// Erring early is free: an unused layer render costs GPU time, a late one corrupts the take.
static bool fx_beat_due() {
    const auto now = std::chrono::high_resolution_clock::now();
    return (now + std::chrono::microseconds(g_frame_dt_us)) >= g_next_capture_time;
}

static bool fx_should_render(const RecordPass& p) {
    if (p.source != SRC_FX_LAYER || !p.promoted || !p.enabled || !p.fx_tech[0]) return false;
    // A still is being grabbed on THIS frame (mpr_shot.inc). Every enabled layer has to be current
    // for it, whatever the recording/preview cadence below would otherwise decide -- a layer left
    // a beat stale would put this pass's still out of register with the rest of the set.
    if (g_live_frame == g_shot_capture_frame) return true;
    if (g_recording) return fx_beat_due();
    // Idle: only worth rendering when someone is looking at it. The pass under the Viewfinder /
    // Debug View renders every frame -- that's the one you're actively watching while you cycle
    // debug values, and a throttled image makes the controls feel broken. Every other pass is just
    // feeding a gallery thumbnail while the panel is open, which is fine at a quarter rate.
    if (g_preview_pass == &p && (g_preview_open || g_debug_view)) return true;
    return g_settings_ttl > 0 && (g_live_frame % 4) == 0;
}

// ---- The hook -----------------------------------------------------------------------------
static void on_reshade_begin_effects(effect_runtime* rt, command_list* cmd, resource_view rtv, resource_view)
{
    if (g_fx_in_render || rtv.handle == 0) return; // our own render_technique re-enters here

    device* dev = rt->get_device();
    const resource back = dev->get_resource_from_view(rtv);
    if (back.handle == 0) return;
    const resource_desc bd = dev->get_resource_desc(back);

    // Only build layers off the GAME's frame. This event also fires when another addon drives
    // render_effects into a target of its own; matching that target's size would thrash every
    // layer RT (destroy + recreate, twice per frame) and record a foreign image. Compared against
    // the runtime's effect resolution rather than get_current_back_buffer(), because with MSAA or
    // a format conversion ReShade renders into an intermediate, not the swapchain image itself.
    uint32_t rw = 0, rh = 0;
    rt->get_screenshot_width_and_height(&rw, &rh);
    if (rw == 0 || bd.texture.width != rw || bd.texture.height != rh) return;

    // The native `world` plate rides this hook rather than finish_effects: it wants the back
    // buffer as the GAME handed it over, before the visible chain grades it. Both guards above
    // (our own re-entrancy, and "this is the game's frame, not another addon's target") apply to
    // it identically, which is why it is called from here instead of duplicating them.
    native_world_tick(dev, cmd, back, bd);

    const format bf = format_to_default_typed(bd.texture.format);

    for (auto& p : g_passes) {
        if (!fx_should_render(p)) continue;
        const effect_technique tech = fx_find_technique(rt, p);
        if (tech == 0) continue; // effect not loaded (yet) — reported reactively by on_reshade_present
        if (!fx_ensure_rt(dev, p, bd.texture.width, bd.texture.height, bf)) continue;

        // ---- Seed: what the technique composites onto ----
        if (p.fx_seed == FXSEED_SCENE) {
            const resource       res[2]    = { back, p.fx_rt };
            const resource_usage old_st[2] = { resource_usage::render_target, resource_usage::render_target };
            const resource_usage new_st[2] = { resource_usage::copy_source,   resource_usage::copy_dest };
            cmd->barrier(2, res, old_st, new_st);
            cmd->copy_texture_region(back, 0, nullptr, p.fx_rt, 0, nullptr, filter_mode::min_mag_mip_point);
            cmd->barrier(2, res, new_st, old_st);
        } else {
            const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            cmd->clear_render_target_view(p.fx_rtv, p.fx_seed == FXSEED_WHITE ? white : black);
        }

        // ---- Render the technique into OUR target, with this pass's debug value ----
        FxUniformSave saved;
        const bool pushed = fx_dbg_push(rt, p, saved);
        g_fx_in_render = true;
        rt->render_technique(tech, cmd, p.fx_rtv);
        g_fx_in_render = false;
        if (pushed) fx_dbg_pop(rt, saved);

        p.fx_last_render_frame = g_live_frame;
    }
}
