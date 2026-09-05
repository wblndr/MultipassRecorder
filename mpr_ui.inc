// === MultipassRecorder — mpr_ui.inc ===
// The full ReShade overlay UI: the tabbed settings panel (Record | Buffers | Effects | Stills |
// Overlays | Setup) and the floating Viewfinder (preview image, exposure scope, waveform,
// .fx pass controls).
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.

// ---- UI helpers ----
// (help_marker, section_gap, the color tokens and glyph icons live in mpr_theme.inc.)

// One description of Debug View, shared by the pass-row button and the Viewfinder checkbox so
// the feature is explained identically wherever it's toggled.
static const char* const MPR_TT_DEBUG_VIEW =
    "Projects this pass full-screen directly over the game itself, behind the ReShade menu --\n"
    "like a renderer's debug buffer view. Screen-only (never captured), paused while recording.\n"
    "Toggle it here or in the Viewfinder; opening the ReShade menu (Home) always gets you back.";

// ---- Coalesced config writes --------------------------------------------------------------
// One save_addon_config is ~300 sprintf_s + set_config_value calls across all 16 passes. A widget
// being DRAGGED reports a change every frame it moves, which ran that whole write per frame; a
// checkbox/combo/text field reports once and is already deactivated by then. save_cfg splits the
// two on exactly that test: nothing owns the mouse -> write now, something does -> mark dirty and
// let cfg_save_tick flush it the frame the drag ends. The tick is called from draw_buffer_preview,
// the one function of ours ImGui reaches EVERY frame (menu open or not), so a deferred write can't
// be stranded by closing the overlay.
static bool g_cfg_dirty = false;
static void save_cfg(effect_runtime* rt) {
    if (ImGui::IsAnyItemActive()) { g_cfg_dirty = true; return; }
    g_cfg_dirty = false;
    save_addon_config(rt);
}
static void cfg_save_tick(effect_runtime* rt) {
    if (g_cfg_dirty && !ImGui::IsAnyItemActive()) { g_cfg_dirty = false; save_addon_config(rt); }
}

// One-line description of WHAT a pass captures (buffer + native precision).
// Promote a live RT into the next free custom slot as a NEW pass (non-destructive; never touches
// the fixed passes). Packed-HDR formats default to 10-bit; the r11g11b10 unpack is handled at
// capture via cap_format. Returns false (with a message) if all custom slots are full.
static bool promote_rt(effect_runtime* rt, const LiveRT& s) {
    // Fingerprint against the SAME bind kind the buffer was promoted from, so the ordinal is
    // computed over (and later resolved against) an RTV-only or UAV-only candidate set. The
    // ordinal is the buffer's index in the exact candidate list resolve_engine_rt reads back
    // (engine_rt_candidates), so the saved fingerprint resolves to THIS buffer by construction.
    const uint8_t bind = (s.bound_as & LIVE_RTV) ? LIVE_RTV : LIVE_UAV;
    const LiveRT* cand[64];
    const int n = engine_rt_candidates(s.desc.texture.format, s.desc.texture.width, bind, cand);
    int ord = -1;
    for (int k = 0; k < n; ++k) if (cand[k]->handle == s.handle) { ord = k; break; }
    if (ord < 0) {
        // Gallery rows linger well past the resolve window (LIVE_LIST_FRAMES, so intermittent
        // buffers don't flicker out of the list), so a stale row can't be fingerprinted — refuse
        // rather than save an ordinal that resolves to a different buffer. The Buffers tab greys
        // the button out for idle rows, so reaching this is the rare "went idle mid-click" race.
        g_scan_report = "That buffer wasn't bound this frame, so it can't be fingerprinted yet - try again while the scene is rendering it.";
        return false;
    }
    for (int i = 0; i < N_CUSTOM; ++i) {
        RecordPass& c = g_passes[N_FIXED + i];
        if (c.promoted) continue;
        c.promoted   = true;
        c.enabled    = true;
        c.source     = SRC_ENGINE_RT;
        c.cap_format = format::unknown;
        c.rt_fmt     = s.desc.texture.format;
        c.rt_min_w   = s.desc.texture.width;
        c.rt_bind    = bind;
        c.rt_ordinal = ord;
        const bool hdrfmt = (s.desc.texture.format == format::r11g11b10_float);
        c.enc_codec = ENC_HEVC; c.enc_lossless = true; c.enc_chroma = CHROMA_444;
        c.enc_bits  = hdrfmt ? BITS_10 : BITS_8;
        c.enc_hdr   = hdrfmt ? HDR_PQ : HDR_REC709; // packed-HDR -> PQ/HDR10 (standard, one-click in any NLE)
        sprintf_s(c.label, "o%02d%s %s %ux%u", s.order, (c.rt_bind == LIVE_UAV) ? " cs" : "",
                  probe_format_name(s.desc.texture.format), s.desc.texture.width, s.desc.texture.height);
        save_addon_config(rt);
        g_scan_report = std::string("Added as pass '") + c.label + "' - see CUSTOM in the Record tab.";
        return true;
    }
    g_scan_report = "All custom pass slots are full - remove one first.";
    return false;
}

// Promote a ReShade effect TECHNIQUE into the next free custom slot as a NEW pass: the addon will
// render it into a target of its own each frame (mpr_fxlayer.inc) instead of reading an existing
// buffer. Same non-destructive contract as promote_rt -- additive, never touches a fixed pass.
static bool promote_fx_technique(effect_runtime* rt, const char* effect_name, const char* tech_name) {
    if (find_fx_pass(effect_name, tech_name)) {
        g_fx_report = std::string("'") + tech_name + "' is already a pass.";
        return false;
    }
    for (int i = 0; i < N_CUSTOM; ++i) {
        RecordPass& c = g_passes[N_FIXED + i];
        if (c.promoted) continue;
        c.promoted   = true;
        c.enabled    = true;
        c.source     = SRC_FX_LAYER;
        c.cap_format = format::unknown;
        // _TRUNCATE throughout: the secure CRT ABORTS the process on overflow rather than
        // truncating, and a long shader/technique name must never be able to kill the game.
        strncpy_s(c.fx_effect, effect_name, _TRUNCATE);
        strncpy_s(c.fx_tech,   tech_name,   _TRUNCATE);
        c.fx_dbg_on = false; c.fx_dbg_var[0] = '\0'; c.fx_dbg_val = 0;
        c.fx_seed   = FXSEED_SCENE;
        c.fx_last_render_frame = g_live_frame;
        // The layer inherits the back buffer, which is 8-bit LDR on every game we target, so the
        // plain lossless default applies -- no HDR curve, no exposure.
        c.enc_codec = ENC_HEVC; c.enc_lossless = true; c.enc_chroma = CHROMA_444;
        c.enc_bits  = BITS_8;   c.enc_hdr = HDR_REC709;
        strncpy_s(c.label, tech_name, _TRUNCATE);
        save_addon_config(rt);
        g_fx_report = std::string("Added '") + tech_name + "' as a pass - see CUSTOM in the Record tab.";
        return true;
    }
    g_fx_report = "All custom pass slots are full - remove one first.";
    return false;
}

// Remove a promoted custom pass, freeing its slot (stops/joins its worker first).
static void remove_custom_pass(effect_runtime* rt, RecordPass& p) {
    p.enabled = false;
    p.ui_expanded = false;
    destroy_pass_resources(rt->get_device(), p);
    fx_destroy(rt->get_device(), p);
    if (g_preview_pass == &p) { g_preview_pass = nullptr; g_debug_view = false; } // never leave the Viewfinder on a dead slot
    p.promoted = false;
    p.label[0] = '\0';
    p.rt_fmt = format::unknown; p.rt_min_w = 0; p.rt_ordinal = 0; p.rt_bind = LIVE_RTV;
    p.cap_format = format::unknown;
    p.source = SRC_ENGINE_RT; // neutral empty-slot state (what a fresh slot is initialised to)
    p.fx_effect[0] = p.fx_tech[0] = p.fx_dbg_var[0] = '\0';
    p.fx_dbg_on = false; p.fx_dbg_val = 0; p.fx_seed = FXSEED_SCENE;
    save_addon_config(rt);
}

static const char* pass_source_caption(const RecordPass& p) {
    static char fxbuf[320];
    switch (p.source) {
        case SRC_FX_LAYER:
            _snprintf_s(fxbuf, sizeof(fxbuf), _TRUNCATE, "ReShade effect '%s'%s%s%s - rendered on its own, %s plate",
                      p.fx_tech,
                      p.fx_effect[0] ? " (" : "", p.fx_effect[0] ? p.fx_effect : "", p.fx_effect[0] ? ")" : "",
                      p.fx_seed == FXSEED_WHITE ? "white" : p.fx_seed == FXSEED_BLACK ? "black" : "clean-scene");
            return fxbuf;
        case SRC_ENGINE_RT:
            if (p.is_custom) return p.label[0] ? p.label : "promoted engine RT";
            if (strcmp(p.name, "normals_native") == 0) return "native engine G-buffer normals - 8-bit, RG-encoded";
            return "engine render target";
        case SRC_NATIVE_DEPTH:
            _snprintf_s(fxbuf, sizeof(fxbuf), _TRUNCATE,
                        "linearized depth, computed by the addon - 32-bit float, opaque only%s",
                        p.native_linear_output ? ", scene-linear" : "");
            return fxbuf;
        case SRC_NATIVE_NORMALS: return "normals reconstructed from depth by the addon - 8-bit";
        case SRC_NATIVE_WORLD:   return "engine backbuffer before ReShade's effects - 8-bit LDR, incl. HUD";
        default: // SRC_NAMED
            return "ReShade .fx texture";
    }
}

// True if this pass captures a packed-HDR (float) buffer, so the HDR-curve choice is meaningful.
static bool pass_is_hdr_src(const RecordPass& p) {
    if (p.cap_format == format::r11g11b10_float) return true;
    if (p.source == SRC_ENGINE_RT && p.rt_fmt == format::r11g11b10_float) return true;
    return false;
}

// Compact one-line summary of a pass's effective encode ("HEVC 10-bit PQ lossless") — shared
// by the pass-table encode column and the header strip. Mirrors the arg logic in worker_func.
static void pass_encode_summary(const RecordPass& p, char* buf, size_t bufsz) {
    const bool hdr = pass_is_hdr_src(p);
    if (p.enc_codec == ENC_PNG)    { sprintf_s(buf, bufsz, "PNG %s linear", hdr ? "16-bit" : "8-bit"); return; }
    if (p.enc_codec == ENC_CUSTOM) { sprintf_s(buf, bufsz, "Custom FFmpeg args"); return; }
    const char* codec = (p.enc_codec == ENC_H264) ? "H.264" : "HEVC";
    const int   bits  = (p.enc_codec == ENC_HEVC && ((p.enc_bits == BITS_10) || (hdr && (p.enc_hdr == HDR_PQ || p.enc_hdr == HDR_LOG2)))) ? 10 : 8;
    const char* curve = hdr ? (p.enc_hdr == HDR_PQ ? " PQ" : p.enc_hdr == HDR_LOG2 ? " Log2" : " Rec709") : "";
    if (p.enc_lossless) sprintf_s(buf, bufsz, "%s %d-bit%s lossless", codec, bits, curve);
    else                sprintf_s(buf, bufsz, "%s %d-bit%s CQ%d", codec, bits, curve, p.enc_cq);
}


// The capture window of each HDR curve, expressed in stops relative to diffuse white (0 = white).
// floor = below this the encode is effectively black; ceil = above this it clips. Log2 edges are
// exact; PQ/Rec.709 floors are practical (10-bit quantization), so treat them as approximate.
static void curve_window_stops(int hdr, float* floor_st, float* white_st, float* ceil_st) {
    *white_st = 0.0f;
    if (hdr == HDR_LOG2)    { *floor_st = HDR_LOG2_FLOOR; *ceil_st = HDR_LOG2_CEIL; }          // -14..+6, exact
    else if (hdr == HDR_PQ) { *floor_st = -12.0f;         *ceil_st = log2f(10000.0f / g_hdr_pq_paper_nits); } // ceil ~ +6.6 @100 nits
    else                    { *floor_st = -8.0f;          *ceil_st = 4.0f; }                    // Rec.709 tonemap, approximate
}

// Normalized output code (0..1) the selected curve assigns to an EXPOSED-linear value. Reuses the
// exact encode transfer functions, so the plotted curve *is* what the file stores. HDR_REC709 is the
// SDR path. The local slope of this curve = code values spent per stop = precision at that tone.
static float curve_code(int hdr, float lin) {
    switch (hdr) {
        case HDR_LOG2: return hdr_log2_u16(lin)    * (1.0f / 65535.0f);
        case HDR_PQ:   return hdr_pq_u16(lin)      * (1.0f / 65535.0f);
        default:       return hdr_tonemap_u16(lin) * (1.0f / 65535.0f);
    }
}

// AE / EV / manual-exposure controls for one pass — ONE implementation shared by the Viewfinder
// scope header (compact camera-style row) and the per-pass encoding block, so the two can never
// drift apart. Camera model: Auto (AE) continuously meters and drops the scene into the capture
// window (protecting highlights), the EV dial nudges it in 1/3-stop detents, and the value
// freezes the instant recording starts. Manual = the raw exposure slider + one-shot Fit.
static void draw_exposure_controls(effect_runtime* rt, RecordPass& p, bool compact) {
    float fp, wp, cp; curve_window_stops(p.enc_hdr, &fp, &wp, &cp);
    if (compact) mpr_icon_sun();
    if (ImGui::Checkbox(compact ? "Auto (AE)" : "Auto exposure (AE)", &p.ae_auto)) save_addon_config(rt);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Auto-exposure: continuously drops the scene into the capture window,\n"
        "protecting highlights. The EV dial nudges it. Locks the moment recording starts (no drift).");
    if (compact) ImGui::SameLine(0, 12);
    if (p.ae_auto) {
        if (compact) { ImGui::TextUnformatted("EV"); ImGui::SameLine(); ImGui::SetNextItemWidth(-1.0f); }
        else         ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderFloat(compact ? "##evcomp" : "EV compensation", &p.ev_comp, -5.0f, 5.0f, "%+.1f EV")) {
            p.ev_comp = roundf(p.ev_comp * 3.0f) / 3.0f; // snap to 1/3-stop camera detents
            save_cfg(rt);
        }
        if (compact) {
            ImGui::TextDisabled("applied %+.1f st", p.enc_exposure);
            if (g_recording) { // exposure is frozen for the take, camera-style rec-lock
                ImGui::SameLine(0, 6);
                mpr_icon_lock();
                ImGui::TextDisabled("locked");
            }
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled("-> %+.1f st%s", p.enc_exposure, g_recording ? " (locked)" : "");
        }
    } else {
        if (compact) { ImGui::TextUnformatted("Exposure"); ImGui::SameLine(); ImGui::SetNextItemWidth(-96.0f); } // room for Fit + (?)
        else         ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderFloat(compact ? "##exposure" : "Exposure (stops)", &p.enc_exposure, -6.0f, 12.0f, "%+.1f st")) save_cfg(rt);
        ImGui::SameLine();
        // Fit needs a live sample OF THIS PASS: the Viewfinder samples the previewed pass; with
        // the Viewfinder closed, the OSD meter's pass is sampled instead (see on_reshade_present).
        RecordPass* sampled = g_preview_open ? (g_preview_handle ? nullptr : g_preview_pass) : g_meter_pass;
        const bool can_fit = g_pv.have_frame && sampled == &p;
        if (!can_fit) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Fit")) {
            // Drop the data range inside the window, protecting highlights (p99.5 just under the ceiling).
            float e = (cp - 0.5f) - g_pv.span_hi;
            if (g_pv.span_lo + e < fp && (cp - fp) >= (g_pv.span_hi - g_pv.span_lo))
                e = fp - g_pv.span_lo; // it fits end-to-end: seat the shadows on the floor instead
            if (e < -6.0f) e = -6.0f; else if (e > 12.0f) e = 12.0f;
            p.enc_exposure = e; save_addon_config(rt);
        }
        if (!can_fit) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(can_fit
            ? "One-shot auto-exposure: drops the scene into the capture window, protecting highlights."
            : "One-shot auto-exposure. Needs a live sample of this pass - open the Viewfinder on it first.");
        ImGui::SameLine();
        help_marker("Slide right = brighter: the histogram moves right, toward the clip (red) end.\n"
                    "Baked into the file but invertible in post (subtract the same stops).\n\n"
                    "Engine HDR buffers are often pre-exposure (very dark) - use Fit to drop the scene\n"
                    "into the usable part of the curve. See docs/hdr-exposure.md.");
    }
}

// ---- Viewfinder instrument layout ---------------------------------------------------------
// The two instrument heights, named once and used BOTH by the draw and by the reservation below,
// so the image can never be sized against a number the scopes don't actually use.
static const float MPR_SCOPE_H = 96.0f;  // exposure histogram strip (draw_exposure_scope)
static const float MPR_WF_H    = 150.0f; // waveform / parade image  (draw_waveform_scope)

// ONE options control for the Viewfinder's persistent VIEW-MODE toggles: clip warning, native vs
// exposed histogram range, the curve overlay, and the waveform's on/mode/brightness. Those are all
// choices about HOW YOU ARE LOOKING, and the trigger's own label states every one of them, so
// folding them behind a click hides no information and no mode can be silently on. Everything that
// REPORTS state -- the scope itself, the waveform image, the AE/EV row, the over-range and
// precision lines, the recording-paused warning, the source identity -- stays permanently visible.
// (That distinction is the whole reason this is allowed to be a popup; see the no-accordions rule
// in CLAUDE.md.)
static void draw_scope_options(effect_runtime* rt, RecordPass* pass, bool scope_avail, bool wf_avail) {
    if (!scope_avail && !wf_avail) return;
    static const char* const WF_MODE_TOK[3] = { "wf overlay", "wf parade", "wf luma" };
    const int   hdr        = pass ? pass->enc_hdr : HDR_PQ;
    const bool  png_here   = pass && pass->enc_codec == ENC_PNG; // PNG stores raw linear
    const char* curve_name = png_here ? "Linear" : hdr == HDR_PQ ? "PQ" : hdr == HDR_LOG2 ? "Log2" : "Rec.709";
    const int   wf_mode    = (g_wf_mode >= 0 && g_wf_mode < 3) ? g_wf_mode : 0;

    // Label = the live state of every toggle inside. _TRUNCATE everywhere: the secure CRT ABORTS
    // the process on overflow, and this interpolates a curve name.
    char lbl[192] = "Scope:";
    bool first = true;
    auto append = [&](const char* tok) {
        strncat_s(lbl, first ? " " : " | ", _TRUNCATE);
        strncat_s(lbl, tok, _TRUNCATE);
        first = false;
    };
    char curve_tok[48];
    if (scope_avail) {
        if (g_scope_curve) _snprintf_s(curve_tok, sizeof(curve_tok), _TRUNCATE, "curve %s", curve_name);
        else               strncpy_s(curve_tok, "no curve", _TRUNCATE);
        append(curve_tok);
        append(g_pv_show_clip ? "clip"   : "no clip");
        append(g_scope_native ? "native" : "exposed");
    }
    if (wf_avail) append(g_wf_show ? WF_MODE_TOK[wf_mode] : "wf off");
    strncat_s(lbl, "###vf_scope_opts", _TRUNCATE); // stable ID: the visible half changes on every toggle

    if (ImGui::Button(lbl)) ImGui::OpenPopup("##vf_scope_opts_popup");
    if (ImGui::IsItemHovered()) {
        char tt[640];
        _snprintf_s(tt, sizeof(tt), _TRUNCATE,
                    "View options - how the instruments are DRAWN. Never affects what is recorded.\n"
                    "The button always states them, so nothing here can be silently on:\n\n"
                    "  curve      %s\n"
                    "  clip tint  %s\n"
                    "  histogram  %s\n"
                    "  waveform   %s",
                    scope_avail ? (g_scope_curve ? curve_name : "off") : "n/a",
                    scope_avail ? (g_pv_show_clip ? "on" : "off") : "n/a",
                    scope_avail ? (g_scope_native ? "native range (window slides)" : "exposed (bars slide)") : "n/a",
                    wf_avail ? (g_wf_show ? WF_MODE_TOK[wf_mode] + 3 : "off") : "n/a");
        ImGui::SetTooltip("%s", tt);
    }
    if (ImGui::BeginPopup("##vf_scope_opts_popup")) {
        if (scope_avail) {
            if (ImGui::Checkbox("Clip warning", &g_pv_show_clip)) save_addon_config(rt);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tint preview pixels that fall outside the capture window:\nred = clipped white, blue = crushed black. Live - updates as you drag Exposure.");
            if (ImGui::Checkbox("Native range", &g_scope_native)) save_addon_config(rt);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off (default): curve view - bars slide right as exposure rises, the capture window stays put.\nOn: bars = the buffer's full native range (fixed); the shaded capture window slides instead.");
            if (ImGui::Checkbox("Curve", &g_scope_curve)) save_addon_config(rt);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overlay the encode transfer curve (linear in -> code out) on the scope.\nActive curve = amber; the SDR Rec.709 curve is drawn dim as a baseline.\nSteeper = more code values per stop = finer precision there. That's the HDR-vs-SDR story:\nlog/PQ stay steep across the whole range; SDR 8-bit flattens (crushes) the highlights.");
            if (g_scope_curve) { ImGui::SameLine(); ImGui::TextDisabled("(%s)", curve_name); }
        }
        if (wf_avail) {
            if (scope_avail) ImGui::Separator();
            if (ImGui::Checkbox("Waveform", &g_wf_show)) save_addon_config(rt);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "RGB waveform / parade scope beneath the image.\nX = screen column, Y = pixel value.\n"
                "Works on any pass; on the depth pass Y is normalized depth across the frame.\n"
                "Off also skips its GPU build entirely.");
            if (g_wf_show) {
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::Combo("Mode##wf_mode", &g_wf_mode, "Overlay RGB\0Parade\0Luma\0")) save_addon_config(rt);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Overlay: R, G, B traces superimposed.\nParade: R | G | B panels side by side.\nLuma: single BT.709 luminance trace.");
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Brightness##wf_bright", &g_wf_bright, 2.0f, 48.0f, "%.0f")) save_cfg(rt);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Trace brightness: higher lifts faint/sparse values.");
            }
        }
        ImGui::EndPopup();
    }
}

// Vertical budget the Viewfinder must keep below the image for the instrument rows that follow it.
// Derived from the rows ACTUALLY DRAWN -- one widget row, one text row, a separator, the two named
// scope heights -- instead of the hardcoded 272/232/30/178 px this used to subtract, which had to
// be re-tuned by hand every time a row was added or moved. enc_pass is the previewed pass when its
// exposure is real (so the AE/EV row is drawn), else null.
static float preview_panel_reserve(bool scope_avail, const RecordPass* enc_pass, bool wf_avail) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float row  = ImGui::GetFrameHeightWithSpacing();    // a widget row (button / checkbox / slider)
    const float line = ImGui::GetTextLineHeightWithSpacing(); // a text-only row
    const float sep  = st.ItemSpacing.y * 2.0f + 1.0f;        // ImGui::Separator()
    float h = st.ItemSpacing.y;                               // the gap under the image itself
    if (scope_avail || wf_avail) h += row;                    // the "Scope: ..." options button
    if (scope_avail) {
        if (enc_pass) h += row + (enc_pass->ae_auto ? line : 0.0f); // AE/EV row (+ its "applied" readout)
        h += MPR_SCOPE_H + st.ItemSpacing.y;                  // the histogram strip (drawn into a Dummy)
        h += line;                                            // over-range warning -- reserved whether or not
                                                              // it fires, so the image can't jump when it does
        h += line;                                            // precision line
    }
    if (wf_avail && g_wf_show) h += sep + MPR_WF_H + 2.0f + line; // separator + scope image + its axis caption
    return h;
}

// The "full range" exposure scope. The histogram (g_pv.hist) is fixed native-stop data; we render
// it two ways and overlay the capture window. pass may be null (raw engine-buffer preview).
// The view-mode toggles that used to head this function (clip / native / curve) now live in the
// Scope options popup drawn one row above it (draw_scope_options).
static void draw_exposure_scope(effect_runtime* rt, RecordPass* pass) {
    const float exposure = pass ? pass->enc_exposure : 0.0f;
    const int   hdr      = pass ? pass->enc_hdr : HDR_PQ;
    float fp, wp, cp; curve_window_stops(hdr, &fp, &wp, &cp);

    if (pass) draw_exposure_controls(rt, *pass, /*compact=*/true);

    if (!g_pv.have_frame) { ImGui::TextDisabled("sampling buffer..."); return; }

    const bool  nat = g_scope_native;
    const bool  is_png = pass && pass->enc_codec == ENC_PNG; // PNG stores raw linear -> plot a linear curve, not PQ/Rec.709
    const float axLO = nat ? PreviewCap::HIST_LO : fp - 2.0f; // fixed per mode -> no frame-to-frame jitter
    const float axHI = nat ? PreviewCap::HIST_HI : cp + 2.0f;
    const float axSpan = axHI - axLO;
    // Capture-window edges on the CURRENT axis. Native: window slides by -exposure. Curve: fixed.
    const float wl = nat ? fp - exposure : fp;
    const float wr = nat ? cp - exposure : cp;
    const float ww = nat ? -exposure     : 0.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ScopeRect r{ o, ImGui::GetContentRegionAvail().x, MPR_SCOPE_H, axLO, axHI };
    const float W = r.W, H = r.H;
    auto CX = [&](float s) { return scope_x(r, s); };

    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), MPR_SCOPE_BG);

    // Graticule: a faint stop-interval grid, the one thing every waveform/histogram/false-color
    // monitor has that this scope didn't -- reads as a measuring instrument before any number does.
    for (float s = ceilf(axLO / 4.0f) * 4.0f; s <= axHI; s += 4.0f) {
        float gx = CX(s);
        dl->AddLine(ImVec2(gx, o.y), ImVec2(gx, o.y + H), MPR_GRID, 1.0f);
    }

    // Crush/clip render as diagonal hazard hatching -- the camera "zebra" clip-warning pattern --
    // instead of a flat tint, in the same blue/red as the floor/ceil lines below. Kept gets no zone
    // treatment at all: it's the absence of a warning, not a state worth advertising.
    auto hazard = [&](float x0, float x1, ImU32 col) {
        if (x1 <= x0) return;
        dl->PushClipRect(ImVec2(x0, o.y), ImVec2(x1, o.y + H), true);
        for (float sx = x0 - H; sx < x1 + H; sx += 9.0f)
            dl->AddLine(ImVec2(sx, o.y + H), ImVec2(sx + H, o.y), col, 2.2f);
        dl->PopClipRect();
    };
    hazard(o.x, CX(wl), mpr_alpha(MPR_CRUSH, 85));
    hazard(CX(wr), o.x + W, mpr_alpha(MPR_CLIP, 90));
    // Thin opaque identity caps at the top of each hazard zone for an at-a-glance color key.
    dl->AddRectFilled(ImVec2(o.x, o.y),    ImVec2(CX(wl),  o.y + 3), mpr_alpha(MPR_CRUSH, 235));
    dl->AddRectFilled(ImVec2(CX(wr), o.y), ImVec2(o.x + W, o.y + 3), mpr_alpha(MPR_CLIP, 235));

    scope_draw_bars(dl, r, nat ? 0.0f : exposure); // curve view shifts bars by exposure
    scope_draw_marklines(dl, r, wl, wr, ww);

    // Outer bezel + viewfinder-style corner brackets -- the framing cue that reads as "monitor,"
    // not "plot." Drawn before the labels below so an opaque label chip cleanly covers any overlap
    // rather than a bracket line drawing over legible text. Top corners only: the bottom two are
    // where the axis min/max labels live, and those have no chip to hide a line crossing them.
    dl->AddRect(o, ImVec2(o.x + W, o.y + H), mpr_alpha(MPR_WHITE, 40), 0.0f, 0, 1.0f);
    {
        const float bl = 9.0f; const ImU32 bc = mpr_alpha(MPR_TEXT_HI, 200);
        dl->AddLine(ImVec2(o.x + 0.5f, o.y + 0.5f), ImVec2(o.x + 0.5f + bl, o.y + 0.5f), bc, 1.4f);
        dl->AddLine(ImVec2(o.x + 0.5f, o.y + 0.5f), ImVec2(o.x + 0.5f, o.y + 0.5f + bl), bc, 1.4f);
        dl->AddLine(ImVec2(o.x + W - 0.5f, o.y + 0.5f), ImVec2(o.x + W - 0.5f - bl, o.y + 0.5f), bc, 1.4f);
        dl->AddLine(ImVec2(o.x + W - 0.5f, o.y + 0.5f), ImVec2(o.x + W - 0.5f, o.y + 0.5f + bl), bc, 1.4f);
    }

    // Transfer-curve overlay: output code (0..1, panel top = max code) vs the same stops axis. Uses
    // the identical exposed-linear mapping as the window/histogram, so the curve rides over the bars
    // it actually encodes. Curve steepness = codes/stop = precision, made directly comparable by
    // drawing the SDR Rec.709 curve dim behind any HDR curve.
    if (g_scope_curve) {
        auto plot = [&](ImU32 col, float thick, auto codefn) {
            ImVec2 prev; bool have = false;
            for (float px = 0.0f; px <= W; px += 2.0f) {
                float s    = axLO + axSpan * (px / W);   // axis stop
                float exps = nat ? s + exposure : s;     // exposed stop (window/curve live in exposed space)
                float code = codefn(powf(2.0f, exps));
                ImVec2 pt(o.x + px, o.y + H * (1.0f - code));
                if (have) dl->AddLine(prev, pt, col, thick);
                prev = pt; have = true;
            }
        };
        if (is_png) {
            plot(MPR_CURVE_PNG, 2.0f,
                 [](float lin){ float c = lin * HDR_PNG_SCALE * (1.0f / 65535.0f); return c > 1.0f ? 1.0f : c; });
        } else {
            if (hdr != HDR_REC709) // SDR baseline behind the active HDR curve
                plot(MPR_CURVE_SDR, 1.5f, [](float lin){ return hdr_tonemap_u16(lin) * (1.0f / 65535.0f); });
            plot(MPR_CURVE_ACTIVE, 2.0f, [&](float lin){ return curve_code(hdr, lin); });
        }
    }

    // Physical population split (identical in both modes): crush = below the native floor, clip =
    // above the ceil, kept = in between. Computed here so the zone %'s can be drawn onto the scope.
    double crush, kept, clip, tot;
    scope_population(exposure, fp, cp, &crush, &kept, &clip, &tot);
    const float dr = g_pv.span_hi - g_pv.span_lo, winw = cp - fp;

    // ---- On-scope labels (replace the old rows of text below the graph) ----
    // Anchored text with an optional dark backing chip so it stays legible over bars/zones
    // (mpr_tag in mpr_theme.inc); a leading swatch bonds a label to its zone/line color.
    const float fs = ImGui::GetFontSize();
    auto tag = [&](float x, float y, ImU32 col, int anchor, bool chip, const char* s, ImU32 swatch = 0) {
        mpr_tag(dl, x, y, o.x, o.x + W, col, anchor, chip, s, swatch);
    };
    char b[64];
    const float topY = o.y + 3, midY = o.y + H * 0.5f - fs * 0.5f, botY = o.y + H - fs - 2;
    // Axis extents at the bottom corners (replaces "native/curve stops A .. B").
    sprintf_s(b, "%+.0f st", axLO); tag(o.x + 2,     botY, MPR_TEXT_AXIS, 0, false, b);
    sprintf_s(b, "%+.0f st", axHI); tag(o.x + W - 2, botY, MPR_TEXT_AXIS, 2, false, b);
    // Capture-window edges + diffuse white, labelled on their lines along the top.
    tag(CX(wl), topY, mpr_alpha(MPR_CRUSH, 235), 0, true, "FLOOR");
    tag(CX(wr), topY, mpr_alpha(MPR_CLIP, 235), 2, true, "CEIL");
    sprintf_s(b, "WHITE %+.1f", ww); tag(CX(ww), topY, mpr_alpha(MPR_TEXT_HI, 235), 1, true, b);
    // (Curve identity is stated by the "Scope: curve PQ" options button above, not floated at the
    // curve's height here -- that height moves with the data, which was the one label that broke
    // the fixed top/mid/bottom row grid every other tag on this scope follows.)
    // Population % inside each zone (skip a zone too narrow to hold a label; drop the word before the swatch).
    if (CX(wl) - o.x > 34) {
        double pct = tot > 0 ? 100.0 * crush / tot : 0;
        sprintf_s(b, CX(wl) - o.x > 74 ? "%.1f%% CRUSH" : "%.1f%%", pct);
        tag((o.x + CX(wl)) * 0.5f, midY, MPR_CRUSH, 1, true, b, MPR_CRUSH);
    }
    if (o.x + W - CX(wr) > 34) {
        double pct = tot > 0 ? 100.0 * clip / tot : 0;
        sprintf_s(b, o.x + W - CX(wr) > 74 ? "%.1f%% CLIP" : "%.1f%%", pct);
        tag((CX(wr) + o.x + W) * 0.5f, midY, MPR_CLIP, 1, true, b, MPR_CLIP);
    }
    sprintf_s(b, "%.0f%% KEPT", tot > 0 ? 100.0 * kept / tot : 0);
    tag((CX(wl) + CX(wr)) * 0.5f, midY, MPR_KEPT, 1, true, b, MPR_KEPT);
    // Median as a small triangular tick on the baseline.
    { float mx = CX(g_pv.median_native + (nat ? 0.0f : exposure));
      dl->AddTriangleFilled(ImVec2(mx, o.y + H - 6), ImVec2(mx - 4, o.y + H), ImVec2(mx + 4, o.y + H), MPR_MEDIAN); }

    ImGui::Dummy(ImVec2(W, H));

    // Below the scope: only what the graph can't show spatially — an over-range warning, precision.
    // mpr_icon_warn (mpr_theme.inc) flags only the lines that need attention; OK lines get no icon.
    if (dr > winw) {
        mpr_icon_warn();
        ImGui::TextColored(MPR_WARN,
                           "scene spans %.1f st > %.1f st window - one end must clip or crush", dr, winw);
    }

    // Precision line: the bit-depth story the range band can't tell. codes/stop = 2^bits spread
    // across the curve window (avg; exact for uniform Log2, conservative-in-shadows for PQ). Below
    // ~32 codes/stop, smooth gradients band. PNG carries 16-bit linear -> effectively never bands.
    bool linear_png = pass && pass->enc_codec == ENC_PNG;
    int eff_bits;
    if (!pass)                              eff_bits = 10;                 // raw engine-buffer preview: PQ default
    else if (linear_png)                    eff_bits = 16;
    else if (pass->enc_codec == ENC_H264)   eff_bits = 8;                  // NVENC H.264 has no 10-bit
    else eff_bits = ((pass->enc_bits == BITS_10) || hdr == HDR_PQ || hdr == HDR_LOG2) ? 10 : 8; // PQ/Log2 force 10
    if (linear_png) {
        ImGui::TextColored(MPR_OK_DIM, "precision: 16-bit linear PNG - no banding");
    } else {
        float cps = powf(2.0f, (float)eff_bits) / winw; // avg code values per stop across the window
        bool  ok  = cps >= 32.0f;
        if (!ok) mpr_icon_warn();
        ImGui::TextColored(ok ? MPR_OK_DIM : MPR_WARN,
                           "precision: %d-bit -> ~%.0f codes/stop across %.1f st%s",
                           eff_bits, cps, winw, ok ? "" : "   banding risk - use 10-bit");
        if (hdr != HDR_REC709) { // contrast against what the 8-bit SDR path would give (the amber-vs-dim curves)
            float f7, w7, c7; curve_window_stops(HDR_REC709, &f7, &w7, &c7);
            ImGui::SameLine(); ImGui::TextDisabled(" vs SDR 8-bit ~%.0f/stop", 256.0f / (c7 - f7));
        }
    }
    ImGui::SameLine();
    help_marker("Scope: blue = crushed to black, green = captured, red = clipped white.\n"
                "Top labels sit on the window floor/ceil lines and diffuse white; the yellow tick is the median.\n"
                "Zone %'s are how much of the frame lands in each region.\n\n"
                "Curve overlay: the encode transfer curve (linear in -> code out). Amber = the active curve,\n"
                "dim teal = the SDR Rec.709 baseline. Where a curve is steep it spends more codes per stop\n"
                "(finer precision); where it flattens it crushes that tonal range. That is the HDR-vs-SDR\n"
                "difference at a glance - log/PQ stay steep everywhere, SDR 8-bit flattens the highlights.\n\n"
                "Native range ON: bars show the buffer's full range (fixed); the window slides with Exposure.\n"
                "OFF (curve view): bars slide with Exposure; the window stays put.");
}

// The Codec / Bits / Chroma / Quality controls for one pass, greying out invalid combinations.
static void draw_pass_encoding(effect_runtime* rt, RecordPass& p) {
    ImGui::SetNextItemWidth(170);
    if (ImGui::Combo("Codec", &p.enc_codec, ENC_CODEC_LABELS, IM_ARRAYSIZE(ENC_CODEC_LABELS))) save_addon_config(rt);

    if (p.enc_codec == ENC_HEVC || p.enc_codec == ENC_H264) {
        // HDR curve: only for packed-HDR sources on HEVC (PQ/Log2 need the 10-bit path).
        const bool hdr_src = pass_is_hdr_src(p) && (p.enc_codec == ENC_HEVC);
        const bool ten_forced = hdr_src && (p.enc_hdr == HDR_PQ || p.enc_hdr == HDR_LOG2);
        if (hdr_src) {
            ImGui::SetNextItemWidth(260);
            if (ImGui::Combo("HDR", &p.enc_hdr, ENC_HDR_LABELS, IM_ARRAYSIZE(ENC_HDR_LABELS))) save_addon_config(rt);
            if (ten_forced) p.enc_bits = BITS_10;
            ImGui::SameLine();
            char hdr_help[512];
            if (p.enc_hdr == HDR_PQ)
                sprintf_s(hdr_help, "HDR PQ (ST.2084) transfer in Rec.709 primaries - forces 10-bit 4:2:0.\n"
                                    "Import as PQ / ST.2084, Rec.709 - color-correct for grading.\n"
                                    "Convert to Rec.2020 at delivery if you need true HDR10.");
            else if (p.enc_hdr == HDR_LOG2)
                sprintf_s(hdr_help, "Fixed 2^%.0f..2^%.0f log2 window, max info (forces 10-bit 4:2:0).\nDecode in post with MPR_Log2_to_Linear.cube (or linear = 2^(code*%.0f%+.0f)).",
                          HDR_LOG2_FLOOR, HDR_LOG2_CEIL, HDR_LOG2_SPAN, HDR_LOG2_FLOOR);
            else
                sprintf_s(hdr_help, "Encodes the tonemapped SDR view (Rec.709). Pick PQ or Log2 to keep the HDR range.");
            help_marker(hdr_help);
            // Exposure: engine HDR buffers are often pre-exposure (dark). Lift here so the scene
            // lands in the usable part of the curve. Same shared control row as the Viewfinder
            // scope header (draw_exposure_controls above) -- AE tracks live and locks on record.
            // (Non-compact on purpose: the Bits combo is SameLine'd right after this, and the
            // compact variant stretches its slider to the full remaining width.)
            draw_exposure_controls(rt, p, /*compact=*/false);
        }

        const bool bits_ok = (p.enc_codec == ENC_HEVC); // NVENC H.264 has no 10-bit
        if (!bits_ok) p.enc_bits = BITS_8;
        ImGui::SameLine();
        if (!bits_ok || ten_forced) ImGui::BeginDisabled(); // PQ/Log2 lock bit depth to 10
        ImGui::SetNextItemWidth(90);
        if (ImGui::Combo("Bits", &p.enc_bits, ENC_BITS_LABELS, IM_ARRAYSIZE(ENC_BITS_LABELS))) save_addon_config(rt);
        if (!bits_ok || ten_forced) ImGui::EndDisabled();

        ImGui::SameLine();
        // NVENC 10-bit is 4:2:0 (p010) only — 10-bit 4:4:4 isn't reliable, so lock chroma there.
        const bool ten = (p.enc_bits == BITS_10);
        if (ten) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(150);
        if (ImGui::Combo("Chroma", &p.enc_chroma, ENC_CHROMA_LABELS, IM_ARRAYSIZE(ENC_CHROMA_LABELS))) save_addon_config(rt);
        if (ten) { ImGui::EndDisabled(); ImGui::SameLine(); help_marker("NVENC 10-bit is 4:2:0 (p010) only. For 10-bit 4:4:4 use the PNG codec."); }

        if (ImGui::Checkbox("Lossless", &p.enc_lossless)) save_addon_config(rt);
        if (!p.enc_lossless) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            if (ImGui::SliderInt("CQ", &p.enc_cq, 0, 51)) save_cfg(rt);
            ImGui::SameLine();
            help_marker("Constant quality, lower = better. ~18 is visually near-lossless and holds real-time far better than Lossless.");
        }
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderInt("Encoder speed", &p.enc_preset, 1, 7, "p%d")) save_cfg(rt);
        ImGui::SameLine();
        ImGui::TextDisabled(p.enc_preset <= 3 ? "fast (bigger files)" : p.enc_preset <= 5 ? "balanced" : "slow (best compression)");
        ImGui::SameLine();
        help_marker("Speed/size trade-off, NOT a quality setting: Lossless output is pixel-identical at any "
                    "speed, and CQ holds the same quality target - slower presets just compress the same "
                    "quality into smaller files, at more GPU cost (drop risk). p1 is the safe choice while "
                    "recording; raise it only if file size matters.");
    } else if (p.enc_codec == ENC_PNG) {
        ImGui::SameLine();
        ImGui::TextDisabled(pass_is_hdr_src(p) ? "16-bit/channel (from HDR source)" : "8-bit/channel (from LDR source)");
    } else { // ENC_CUSTOM
        ImGui::TextDisabled("FFmpeg args inserted between the raw input and the output file:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##Args", p.custom_args, sizeof(p.custom_args))) save_addon_config(rt);
    }
}

// Compact status cell for one pass in the table: Idle, or colored REC time + queue + drops.
static void pass_status_cell(RecordPass& p) {
    if (!p.worker.joinable()) { ImGui::TextDisabled("Idle"); return; }
    size_t q_size = p.queue_depth.load(std::memory_order_relaxed);
    uint32_t drops = p.dropped_frames.load(), frames = p.frame_count.load();
    float health = 1.0f - (float)q_size / 60.0f; if (health < 0) health = 0;
    ImVec4 color = MPR_OK;
    if (health < 0.5f) color = MPR_WARN;
    if (health < 0.2f || drops > 0) color = MPR_ERR;
    if (drops > 0) ImGui::TextColored(color, "REC %s  Q:%zu  !%u", format_duration(frames, g_fps).c_str(), q_size, drops);
    else           ImGui::TextColored(color, "REC %s  Q:%zu", format_duration(frames, g_fps).c_str(), q_size);
}

// Read a sampled view's backing resolution + format (for "what am I actually recording" labels).
static bool srv_dims(device* dev, resource_view srv, uint32_t* w, uint32_t* h, format* f) {
    if (!srv.handle) return false;
    resource res = dev->get_resource_from_view(srv);
    if (!res.handle) return false;
    resource_desc d = dev->get_resource_desc(res);
    *w = d.texture.width; *h = d.texture.height; *f = d.texture.format;
    return true;
}

// ---- Single-channel sources and the raw-SRV fallbacks ------------------------------------
// A 1-channel SRV handed to ImGui is sampled as float4(v, 0, 0, 1) and therefore paints PURE RED.
// That is why every PROCESSED path splats R across RGB (flag bit2 — preview_fmt_mono in
// mpr_preview.inc, pv_process, tile_capture_slot, waveform_capture). The raw-SRV fallbacks below
// CANNOT do that: a D3D11 SRV carries no component swizzle (only D3D12's Shader4ComponentMapping
// does), and ImGui lost its per-image tint overload in 1.91.9b — a tint multiplies, so it could
// not move R into G/B even if it were still there.
//
// So they must not draw it at all. A red rectangle is not a degraded view of a depth buffer, it is
// a WRONG one: it breaks the contract this UI is built on — that a tile's appearance is a fact
// about the buffer ("a black tile means a black buffer") — and it reads as a broken pass when the
// pass is fine. The native depth pass (r32_float) is the case that made this visible; every r8/r16
// engine mask buffer has the same shape.
static bool srv_is_mono(device* dev, resource_view srv) {
    uint32_t w = 0, h = 0; format f = format::unknown;
    if (!srv_dims(dev, srv, &w, &h, &f)) return false;
    return preview_fmt_mono(f);
}

// Stand-in for a raw mono thumbnail. Returns true when clicked, so call sites keep the
// click-to-preview affordance the ImGui::Image had — the Viewfinder renders it correctly, and
// getting the user there is the actual fix for wanting to see it.
static bool mono_thumb_placeholder(const char* id, ImVec2 size) {
    const bool clicked = ImGui::InvisibleButton(id, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg));
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border));
    const char* lbl = "1-channel";
    const ImVec2 ts = ImGui::CalcTextSize(lbl);
    if (ts.x <= size.x - 6.0f && ts.y <= size.y - 4.0f)
        dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), lbl);
    return clicked;
}
static const char* const MPR_TT_MONO_RAW =
    "Single channel (depth/mask). Only the Viewfinder and the processed tiles can show it as "
    "grayscale - drawn raw it would be solid red, so it is not drawn here. Click to open it.";

// ---- Pass table ----
// Every fragment (tables are split around an expanded row, and per section) uses the SAME id:
// ImGui treats them as instances of one table, so column widths stay in sync everywhere and a
// user resize applies to all of them. Stretch weights adapt to narrow panels; status is fixed.
static bool begin_pass_table() {
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                               ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable("passes", 4, tf)) return false;
    ImGui::TableSetupColumn("pass",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("buffer", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("encode", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, 115.0f);
    return true;
}

// One compact row: [x] name | WxH fmt | encode summary | status. Clicking the row (anywhere
// but the checkbox) expands/collapses its encoding details below the table fragment.
// Expansion is per-pass (multi-open) — any number of passes can be open at once.
static void draw_pass_row(effect_runtime* rt, RecordPass& p) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(p.name);
    const bool expanded = p.ui_expanded;
    // _TRUNCATE, not sprintf_s: pass_display_name can be a technique or buffer name of arbitrary
    // length, and the secure CRT ABORTS the game on overflow rather than truncating.
    char sel[112]; _snprintf_s(sel, _TRUNCATE, MPR_GLYPH_PAD "%s", pass_display_name(p));
    // Row-spanning selectable first, checkbox overlapped on top after (AllowOverlap gives the
    // later-submitted checkbox interaction priority over the selectable underneath it).
    const float row_x = ImGui::GetCursorPosX();
    const float cb_w  = ImGui::GetFrameHeight() + 6.0f;
    ImGui::SetCursorPosX(row_x + cb_w);
    // Label origin, captured before submitting: a SpanAllColumns selectable's item rect starts at
    // the column edge, not at its text, so the expander chevron can't be derived from that rect.
    const float sel_x = ImGui::GetCursorScreenPos().x;
    if (ImGui::Selectable(sel, expanded, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
        p.ui_expanded = !expanded;
    mpr_button_glyph(expanded ? MPR_GLY_CHEV_D : MPR_GLY_CHEV_R, sel, MPR_GLYPH_INK, sel_x);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nClick to edit encoding / preview.", pass_source_caption(p));
    ImGui::SameLine();
    ImGui::SetCursorPosX(row_x);
    if (ImGui::Checkbox("##on", &p.enabled)) save_addon_config(rt);

    // Resolution + format of what this pass captures (dim until the buffer is bound this frame).
    resource_view srv = pass_preview_srv(rt, p);
    uint32_t bw = 0, bh = 0; format bf = format::unknown;
    bool have_dims = srv_dims(rt->get_device(), srv, &bw, &bh, &bf);
    ImGui::TableSetColumnIndex(1);
    if (have_dims) ImGui::Text("%ux%u %s", bw, bh, probe_format_name(bf));
    else           ImGui::TextDisabled("not bound");

    ImGui::TableSetColumnIndex(2);
    char enc[128]; pass_encode_summary(p, enc, sizeof(enc));
    ImGui::TextUnformatted(enc);

    ImGui::TableSetColumnIndex(3);
    pass_status_cell(p);
    ImGui::PopID();
}

// Expanded detail block for one pass: thumbnail + preview, encoding controls, output name.
static void draw_pass_details(effect_runtime* rt, RecordPass& p) {
    ImGui::PushID(p.name);
    ImGui::Indent(26.0f);
    resource_view srv = pass_preview_srv(rt, p);
    if (srv.handle) {
        // Raw view, never processed — so a 1-channel pass (native depth) would draw solid red here.
        const bool mono = srv_is_mono(rt->get_device(), srv);
        const bool hit  = mono ? mono_thumb_placeholder("##thumb", ImVec2(96, 54))
                               : (ImGui::Image((ImTextureID)srv.handle, ImVec2(96, 54)), ImGui::IsItemClicked());
        if (hit) { g_preview_pass = &p; g_preview_handle = 0; g_preview_open = true; g_debug_view = false; save_addon_config(rt); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mono ? MPR_TT_MONO_RAW : "Click to preview");
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    if (ImGui::SmallButton(MPR_GLYPH_PAD "Preview")) { g_preview_pass = &p; g_preview_handle = 0; g_preview_open = true; g_debug_view = false; save_addon_config(rt); }
    mpr_button_glyph(MPR_GLY_EYE, MPR_GLYPH_PAD "Preview", MPR_GLYPH_INK);
    ImGui::SameLine();
    const bool dv_on = g_debug_view && g_preview_pass == &p;
    if (dv_on) ImGui::PushStyleColor(ImGuiCol_Button, MPR_BTN_ON);
    const char* dv_label = dv_on ? MPR_GLYPH_PAD "Debug View [ON]" : MPR_GLYPH_PAD "Debug View";
    if (ImGui::SmallButton(dv_label)) {
        if (dv_on) g_debug_view = false;
        else { g_preview_pass = &p; g_preview_handle = 0; g_preview_open = true; g_debug_view = true; save_addon_config(rt); }
    }
    mpr_button_glyph(MPR_GLY_RETICLE, dv_label, dv_on ? MPR_TEXT_HI : MPR_GLYPH_INK);
    if (dv_on) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", MPR_TT_DEBUG_VIEW);
    bool removed = false;
    if (p.is_custom) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) { remove_custom_pass(rt, p); removed = true; }
    }
    if (!removed) {
        ImGui::SameLine(0, 12);
        char tag[96]; pass_file_tag(p, tag, sizeof(tag)); // FX passes are filed under their technique name
        if (p.enc_codec == ENC_PNG) ImGui::TextDisabled("-> %s_%s\\frame_*.png", g_session_time, tag);
        else                        ImGui::TextDisabled("-> %s_%s.mp4", g_session_time, tag);
        draw_pass_encoding(rt, p);
    }
    ImGui::EndGroup();
    ImGui::Unindent(26.0f);
    ImGui::Spacing();
    ImGui::PopID();
}

// Rows for one section of the pass table, splitting the table around each expanded row so its
// details render full-width. All fragments are instances of one shared table (aligned columns).
static void draw_pass_section(effect_runtime* rt, const int* idxs, int count) {
    bool in_table = false;
    for (int i = 0; i < count; ++i) {
        const int idx = idxs[i];
        if (!in_table) {
            if (!begin_pass_table()) return;
            in_table = true;
        }
        draw_pass_row(rt, g_passes[idx]);
        if (g_passes[idx].ui_expanded) {
            ImGui::EndTable(); in_table = false;
            draw_pass_details(rt, g_passes[idx]);
        }
    }
    if (in_table) ImGui::EndTable();
}

// ---- UI ----
// Header strip: the primary Record action, a neutral load readout, and REACTIVE warnings only
// - drops are reported when they actually happen (live, and retrospectively for the last run),
// naming the culprit pass. No predictive "this might drop" noise.
static void draw_header_strip(effect_runtime* rt) {
    int nvenc = 0, total = 0;
    for (auto& p : g_passes) {
        if (!p.enabled || (p.is_custom && !p.promoted)) continue;
        total++;
        if (p.enc_codec != ENC_PNG) nvenc++;
    }

    // Key hints on the buttons. GetKeyNameTextA can hand back a long name for an exotic key, and a
    // chord adds modifiers on top, so these format with _TRUNCATE — sprintf_s would abort the
    // process on overflow. An unbound key => no parenthetical at all.
    char keyhint[80] = "";
    if (g_hotkeys[HK_RECORD].vk != 0)
        _snprintf_s(keyhint, _TRUNCATE, "  (%s)", hotkey_label(HK_RECORD));
    char skey[80] = "";
    if (g_hotkeys[HK_STILL].vk != 0)
        _snprintf_s(skey, _TRUNCATE, "  (%s)", hotkey_label(HK_STILL));

    // Capture block: ONE row, not two stacked full-width slabs. Both actions matter, but two
    // competing primaries read as none — so Record takes whatever width is left and Still is
    // sized to its own text. Still's width is computed from its WIDEST label, not the current
    // one, so the row doesn't reflow (and Record doesn't resize) mid-sequence; and it's capped at
    // a share of the strip so Record stays visibly dominant in a narrow panel too.
    // Take Still is available whether or not a take is rolling (a still uses its own staging and
    // costs the recording nothing but one extra GPU copy on the shutter frame). Busy = the
    // sequence or its writer is still working; a second press then would interleave two bursts
    // into one folder.
    char slbl[128];
    const float shot_t = shot_timer_left();
    if (shot_t > 0.0f)       _snprintf_s(slbl, _TRUNCATE, MPR_GLYPH_PAD "Still in %d...", (int)(shot_t + 0.999f));
    else if (shot_is_busy()) _snprintf_s(slbl, _TRUNCATE, MPR_GLYPH_PAD "Taking still...");
    else                     _snprintf_s(slbl, _TRUNCATE, MPR_GLYPH_PAD "Take Still%s", skey);

    const ImGuiStyle& gst = ImGui::GetStyle();
    char srest[128]; _snprintf_s(srest, _TRUNCATE, MPR_GLYPH_PAD "Take Still%s", skey);
    float still_w = ImGui::CalcTextSize(srest).x;
    const float busy_w = ImGui::CalcTextSize(MPR_GLYPH_PAD "Taking still...").x;
    if (busy_w > still_w) still_w = busy_w;
    still_w += gst.FramePadding.x * 2.0f + 6.0f;
    const float still_cap = ImGui::GetContentRegionAvail().x * 0.42f;
    if (still_w > still_cap) still_w = still_cap;
    const float rec_w = -(still_w + gst.ItemSpacing.x); // negative = "fill, minus the Still button"

    if (!g_recording) {
        char lbl[128]; _snprintf_s(lbl, _TRUNCATE, MPR_GLYPH_PAD "Record %d Pass%s%s", total, total == 1 ? "" : "es", keyhint);
        const char* rlbl = total ? lbl : MPR_GLYPH_PAD "Record  -  no passes enabled";
        if (total == 0) ImGui::BeginDisabled();
        if (ImGui::Button(rlbl, ImVec2(rec_w, 0))) toggle_recording();
        // Draw-list glyphs don't inherit BeginDisabled's alpha, so the disc is faded by hand.
        mpr_button_glyph(MPR_GLY_RECORD, rlbl, total ? MPR_REC_DISC : mpr_alpha(MPR_REC_DISC, 70));
        if (total == 0) ImGui::EndDisabled();
    } else {
        char lbl[128]; _snprintf_s(lbl, _TRUNCATE, MPR_GLYPH_PAD "Stop Recording%s", keyhint);
        ImGui::PushStyleColor(ImGuiCol_Button, MPR_BTN_STOP);
        if (ImGui::Button(lbl, ImVec2(rec_w, 0))) toggle_recording();
        ImGui::PopStyleColor();
        mpr_button_glyph(MPR_GLY_STOP, lbl, MPR_TEXT_HI);
    }

    ImGui::SameLine(0.0f, gst.ItemSpacing.x);
    {
        const bool busy = shot_is_busy();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(slbl, ImVec2(still_w, 0))) shot_trigger();
        mpr_button_glyph(MPR_GLY_SHUTTER, slbl, busy ? mpr_alpha(MPR_GLYPH_INK, 80) : MPR_GLYPH_INK);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !busy)
            ImGui::SetTooltip("One frame: the game view plus every enabled pass, all from the same "
                              "frame. Formats and burst/timer are in the Stills tab.");
    }

    if (total > 0)
        ImGui::TextDisabled("%d pass%s - %d NVENC session%s", total, total == 1 ? "" : "es", nvenc, nvenc == 1 ? "" : "s");

    // Reactive drop report. Counters reset at the next recording start, so after a stop this
    // doubles as the retrospective verdict for the last run, naming the worst pass.
    uint32_t drops = 0, worst = 0; const char* worst_nm = nullptr;
    for (auto& p : g_passes) {
        if (p.is_custom && !p.promoted) continue;
        const uint32_t d = p.dropped_frames.load();
        if (d == 0) continue;
        drops += d;
        if (d >= worst) { worst = d; worst_nm = pass_display_name(p); }
    }
    if (drops > 0 && worst_nm) {
        if (g_recording)
            ImGui::TextColored(MPR_ERR, "%u dropped - bottleneck: %s%s%s",
                               drops, worst_nm, g_drop_reason[0] ? " - " : "", g_drop_reason);
        else
            ImGui::TextColored(MPR_WARN, "last recording: %u dropped - bottleneck: %s%s%s",
                               drops, worst_nm, g_drop_reason[0] ? " - " : "", g_drop_reason);
        ImGui::SameLine();
        help_marker("All passes drop together (frame lock), but the named pass is what backed up.\n\n"
                    "Fixes, on that pass: uncheck Lossless (CQ ~18 looks near-identical), lower Encoder "
                    "speed toward p1, or switch other passes to PNG (PNG doesn't use an NVENC session). "
                    "Beyond that: fewer passes, lower capture FPS, or lower game resolution.");
    }

    // Reactive encoder report. Like the drop report above, the flags are cleared only when the
    // next take spawns a worker, so after a stop this is the retrospective verdict for the last
    // run: which passes produced no file, and why. Nothing here is predictive — a pass is only
    // named once its FFmpeg has actually failed.
    int dead_count = 0;
    for (auto& p : g_passes) {
        if (!(p.enabled && p.worker.joinable())) continue;
        if (!p.enc_failed.load(std::memory_order_acquire)) continue;
        ImGui::TextColored(MPR_ERR, "%s'%s' encoder failed - %s",
                           g_recording ? "" : "last recording: ", pass_display_name(p), p.enc_error);
        dead_count++;
    }
    if (dead_count) {
        ImGui::SameLine();
        help_marker("This pass wrote no file (or a truncated one); the others were unaffected.\n\n"
                    "Most common cause with several passes rolling: every video pass opens its own "
                    "NVENC session, and consumer GeForce drivers cap how many can run at once "
                    "(3 on older drivers, 8 since 2023) - the passes past the cap fail at startup. "
                    "Fixes: fewer video passes, or switch some to PNG sequence (PNG uses no NVENC "
                    "session at all).\n\n"
                    "Otherwise check the FFmpeg path in Setup, free disk space, and the tail of "
                    "captures\\ffmpeg_log.txt, which has FFmpeg's own error.");
    }

    // The classic black-recording trap, surfaced in the panel instead of only the log.
    if ((g_passes[0].enabled || g_passes[1].enabled || g_passes[2].enabled) && !is_setup_active(rt))
        ImGui::TextColored(MPR_ERR,
                           "'Multipass Recorder Setup' technique is OFF - depth/normals/world record black!");
}

// Live per-pass recording telemetry: effective write rate vs target, frames written, drops, and
// the encoder queue backlog (the number that reveals what's choking). Effective fps is sampled
// ~2x/sec so it's readable rather than jittering every overlay frame.
// ONE implementation of the per-pass telemetry table, shared by the settings panel (full) and
// the always-on OSD (compact: short names, no 'written' column, fixed-fit sizing) so the numbers,
// thresholds and colors always match. Forward-declared in mpr_osd.inc.
static void draw_pass_stats_table(bool compact) {
    sample_pass_fps();
    const ImGuiTableFlags tf = compact
        ? ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX | ImGuiTableFlags_RowBg
        : ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable(compact ? "##mpr_stats_osd" : "##mpr_stats", compact ? 4 : 5, tf)) return;
    ImGui::TableSetupColumn("pass");
    ImGui::TableSetupColumn(compact ? "fps" : "fps/target");
    ImGui::TableSetupColumn("queue");
    ImGui::TableSetupColumn(compact ? "drop" : "dropped");
    if (!compact) ImGui::TableSetupColumn("written");
    ImGui::TableHeadersRow();
    for (int i = 0; i < N_FIXED + N_CUSTOM; ++i) {
        auto& p = g_passes[i];
        if (!(p.enabled && p.worker.joinable())) continue;
        // OSD: only the short leading token (e.g. "o01"), not the full verbose label
        // ("o01 r11g11b10_float 1920x1080"), so the overlay stays narrow.
        char snm[16];
        const char* nm = pass_display_name(p);
        if (compact) { pass_short_name(p, snm, sizeof(snm)); nm = snm; }
        size_t qd = p.queue_depth.load(std::memory_order_relaxed);
        uint32_t fc = p.frame_count.load(), dr = p.dropped_frames.load();
        double fps = g_pass_fps[i];
        bool behind = fps > 0 && fps < g_fps * 0.9;
        // A pass whose encoder died keeps its row — its fps/queue would otherwise read as a
        // healthy zero — but says DEAD instead of numbers that no longer mean anything. The
        // frames column still shows what it got written before it went, which is the useful part.
        const bool dead = p.enc_failed.load(std::memory_order_acquire);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextColored(dead ? MPR_ERR : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", nm);
        ImGui::TableSetColumnIndex(1);
        if (dead) {
            ImGui::TextColored(MPR_ERR, "DEAD");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.enc_error);
            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(3); if (dr) ImGui::TextColored(MPR_ERR, "%u", dr); else ImGui::TextDisabled("-");
            if (!compact) { ImGui::TableSetColumnIndex(4); ImGui::Text("%u", fc); }
            continue;
        }
        ImGui::TextColored(behind ? MPR_WARN : MPR_OK_DIM, "%.0f/%d", fps, g_fps);
        ImGui::TableSetColumnIndex(2); ImGui::TextColored(qd >= 45 ? MPR_ERR : qd >= 20 ? MPR_WARN : MPR_DIM, compact ? "%zu" : "%zu/60", qd);
        ImGui::TableSetColumnIndex(3); if (dr) ImGui::TextColored(MPR_ERR, "%u", dr); else ImGui::TextDisabled("-");
        if (!compact) { ImGui::TableSetColumnIndex(4); ImGui::Text("%u", fc); }
    }
    ImGui::EndTable();
}

static void draw_recording_stats(effect_runtime* rt) {
    int active = 0; uint32_t total_written = 0, total_dropped = 0;
    for (int i = 0; i < N_FIXED + N_CUSTOM; ++i) {
        auto& p = g_passes[i];
        if (!(p.enabled && p.worker.joinable())) continue;
        active++;
        uint32_t fc = p.frame_count.load();
        if (fc > total_written) total_written = fc;
        total_dropped += p.dropped_frames.load();
    }

    ImGui::TextColored(total_dropped ? MPR_ERR : MPR_OK, "[REC] %s   %d pass%s   %u frames written",
                       format_hms(recording_elapsed_sec()).c_str(), active, active == 1 ? "" : "es", total_written);
    ImGui::SameLine();
    help_marker("fps below target or a rising queue = that pass is the bottleneck; queue at 60 = frames drop.");
    if (total_dropped) {
        ImGui::TextColored(MPR_ERR, "%u DROPPED", total_dropped);
        if (g_drop_reason[0]) { ImGui::SameLine(); ImGui::TextColored(MPR_WARN, "bottleneck: %s", g_drop_reason); }
    }
    draw_pass_stats_table(/*compact=*/false);
    ImGui::Separator();
}

// ---- Window layout anchoring (see LayoutMode in mpr_state.inc for the why) ----
// The panel is a tall edge column (the Buffers tab needs the height for its gallery); the
// Viewfinder sits beside it in the remaining space, so the two never open on top of each other.
// Both rects are derived from the CURRENT DisplaySize every time they're applied, which is what
// makes a launch at a different resolution land sanely instead of restoring stale coordinates.
struct LayoutRect { float x, y, w, h; };
static const float LAYOUT_MARGIN = 20.0f, LAYOUT_GAP = 8.0f;

static LayoutRect layout_panel_rect() {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    float w = ds.x * 0.5f; if (w > 700.0f) w = 700.0f; if (w < 360.0f) w = 360.0f; // wide enough for the gallery's info column, but keep to one half
    float h = ds.y - 2.0f * LAYOUT_MARGIN; if (h < 400.0f) h = 400.0f;
    float x = (g_layout_mode == LAYOUT_RIGHT) ? ds.x - w - LAYOUT_MARGIN : LAYOUT_MARGIN;
    if (x < 0.0f) x = 0.0f;
    return { x, LAYOUT_MARGIN, w, h };
}

// Viewfinder: parked in the space the panel column leaves free, on the opposite side.
static LayoutRect layout_preview_rect() {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const LayoutRect p = layout_panel_rect();
    float avail = ds.x - p.w - 2.0f * LAYOUT_MARGIN - LAYOUT_GAP;
    float w = 720.0f; if (w > avail) w = avail; if (w < 320.0f) w = 320.0f;
    float h = 460.0f; if (h > ds.y - 2.0f * LAYOUT_MARGIN) h = ds.y - 2.0f * LAYOUT_MARGIN;
    float x = (g_layout_mode == LAYOUT_RIGHT) ? p.x - LAYOUT_GAP - w : p.x + p.w + LAYOUT_GAP;
    if (x < 0.0f) x = 0.0f;
    return { x, LAYOUT_MARGIN, w, h };
}

// When an anchor actually gets written to a window. Always = "move it now" (the once-per-launch
// snap, or an explicit Apply now); FirstUseEver = "only if ImGui has nothing saved", i.e. the
// user's own dragging wins for the rest of the session.
static ImGuiCond layout_cond() {
    if (g_layout_force_frame != 0xffffffff && g_live_frame - g_layout_force_frame <= 1)
        return ImGuiCond_Always; // "Apply now", spans both draw callbacks
    if (g_layout_mode == LAYOUT_FREE) return ImGuiCond_FirstUseEver;
    // Once per game launch: forced for the whole of the first frame the panel draws on, so the
    // Viewfinder (drawn later in that same frame) is anchored by the same pass.
    if (g_layout_applied_frame == 0xffffffff || g_layout_applied_frame == g_live_frame)
        return ImGuiCond_Always;
    return ImGuiCond_FirstUseEver;
}

// Display size the current layout was derived from. ImGui clamps a restored window's *position*
// into the viewport every frame but not its *size*, so a panel laid out at 4K and restored at
// 1080p stays taller than the screen. Re-anchoring on a resolution change (game launched at a new
// resolution, or a mid-session windowed/fullscreen switch) re-derives both.
static float g_layout_ds_w = 0.0f, g_layout_ds_h = 0.0f;

// Applied to the settings window from inside the overlay callback -- ReShade owns the Begin, so we
// position the *current* window rather than the next one.
static void seed_window_layout() {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x != g_layout_ds_w || ds.y != g_layout_ds_h) {
        if (g_layout_mode != LAYOUT_FREE && g_layout_ds_w != 0.0f)
            g_layout_force_frame = g_live_frame; // resolution changed under a saved layout -> re-derive
        g_layout_ds_w = ds.x; g_layout_ds_h = ds.y;
    }
    const LayoutRect r = layout_panel_rect();
    const ImGuiCond cond = layout_cond();
    ImGui::SetWindowPos(ImVec2(r.x, r.y), cond);
    ImGui::SetWindowSize(ImVec2(r.w, r.h), cond);
}

// ---- Viewfinder: floating large / full-screen view of the selected pass OR engine buffer ----
// Deliberately its own window, not a tab: it stays visible beside the panel while encode settings
// are edited in the Record tab. It can be opened from several places (pass table rows, Buffers
// gallery thumbnails/buttons), so it's drawn exactly once per frame from on_reshade_overlay
// (mpr_osd.inc) rather than nested in on_draw_settings -- drawing it from more than one caller
// could double-call ImGui::Begin("Viewfinder") in the same frame, invalid ImGui usage that
// silently no-ops the window instead of drawing it.

// Image zoom/pan (session-only, reset when the preview closes): pan is the uv-space center of
// the visible window, zoom 1 = the whole buffer fitted.
static float g_pv_zoom  = 1.0f;
static float g_pv_pan_x = 0.5f, g_pv_pan_y = 0.5f;
// Screen-space rect of the last preview image pv_draw_image drew this frame, so the waveform scope
// can sit directly beneath it at the same width/offset (columns line up = the pass's aspect ratio).
static float g_pv_img_x0 = 0.0f, g_pv_img_w = 0.0f;
static const char* PV_CHANNEL_LABELS[] = { "RGB", "R", "G", "B", "A", "Luma" }; // indices = g_pv_channel

// Aspect-fitted image with pixel-peeping controls: mouse wheel zooms about the cursor, drag pans
// (only once zoomed), double-click resets. Drawn through the window draw list so the zoom is a
// pure uv-window change -- works identically for processed bitmaps and raw SRVs.
static void pv_draw_image(resource_view tex, float iw, float ih, float box_h) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    float box_w = avail.x;
    if (box_h > avail.y) box_h = avail.y;
    if (box_h < 80.0f) box_h = 80.0f;
    if (iw <= 0 || ih <= 0 || box_w <= 8.0f) return;
    float s = (box_w / iw < box_h / ih) ? box_w / iw : box_h / ih;
    if (s <= 0) s = 1.0f;
    const ImVec2 img(iw * s, ih * s);
    const float xoff = (box_w - img.x) * 0.5f; // center horizontally in the box
    if (xoff > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + xoff);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    g_pv_img_x0 = p0.x; g_pv_img_w = img.x; // publish for the waveform scope's horizontal alignment
    ImGui::InvisibleButton("##pv_img", img); // claims the rect + captures drag so the window doesn't move
    float half = 0.5f / g_pv_zoom;
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            float nz = g_pv_zoom * powf(1.25f, io.MouseWheel);
            if (nz < 1.0f) nz = 1.0f; else if (nz > 16.0f) nz = 16.0f;
            const float nhalf = 0.5f / nz;
            // Keep the texel under the cursor stationary while the uv window shrinks/grows.
            const float fx = (io.MousePos.x - p0.x) / img.x, fy = (io.MousePos.y - p0.y) / img.y;
            const float u = (g_pv_pan_x - half) + fx * 2.0f * half;
            const float v = (g_pv_pan_y - half) + fy * 2.0f * half;
            g_pv_zoom = nz;
            g_pv_pan_x = u - (2.0f * fx - 1.0f) * nhalf;
            g_pv_pan_y = v - (2.0f * fy - 1.0f) * nhalf;
            half = nhalf;
        }
        if (g_pv_zoom > 1.0f && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            g_pv_pan_x -= io.MouseDelta.x / img.x * 2.0f * half;
            g_pv_pan_y -= io.MouseDelta.y / img.y * 2.0f * half;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { g_pv_zoom = 1.0f; g_pv_pan_x = g_pv_pan_y = 0.5f; half = 0.5f; }
    }
    auto clampc = [&](float c) { return c < half ? half : (c > 1.0f - half ? 1.0f - half : c); };
    g_pv_pan_x = clampc(g_pv_pan_x); g_pv_pan_y = clampc(g_pv_pan_y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage((ImTextureID)tex.handle, p0, ImVec2(p0.x + img.x, p0.y + img.y),
                 ImVec2(g_pv_pan_x - half, g_pv_pan_y - half), ImVec2(g_pv_pan_x + half, g_pv_pan_y + half));
    if (g_pv_zoom > 1.001f) {
        char zb[64]; sprintf_s(zb, "x%.1f  drag to pan, double-click to reset", g_pv_zoom);
        const float fs = ImGui::GetFontSize();
        dl->AddRectFilled(ImVec2(p0.x + 4, p0.y + img.y - fs - 8), ImVec2(p0.x + 12 + ImGui::CalcTextSize(zb).x, p0.y + img.y - 2), mpr_alpha(MPR_CHIP, 200), 3.0f);
        dl->AddText(ImVec2(p0.x + 8, p0.y + img.y - fs - 5), mpr_alpha(MPR_TEXT_HI, 230), zb);
    }
}

// RGB waveform / parade scope. Its on/off, mode and brightness are VIEW MODES and live in the
// Scope options popup above the image (draw_scope_options) -- this function draws only the
// instrument itself, so the waveform costs no permanent chrome when it is off. When on, the
// GPU-built waveform texture (mpr_waveform.inc) is drawn beneath the preview image, aligned to it
// horizontally so a column here sits under the same column there, with a 0-100 value scale +
// legend + units. The separator is drawn here (not by the caller) so an off waveform leaves no
// stray rule behind.
static void draw_waveform_scope(effect_runtime* rt) {
    (void)rt;
    if (!g_wf_show) return;
    ImGui::Separator();

    const resource_view wf = waveform_display_srv();
    if (!wf.handle) { ImGui::TextDisabled("Waveform: waiting for a live GPU frame..."); return; }

    // ---- the scope image, aligned under the preview image (matches the pass aspect) ----
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 cur   = ImGui::GetCursorScreenPos();
    const float  img_h = MPR_WF_H;
    const float  x0 = (g_pv_img_w > 8.0f) ? g_pv_img_x0 : cur.x;      // align to the frame above,
    const float  w  = (g_pv_img_w > 8.0f) ? g_pv_img_w  : avail.x;    // else fall back to full width
    const float  y0 = cur.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + img_h), MPR_SCOPE_BG);
    dl->AddImage((ImTextureID)wf.handle, ImVec2(x0, y0), ImVec2(x0 + w, y0 + img_h)); // full width => columns align
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + img_h), MPR_OUTLINE);

    // Y value scale 0..100 (top = white/100, bottom = black/0), backed for legibility over the trace.
    for (int k = 0; k <= 4; ++k) {
        const float y = y0 + img_h * (k / 4.0f);
        char lb[8]; sprintf_s(lb, "%d", 100 - k * 25);
        const ImVec2 ts = ImGui::CalcTextSize(lb);
        const float ty = (k == 0) ? y + 1.0f : (k == 4 ? y - ts.y - 1.0f : y - ts.y * 0.5f);
        dl->AddRectFilled(ImVec2(x0 + 1, ty - 1), ImVec2(x0 + ts.x + 6, ty + ts.y + 1), mpr_alpha(MPR_CHIP, 190), 2.0f);
        dl->AddText(ImVec2(x0 + 3, ty), MPR_TEXT_AXIS, lb);
    }

    // Channel legend: parade panel labels, or an R/G/B (Luma) key in the top-right corner.
    const ImU32 cR = MPR_CH_R, cG = MPR_CH_G, cB = MPR_CH_B;
    if (g_wf_mode == 1) {
        const char* nm[3] = { "R", "G", "B" }; const ImU32 cc[3] = { cR, cG, cB };
        for (int t = 0; t < 3; ++t) {
            const ImVec2 ts = ImGui::CalcTextSize(nm[t]);
            dl->AddText(ImVec2(x0 + w * (t + 0.5f) / 3.0f - ts.x * 0.5f, y0 + 3), cc[t], nm[t]);
        }
    } else if (g_wf_mode == 0) {
        const float lx = x0 + w - 42.0f;
        dl->AddText(ImVec2(lx, y0 + 3), cR, "R"); dl->AddText(ImVec2(lx + 14, y0 + 3), cG, "G"); dl->AddText(ImVec2(lx + 28, y0 + 3), cB, "B");
    } else {
        const ImVec2 ts = ImGui::CalcTextSize("Luma");
        dl->AddText(ImVec2(x0 + w - ts.x - 6, y0 + 3), MPR_TEXT_HI, "Luma");
    }

    ImGui::Dummy(ImVec2(avail.x, img_h + 2.0f)); // reserve the drawn area so following text flows below
    const bool is_depth = g_preview_pass && strcmp(g_preview_pass->name, "depth") == 0;
    if (is_depth) ImGui::TextDisabled("Y = normalized depth 0-100 (near-far, engine-dependent)   X = screen left-right");
    else          ImGui::TextDisabled("Y = value 0-100%%   X = screen left-right");
}

// ---- Viewfinder: pass picker + shader-pass (.fx uniform) remote controls -------------

// Cycle the preview through the recordable passes without returning to the pass table. Lists the
// 6 fixed passes always, plus any promoted custom (Buffers-tab) slot. Selecting one re-points the
// viewfinder and persists it (so it reopens on the same pass next session).
static void draw_preview_pass_picker(effect_runtime* rt) {
    RecordPass* items[N_FIXED + N_CUSTOM]; int cur = -1;
    const int n = preview_pass_list(items, cur); // same candidate rule as the pass hotkeys (mpr_control.inc)
    auto label_of = [](RecordPass* p) { return pass_display_name(*p); };
    auto pick = [&](int idx) {
        if (idx < 0 || idx >= n) return;
        g_preview_pass = items[idx]; g_preview_handle = 0; g_preview_open = true;
        save_addon_config(rt);
    };
    ImGui::TextUnformatted("Pass"); ImGui::SameLine();
    if (mpr_glyph_button("##pv_prev", MPR_GLY_CHEV_L, MPR_GLYPH_INK)) pick(cur < 0 ? 0 : (cur - 1 + n) % n);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    const char* cur_label = (cur >= 0) ? label_of(items[cur])
                          : (g_preview_handle ? "(engine buffer)" : "(none)");
    if (ImGui::BeginCombo("##pv_pass", cur_label)) {
        for (int i = 0; i < n; ++i) {
            const bool sel = (i == cur);
            if (ImGui::Selectable(label_of(items[i]), sel)) pick(i);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (mpr_glyph_button("##pv_next", MPR_GLY_CHEV_R, MPR_GLYPH_INK)) pick(cur < 0 ? 0 : (cur + 1) % n);
}

// Defined further down with the Effects tab (it needs the debug-uniform scan): the two controls of
// an FX-layer pass. Surfaced in the Viewfinder as well as its gallery row, because choosing a debug
// output is entirely a look-at-it decision -- you want the big image while you cycle the values.
static void draw_fx_pass_controls_inline(effect_runtime* rt, RecordPass& p);

// Depth / normals controls, in the Viewfinder so you can watch the image while you set them.
//
// These used to remote-control MultipassRecorder.fx uniforms over find_uniform_variable /
// set_uniform_value. The shader has no uniforms any more -- the math is the addon's now
// (mpr_native.inc) -- so they bind straight to the pass's own fields. Three things got better for
// free: the values are PER PASS (two depth passes can hold different ranges), they persist in our
// ini instead of ReShade's preset, and ReShade's Performance Mode can no longer compile them away,
// which is what the old "controls unavailable" note existed to explain.
// Result of the last Auto-calibrate press, shown next to the button. Session-only: it describes an
// action, not a state, so it must not come back on the next launch pretending to be current.
static std::string g_np_msg;

// The raw-source readout. This is the instrument the subsystem shipped without, and its absence is
// what made a mis-set multiplier and a dead depth pipe look identical -- both are a flat frame, and
// nothing on screen said which. It reports texDepthRaw BEFORE any of the controls below touch it,
// so the two failures finally read differently.
static void draw_native_depth_probe(RecordPass& p) {
    ImGui::TextDisabled("Depth source");
    ImGui::SameLine();
    help_marker("What is in ReShade's depth buffer right now, measured before any setting below is "
                "applied.\n\n"
                "Read it first: if the source is empty, no combination of these controls will "
                "produce an image, and if it isn't, the controls are the only thing left to be "
                "wrong. Sampled on a 256x256 grid a few times a second while this panel is open.");

    switch (g_np_state) {
        case NPS_NO_API:
            ImGui::TextColored(MPR_WARN, "Not available on this graphics API.");
            ImGui::TextDisabled("The depth and normals passes are D3D11-only (see Capture backend differences).");
            return;
        case NPS_NO_SHADER:
            ImGui::TextColored(MPR_WARN, "Probe unavailable - its compute shader didn't load.");
            ImGui::TextDisabled("The depth and normals passes themselves may still be fine; only this readout is gone.");
            return;
        case NPS_NO_SOURCE:
            // The single most valuable line in this panel: it names the actual cause of a black
            // depth pass, which until now only ever reached ReShade.log.
            ImGui::TextColored(MPR_ERR, "No depth is reaching the addon.");
            ImGui::TextWrapped("MultipassRecorder.fx isn't loaded, or its 'Multipass Recorder Setup' technique is "
                               "switched off in the ReShade shader list. Nothing below can fix that.");
            return;
        case NPS_IDLE:
        case NPS_WAITING:
            ImGui::TextDisabled("Sampling...");
            return;
        case NPS_OK:
            break;
    }

    ImGui::TextDisabled("texDepthRaw   %ux%u   sampled %u px", g_np.w, g_np.h, NP_SAMPLES);

    // A source that is one value everywhere carries no depth at all. Say so plainly rather than
    // printing a range of zero and leaving the reader to notice.
    if (g_np.max_v - g_np.min_v < 1e-6f) {
        ImGui::TextColored(MPR_ERR, "Every sampled pixel is %.4f - the buffer carries no depth.", g_np.min_v);
        ImGui::TextWrapped("ReShade is handing us a buffer, but not the scene's depth. Pick a different one in "
                           "ReShade's Add-ons tab -> Generic Depth, or try its 'Copy depth buffer before clear "
                           "operations' option - some engines clear depth before the frame ends.");
        return;
    }

    ImGui::TextColored(MPR_OK_DIM, "range %.4f .. %.4f", g_np.min_v, g_np.max_v);
    ImGui::SameLine();
    ImGui::TextDisabled("mean %.4f", g_np.mean);
    ImGui::TextDisabled("at 0.0: %.1f%%   at 1.0: %.1f%%", g_np.frac_lo * 100.0f, g_np.frac_hi * 100.0f);
    if (g_np.frac_oob > 0.001f) {
        ImGui::SameLine();
        ImGui::TextColored(MPR_INFO, "  above 1.0: %.1f%%", g_np.frac_oob * 100.0f);
        ImGui::SameLine();
        help_marker("This buffer isn't in the 0-1 range a hardware depth buffer uses, so it is carrying "
                    "distance in some other unit. The multiplier is what rescales it - Auto-calibrate "
                    "sizes it from the measured maximum.");
    }

    // An INFERENCE, and labelled as one: read off the distribution, not off anything the format
    // guarantees. Shared with Auto-calibrate (native_probe_looks_reversed) so this line can never
    // describe a different conclusion than the button would reach.
    const bool looks_rev = native_probe_looks_reversed();
    ImGui::TextDisabled("distribution suggests %s", looks_rev ? "REVERSED (near = high)" : "conventional (near = low)");
    ImGui::SameLine();
    help_marker("Inferred from where the pixels pile up, not from anything the buffer states. Most of a "
                "scene sits near the far plane, so a conventional buffer crowds 1.0 and a reverse-Z one "
                "crowds 0.0.\n\n"
                "It can read the wrong way round on a frame with no distance in it - point the camera at "
                "a normal view before trusting it.");
    if (looks_rev != p.native_reversed)
        ImGui::TextColored(MPR_WARN, "...but 'Reversed' is currently %s.", p.native_reversed ? "on" : "off");
}

// One line of the same measurement, for the Record tab. The full readout lives behind the depth
// controls -- which are in the Viewfinder, and someone whose depth tile is black has no particular
// reason to suspect the Viewfinder is where the explanation is kept. This puts the single fact that
// decides whether any of the other controls can matter on the first screen the panel opens to, and
// issues its own probe request so it never depends on the Viewfinder having been opened at all.
static void draw_native_source_line() {
    bool want = false;
    for (const auto& q : g_passes)
        if ((q.source == SRC_NATIVE_DEPTH || q.source == SRC_NATIVE_NORMALS) && q.enabled) { want = true; break; }
    if (!want) return;   // nothing here depends on depth; don't spend a line on it
    native_probe_request();

    switch (g_np_state) {
        case NPS_NO_SOURCE:
            ImGui::TextColored(MPR_ERR, "Depth source: none.");
            ImGui::SameLine();
            ImGui::TextWrapped("'Multipass Recorder Setup' is off in the ReShade shader list, or the .fx isn't "
                               "installed - depth and normals will record black.");
            break;
        case NPS_OK:
            if (g_np.max_v - g_np.min_v < 1e-6f) {
                ImGui::TextColored(MPR_ERR, "Depth source: flat (every pixel %.4f).", g_np.min_v);
                ImGui::SameLine();
                help_marker("ReShade is giving us a buffer, but it holds no scene depth. Choose a different one "
                            "in ReShade's Add-ons tab -> Generic Depth.");
            } else {
                ImGui::TextColored(MPR_OK_DIM, "Depth source: ok");
                ImGui::SameLine();
                ImGui::TextDisabled("(range %.3f .. %.3f)  - full readout and calibration are in the Viewfinder.",
                                    g_np.min_v, g_np.max_v);
            }
            break;
        default:
            break;   // idle / waiting / unavailable: say nothing rather than flicker a non-answer
    }
}

static void draw_native_pass_controls(effect_runtime* rt, RecordPass& p) {
    const bool is_depth = (p.source == SRC_NATIVE_DEPTH);
    ImGui::Separator();
    bool ch = false;

    // These controls being on screen IS the request to probe. Nothing else turns it on, so a closed
    // panel costs one comparison per frame in the hook (mpr_native.inc).
    native_probe_request();
    draw_native_depth_probe(p);
    section_gap();

    // Derive the two settings that decide whether there is an image at all from what the probe just
    // measured, rather than making the user find them by feel. Offered for the normals pass too:
    // it re-linearizes depth itself, so it carries its own copy of exactly these fields.
    if (ImGui::SmallButton("Auto-calibrate")) {
        switch (native_auto_calibrate(p)) {
            case NA_OK:
                g_np_msg = "Set from the current frame - check the image, then adjust the far plane to taste.";
                ch = true;
                break;
            case NA_FLAT:
                g_np_msg = "Nothing to calibrate: the source is a single flat value.";
                break;
            case NA_NO_DATA:
                g_np_msg = "No measurement yet - leave this panel open a moment and try again.";
                break;
        }
    }
    ImGui::SameLine();
    help_marker("Sets 'Reversed' and the multiplier from the depth buffer as it is right now.\n\n"
                "A STARTING POINT, not a verdict: it reads the frame currently on screen, so give it a "
                "representative view (some near geometry, some distance) rather than a menu or a wall.\n\n"
                "It deliberately leaves the far plane alone - that is a choice about which distance "
                "should map to white, not something the buffer can tell you.");

    // Getting these wrong makes the image uniformly black or white, which looks exactly like "the
    // pass is broken" -- and once you are hunting blind it is easy to end up somewhere no single
    // slider walks back from (a tiny multiplier plus reverse-Z pins every pixel at 1.0). One click
    // back to knowingly-sane values is worth more than any amount of explanatory text.
    ImGui::SameLine();
    if (ImGui::SmallButton(is_depth ? "Reset depth" : "Reset normals")) {
        g_np_msg.clear();
        if (is_depth) {
            p.native_far_plane = 1000.0f; p.native_depth_mul = 1.0f;
            p.native_reversed = false; p.native_upside_down = false;
            p.native_log_curve = false; p.native_reverse_luma = false;
            p.native_linear_output = false;
        } else {
            p.native_fov = 60.0f; p.native_normal_mul = 1.0f; p.native_use_fov = true;
            p.native_flip_x = p.native_flip_y = p.native_flip_z = false;
            p.native_far_plane = 1000.0f; p.native_reversed = false; p.native_upside_down = false;
        }
        ch = true;
    }
    ImGui::SameLine();
    help_marker("Back to the shipped defaults: far plane 1000, multiplier 1.0, no reverse-Z, no curve.\n\n"
                "Start here on a new game, then raise or lower the far plane until the depth you care "
                "about fills the range. If the whole frame is one flat value, the depth buffer itself "
                "may not be reaching ReShade - check the Depth source readout above.");
    if (!g_np_msg.empty()) ImGui::TextWrapped("%s", g_np_msg.c_str());
    // The subsystem's own reactive report. It was being written and never read: the message that
    // names a failed target allocation or a missing source only ever reached ReShade.log, which is
    // the one place you cannot look at while the game is running.
    if (!g_native_report.empty()) ImGui::TextColored(MPR_WARN, "%s", g_native_report.c_str());

    if (is_depth) {
        ImGui::TextDisabled("Depth range");
        ImGui::SetNextItemWidth(200.0f);
        // LOGARITHMIC, both of these. Linear, the multiplier's useful values (order 0.1) all landed
        // inside the first pixel of a 0.001..1000 track -- reachable in principle, unusable in fact.
        ch |= ImGui::SliderFloat("Far plane", &p.native_far_plane, NATIVE_FAR_MIN, NATIVE_FAR_MAX, "%.0f",
                                 ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine(); help_marker("Distance that maps to white. Set it to roughly the deepest thing "
                                       "you care about: too large and everything near the camera is crushed "
                                       "into the first few values.");
        ImGui::SetNextItemWidth(200.0f);
        ch |= ImGui::SliderFloat("Multiplier", &p.native_depth_mul, NATIVE_DMUL_MIN, NATIVE_DMUL_MAX, "%.4f",
                                 ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine(); help_marker("Rescales the raw buffer before everything else. Leave it at 1.0 for a "
                                       "normal 0-1 depth buffer; a buffer carrying distance in other units needs "
                                       "it, and Auto-calibrate sizes it from the measured range.");
        // Ctrl+click types a value straight in, and ImGui only clamps that when asked -- which it is
        // asked differently across the ImGui versions this addon has to build against. Clamp here
        // instead, so an out-of-range entry can't survive to the load-time clamp and get reset.
        p.native_far_plane = (std::max)(NATIVE_FAR_MIN,  (std::min)(NATIVE_FAR_MAX,  p.native_far_plane));
        p.native_depth_mul = (std::max)(NATIVE_DMUL_MIN, (std::min)(NATIVE_DMUL_MAX, p.native_depth_mul));
        ch |= ImGui::Checkbox("Reversed (reverse-Z)", &p.native_reversed);
        ImGui::SameLine();
        ch |= ImGui::Checkbox("Flip vertically", &p.native_upside_down);
        ch |= ImGui::Checkbox("Logarithmic", &p.native_log_curve);
        ImGui::SameLine();
        ch |= ImGui::Checkbox("Invert", &p.native_reverse_luma);

        section_gap();
        ch |= ImGui::Checkbox("Scene-linear output", &p.native_linear_output);
        ImGui::SameLine();
        help_marker("Write true distance in world units instead of a 0-1 remap - nothing baked in, "
                    "the same stance the EXR stills take with exposure.\n\n"
                    "For STILLS (EXR carries float). Video is 0-1 fixed point, so anything past 1.0 "
                    "clips to white there.");
        if (p.native_linear_output)
            ImGui::TextColored(MPR_WARN, "Video will clip: scene-linear exceeds the 0-1 the encoder writes. Use stills.");
    } else {
        ImGui::TextDisabled("Normal calibration");
        ch |= ImGui::Checkbox("Use FOV slider", &p.native_use_fov);
        ImGui::SameLine(); help_marker("Derives the strength from vertical FOV, which is what you want if "
                                       "you know the game's FOV. Off, the manual multiplier is used instead.");
        if (p.native_use_fov) {
            ImGui::SetNextItemWidth(200.0f);
            ch |= ImGui::SliderFloat("Vertical FOV", &p.native_fov, NATIVE_FOV_MIN, NATIVE_FOV_MAX, "%.1f");
            p.native_fov = (std::max)(NATIVE_FOV_MIN, (std::min)(NATIVE_FOV_MAX, p.native_fov));
        } else {
            ImGui::SetNextItemWidth(200.0f);
            // Logarithmic and floored at 0.0001 for the same reason as the depth multiplier, plus a
            // specific one: the old .fx calibration for at least one game was 0.001, which the
            // previous 0.01 floor made impossible to enter at any slider position.
            ch |= ImGui::SliderFloat("Multiplier", &p.native_normal_mul, NATIVE_NMUL_MIN, NATIVE_NMUL_MAX, "%.4f",
                                     ImGuiSliderFlags_Logarithmic);
            p.native_normal_mul = (std::max)(NATIVE_NMUL_MIN, (std::min)(NATIVE_NMUL_MAX, p.native_normal_mul));
        }
        ch |= ImGui::Checkbox("Flip X", &p.native_flip_x); ImGui::SameLine();
        ch |= ImGui::Checkbox("Flip Y", &p.native_flip_y); ImGui::SameLine();
        ch |= ImGui::Checkbox("Flip Z", &p.native_flip_z);

        section_gap();
        ImGui::TextDisabled("Normals re-derive depth themselves, so this pass has its own");
        ImGui::TextDisabled("far plane / reverse-Z settings (below), independent of the depth pass.");
        ImGui::SetNextItemWidth(200.0f);
        ch |= ImGui::SliderFloat("Far plane##nrm", &p.native_far_plane, NATIVE_FAR_MIN, NATIVE_FAR_MAX, "%.0f",
                                 ImGuiSliderFlags_Logarithmic);
        p.native_far_plane = (std::max)(NATIVE_FAR_MIN, (std::min)(NATIVE_FAR_MAX, p.native_far_plane));
        ch |= ImGui::Checkbox("Reversed (reverse-Z)##nrm", &p.native_reversed);
        ImGui::SameLine();
        ch |= ImGui::Checkbox("Flip vertically##nrm", &p.native_upside_down);
        // These two feed linearize(), which normals_cs calls per tap -- so they have always applied
        // to this pass, they just had no control here. The old .fx exposed them as bDepthLogarithmic
        // / bDepthReverseLuma and a shipped preset set both, which means a migrated pass could carry
        // them on with nothing on screen accounting for the result.
        ch |= ImGui::Checkbox("Logarithmic##nrm", &p.native_log_curve);
        ImGui::SameLine();
        ch |= ImGui::Checkbox("Invert##nrm", &p.native_reverse_luma);
        ImGui::SameLine();
        help_marker("Applied while the normals pass re-derives depth for each of its taps, exactly as the "
                    "depth pass applies them. Off is right for almost every game.");
    }

    if (ch) save_cfg(rt);
}

// Router for whatever the Viewfinder currently has selected.
static void draw_fx_pass_controls(effect_runtime* rt, RecordPass* p) {
    if (!p) return;
    if (p->source == SRC_FX_LAYER) {
        ImGui::Separator();
        ImGui::TextDisabled("Effect layer (%s)", p->fx_effect[0] ? p->fx_effect : "unknown shader");
        draw_fx_pass_controls_inline(rt, *p);
        return;
    }
    if (p->source == SRC_NATIVE_DEPTH || p->source == SRC_NATIVE_NORMALS)
        draw_native_pass_controls(rt, *p);
}

static void draw_buffer_preview(effect_runtime* rt)
{
    cfg_save_tick(rt); // ahead of every early-out: this is our only per-frame ImGui entry point
    if (!(g_preview_open && (g_preview_handle || g_preview_pass))) return;
    // Draw ONLY on frames where a ReShade overlay panel was actually drawn (menu open right now).
    // This is called from on_reshade_overlay, which fires with the menu CLOSED too -- without a gate
    // the preview would linger frozen over the game, unclosable (its close box gets no input with the
    // menu hidden). We key off g_overlay_frame (this-frame stamp) rather than g_settings_ttl's 3-frame
    // countdown for a docking reason: if the preview is docked into ReShade's dockspace, it MUST only
    // be submitted on frames where ReShade also submits that dockspace host (== menu open). During the
    // TTL's trailing frames after the menu closes, ReShade skips its dockspace, so an ImGui window that
    // is still submitted gets orphaned out of its dock node and saved as floating -- it "jumps out" of
    // the dock on the next Home toggle. g_preview_open persists, so it reappears docked on Home.
    if (g_overlay_frame != g_live_frame) return;
    device* dev = rt->get_device();
    // Resolve whichever source is active to a sampled view + label + dimensions.
    resource_view srv = { 0 };
    char plabel[128] = "";
    uint32_t pw = 0, ph = 0; format pf = format::unknown;
    if (g_preview_pass) {
        srv = pass_preview_srv(rt, *g_preview_pass);
        srv_dims(dev, srv, &pw, &ph, &pf);
        sprintf_s(plabel, "%s", pass_display_name(*g_preview_pass));
    } else {
        LiveRT* s = nullptr;
        resource_desc sdesc = {};
        int sorder = 0;
        // Same hold as the gallery: a buffer the engine only touches now and then must not blank
        // the Viewfinder every time it skips a frame. Selected — and everything we display read
        // out — under g_live_mutex, for the same reason the Buffers tab snapshots there: the
        // game's worker threads mutate these slots on D3D12. The POINTER stays valid without the
        // lock (slots are blanked, never erased), and live_get_srv takes the lock itself, so it
        // must be called after this scope, never inside it.
        {
            std::lock_guard<std::mutex> lock(g_live_mutex);
            for (auto& r : g_live_rts)
                if (r.handle && r.handle == g_preview_handle && g_live_frame - r.last_frame <= LIVE_LIST_FRAMES) {
                    s = &r; sdesc = r.desc; sorder = r.order; break;
                }
        }
        if (s) {
            srv = live_get_srv(dev, *s, g_preview_handle); // re-checked under the lock: the slot can be reused between the scan and here
            pw = sdesc.texture.width; ph = sdesc.texture.height; pf = sdesc.texture.format;
            sprintf_s(plabel, "o%02d", sorder);
        }
    }
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse; // wheel = zoom, never scroll
    if (g_preview_fs) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ds, ImGuiCond_Always);
        // The settings panel stays open behind this (it's what the menu-open gate above requires),
        // and by default sits in the top-left quadrant this window now fully covers. Without forcing
        // focus every frame, whichever window the user clicked most recently wins ImGui's hit test
        // for that whole region -- if that's the settings panel, drags/wheel over it never reach the
        // image below, and pan/zoom silently do nothing there. Force this window topmost every frame
        // instead: everything needed while full-screen (pass picker, Full screen toggle) lives in
        // this window's own toolbar, so the settings panel underneath doesn't need to be reachable.
        ImGui::SetNextWindowFocus();
        flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
    } else {
        // Anchored beside the settings panel (layout_preview_rect) instead of the ImGui default
        // cascade over it -- on the same schedule as the panel, so both land together.
        const LayoutRect r = layout_preview_rect();
        const ImGuiCond cond = layout_cond();
        ImGui::SetNextWindowPos(ImVec2(r.x, r.y), cond);
        ImGui::SetNextWindowSize(ImVec2(r.w, r.h), cond);
    }
    // HDR-format sources get the WYSIWYG exposure view + scope. The processed frame itself can
    // come from either backend (pv_display_srv); on the GPU backend LDR sources are processed
    // too (passthrough + channel isolate), otherwise LDR falls back to the raw SRV.
    const bool hdr_fmt = preview_fmt_hdr(pf);
    uint32_t dw = 0, dh = 0;
    const resource_view proc = pv_display_srv(&dw, &dh);
    MprThemeScope theme; // must be constructed BEFORE Begin() -- Begin() paints WindowBg/Border/
                         // rounding/scrollbar itself, using whatever style is active at that call
    if (ImGui::Begin("Viewfinder", &g_preview_open, flags)) {
        // ---- pass picker: cycle depth/normals/world/... without leaving the window ----
        draw_preview_pass_picker(rt);
        // ---- toolbar: view controls left, source identity right ----
        if (ImGui::Checkbox("Full screen", &g_preview_fs)) save_addon_config(rt);
        // Debug View only supports a configured pass (not a raw engine buffer) -- it needs a
        // pass identity for the on-screen label and for pass_preview_srv to re-resolve every frame.
        if (g_preview_pass) {
            ImGui::SameLine();
            ImGui::Checkbox("Debug View (over game)", &g_debug_view); // session-only -- not saved
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", MPR_TT_DEBUG_VIEW);
        }
        if (pv_gpu_active()) { // channel isolation is computed in the GPU shader; hidden on the CPU path
            ImGui::SameLine(0, 16);
            ImGui::TextUnformatted("Ch"); ImGui::SameLine();
            ImGui::SetNextItemWidth(74.0f);
            ImGui::Combo("##pv_channel", &g_pv_channel, PV_CHANNEL_LABELS, IM_ARRAYSIZE(PV_CHANNEL_LABELS));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Isolate a channel in the preview image (A = alpha as grayscale, Luma = BT.709 luminance).\n"
                "View-only and session-only - the recording is never affected.");
        }
        ImGui::SameLine();
        if (pw) ImGui::TextDisabled("%s  %ux%u  %s", plabel, pw, ph, probe_format_name(pf));
        else    ImGui::TextDisabled("%s  (not bound this frame)", plabel);
        // Live sampling is suspended while recording -- see hires_preview_wanted in
        // on_reshade_present -- so a processed frame below is frozen, not live. Raw-SRV
        // previews cost nothing extra and stay live throughout.
        if (proc.handle && g_recording)
            ImGui::TextColored(MPR_WARN, "Recording -- live sampling paused (last frame shown, resumes on stop)");

        if (proc.handle) {
            // Exposure controls only where exposure is real (an encode-HDR pass whose enc_exposure
            // actually bakes into the file); other HDR sources get the plain scope at identity,
            // same as raw engine buffers.
            RecordPass* const enc_pass = (g_preview_pass && pass_is_hdr_src(*g_preview_pass)) ? g_preview_pass : nullptr;
            // The waveform is a GPU-path feature: available whenever that path is live. Its toggle
            // now lives in the Scope options popup, so an off waveform reserves nothing at all.
            const bool wf_avail = pv_gpu_active();
            // Room below the image, derived from the rows that actually follow it (no magic numbers).
            const float panel_h = preview_panel_reserve(hdr_fmt, enc_pass, wf_avail);
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            pv_draw_image(proc, (float)dw, (float)dh, avail.y - panel_h);
            // One options control for every view-mode toggle of both instruments; its label states
            // all of them, so no mode is invisible. The instruments themselves stay always-on below.
            draw_scope_options(rt, enc_pass, hdr_fmt, wf_avail);
            if (hdr_fmt) draw_exposure_scope(rt, enc_pass);
            if (wf_avail) draw_waveform_scope(rt);
        } else if (srv.handle && pw && !srv_is_mono(dev, srv)) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            pv_draw_image(srv, (float)pw, (float)ph, avail.y - ImGui::GetFontSize() * 2.6f);
            if (hdr_fmt) ImGui::TextDisabled("Sampling HDR buffer for exposure preview...");
            else         ImGui::TextDisabled("Raw buffer contents (no processed view for this format/API).");
        } else if (srv.handle && pw) {
            // Mono source with no processed frame. This window is where the thumbnails and the
            // glance strip send you for "the real view", so it must not be the one place that
            // paints it red — a 1-channel SRV drawn as RGB is float4(v,0,0,1). Only the compute
            // processor can splat R across RGB, so name what is missing instead of faking it.
            ImGui::TextColored(MPR_WARN, "Single channel (%s) - needs the processed view to show as "
                                         "grayscale, and it isn't available here.", probe_format_name(pf));
            ImGui::TextDisabled("The GPU preview processor is D3D11-only; on other APIs, and if "
                                "d3dcompiler_47 failed to load, this pass can still be RECORDED "
                                "normally - only the on-screen view is unavailable.");
        } else {
            ImGui::TextDisabled("Buffer not bound this frame, or not sampleable as an SRV.");
        }
        // Per-pass controls for whatever is selected: depth range / normal calibration for the
        // addon-computed passes, the debug-view picker for an effect layer.
        if (g_preview_pass) draw_fx_pass_controls(rt, g_preview_pass);
    }
    ImGui::End();
    if (!g_preview_open) { // closed -> clear source + one-shot view state
        g_preview_handle = 0; g_preview_pass = nullptr; g_debug_view = false;
        g_pv_zoom = 1.0f; g_pv_pan_x = g_pv_pan_y = 0.5f;
        save_addon_config(rt); // persist the closed state (keeps last-viewed pass for next open)
    }
}

// ---- Buffers tab: the live engine-buffer gallery ----
// Live view of every render target the game binds; any buffer can become a capture pass. Its own
// tab (not a section of the Record tab) so this tall, scroll-heavy gallery gets the full window
// height instead of shoving the recording cockpit off-screen.
static void draw_buffers_tab(effect_runtime* rt)
{
    device* dev = rt->get_device();
    ImGui::Spacing();

    // One-button auto-profiler: guess this game's HDR + normals buffers and save to its ini.
    if (ImGui::Button("Scan & Auto-Profile This Game")) g_scan_report = auto_profile_scan();
    ImGui::SameLine();
    help_marker("Open a 3D scene first (not a menu) - the list warms up over a couple of frames.\n\n"
                "Each row is one buffer the game renders this frame - both color render targets and\n"
                "compute-shader outputs (tagged 'compute (UAV)': motion vectors, SSAO/SSR/GI, bloom,\n"
                "auto-exposure). Each row is one line of chips - bind order, size, pixel format,\n"
                "and 'UAV' if a compute shader wrote it. The FORMAT chip's colour states only what\n"
                "the pixel format guarantees (never a guess at what the buffer is for) -\n"
                "  green = color you can grade (HDR float = push exposure freely; 10/16-bit = clean)\n"
                "  amber = 8-bit color - limited, bands if pushed hard\n"
                "  blue  = a depth/stencil format (a distance buffer, not a color image)\n"
                "  grey  = 1- or 2-channel - can't be a full-color image\n"
                "The THUMBNAIL tells you what it actually is; hover any chip for the rest (what the\n"
                "eye can't see: HDR vs clamped, bit depth, byte cost, binds/draws). Click a\n"
                "thumbnail to preview; '+ Add as pass' records that buffer as its own capture pass,\n"
                "'- Remove pass' takes it back out once added.\n\n"
                "'Scan & Auto-Profile' guesses this game's main HDR + normals buffers and saves them "
                "to its ReShade ini.");
    if (!g_scan_report.empty()) ImGui::TextWrapped("%s", g_scan_report.c_str());

    // Row set and row ORDER are both deliberately sticky (see the stability comment in
    // mpr_state.inc): buffers linger for LIVE_LIST_FRAMES after their last bind instead of
    // popping out of the list, and rows sort on the stable creation sequence rather than on
    // this frame's bind order. Sorting on bind order made the whole table reshuffle whenever
    // the engine skipped a conditional pass, which moved the row out from under the cursor
    // mid-click.
    // Snapshot the row set under the lock so `handle`/`last_frame` are read coherently while the
    // game's worker threads are registering binds. The POINTERS stay valid without the lock (the
    // vector is reserve()d to its 64-slot cap and slots are blanked, never erased) — only the
    // selection needs protecting, so the lock is released before any ImGui work.
    std::vector<LiveRT*> slots;
    {
        std::lock_guard<std::mutex> lock(g_live_mutex);
        for (auto& s : g_live_rts)
            if (s.handle && g_live_frame - s.last_frame <= LIVE_LIST_FRAMES) slots.push_back(&s);
    }
    std::sort(slots.begin(), slots.end(), [](LiveRT* a, LiveRT* b) { return a->seq < b->seq; });

    if (slots.empty())
        ImGui::TextDisabled("No RTs yet - open a scene (enumeration warms up over a couple of frames).");
    // Three columns, not four: the old "buffer" column existed to stack o## / idle / size above the
    // role/format/UAV lines in "info". Both collapse into one chip line, so there is one body
    // column now and it gets all the slack.
    else if (ImGui::BeginTable("rtb", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                         ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("thumb", ImGuiTableColumnFlags_WidthFixed, 126.0f);
        ImGui::TableSetupColumn("info",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("act",   ImGuiTableColumnFlags_WidthFixed, 200.0f);
        for (LiveRT* s : slots) {
            ImGui::PushID((int)(s->handle & 0x7fffffff));
            // Held over from a recent frame rather than bound right now: the row stays put (so the
            // table doesn't jump) but is dimmed and tagged, because its thumbnail and its
            // binds/draws counts are last-known values, not this frame's.
            const bool idle = (g_live_frame - s->last_frame) > LIVE_FRESH_FRAMES;
            if (!idle) s->dup_pass = find_promoted_pass_for_handle(s->handle);
            RecordPass* dup = s->dup_pass;
            // The memo outlives the pass it names (the user can remove it while the row is idle),
            // so re-check what it points at rather than trusting the pointer.
            if (dup && !(dup->promoted && dup->source == SRC_ENGINE_RT)) { dup = nullptr; s->dup_pass = nullptr; }
            ImGui::TableNextRow();
            if (dup) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(MPR_ROW_ADDED));
            if (idle) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
            ImGui::TableSetColumnIndex(0);
            resource_view srv = live_get_srv(dev, *s, s->handle); // no thumbnail beats another buffer's thumbnail if the slot was reused
            if (srv.handle) {
                // Gallery thumbnails are always the raw SRV (no processor runs over 64 slots), so a
                // 1-channel mask/depth buffer would be a solid red rectangle. The chip row below
                // already states resolution, format and the "1-channel" role, which identifies the
                // buffer better than a red silhouette would.
                const bool mono = srv_is_mono(dev, srv);
                const bool hit  = mono ? mono_thumb_placeholder("##thumb", ImVec2(120, 68))
                                       : (ImGui::Image((ImTextureID)srv.handle, ImVec2(120, 68)), ImGui::IsItemClicked());
                // Clear g_preview_pass too: it has precedence in the preview window, so without
                // this you could never switch FROM a pass preview back to a raw engine buffer.
                // Clear g_debug_view too: Debug View only supports configured passes, not raw
                // engine buffers, and a plain "preview" click should never carry it over.
                if (hit) { g_preview_handle = s->handle; g_preview_pass = nullptr; g_preview_open = true; g_debug_view = false; } // click thumb = preview
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mono ? MPR_TT_MONO_RAW : "Click to preview");
            } else {
                ImGui::Dummy(ImVec2(120, 68));
            }
            // ---- The row body is ONE line of chips: o03 · 1920x1080 · rgba16f · UAV ----
            // It used to be four to six stacked text lines per buffer, and eight buffers made a
            // wall of it. Nothing was dropped: every one of those lines is still here, on the hover
            // of the chip it belongs to (details-on-hover is the pattern the rest of this row
            // already used for byte cost and binds/draws). The format chip is TINTED BY ROLE, so
            // "can I grade this?" is answered by the colour of something you were already reading
            // instead of by a line of its own.
            ImGui::TableSetColumnIndex(1);
            const format fmt = s->desc.texture.format;
            char cb[96];
            _snprintf_s(cb, sizeof(cb), _TRUNCATE, "o%02d", s->order);
            mpr_chip(MPR_TEXT_AXIS, cb);
            {
                const int nth = s->order + 1; // o00 = the first target bound
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Bind order: the %d%s distinct render target the game binds each frame\n"
                    "(o00 = first). Use it to recognize a buffer across frames.\n\n"
                    "Held steady on purpose - engines skip passes some frames, which would\n"
                    "otherwise renumber this every frame. It only moves once the new position\n"
                    "has held for %u frames.",
                    nth, (nth % 10 == 1 && nth % 100 != 11) ? "st" : (nth % 10 == 2 && nth % 100 != 12) ? "nd"
                       : (nth % 10 == 3 && nth % 100 != 13) ? "rd" : "th", (unsigned)LIVE_ORDER_HOLD);
            }
            if (idle) {
                mpr_chip(mpr_u32(MPR_DIM), "idle", true);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "The game hasn't drawn into this buffer for %u frames, so the thumbnail and\n"
                    "the counts on these chips are the last values seen. It's kept listed for about\n"
                    "half a second so buffers the engine only touches now and then (shadow cascades,\n"
                    "probe or reflection updates) don't flicker in and out of the list.\n\n"
                    "Adding it as a pass still works - it records whenever the game draws into it.",
                    (unsigned)(g_live_frame - s->last_frame));
            }
            // Dimensions only, deliberately: the old "full res / half res / low res" class was
            // derived from the widest color buffer in the registry, which mislabels every
            // full-frame buffer in a game that binds something wider (BO6 has 4224-wide atlas
            // targets, so every 1920 G-buffer read "half res"). The thumbnail already shows
            // relative scale, and the exact numbers are right here. Byte cost stays on the hover.
            _snprintf_s(cb, sizeof(cb), _TRUNCATE, "%ux%u", s->desc.texture.width, s->desc.texture.height);
            mpr_chip(MPR_TEXT_AXIS, cb, true);
            if (uint32_t bpp = format_bpp(fmt); bpp && ImGui::IsItemHovered())
                ImGui::SetTooltip("%.1f MB/frame uncompressed.",
                                  (double)s->desc.texture.width * s->desc.texture.height * bpp / (1024.0 * 1024.0));
            // The format, in shorthand and tinted by role. The long role label, what it's good for,
            // the exact format name and the engine internals all live on this one hover.
            RoleView rv = role_view(format_role(fmt));
            if (const char* shortn = format_short_name(fmt); shortn[0]) strncpy_s(cb, shortn, _TRUNCATE);
            else _snprintf_s(cb, sizeof(cb), _TRUNCATE, "fmt %u", (unsigned)fmt);
            mpr_chip(mpr_u32(rv.col), cb, true);
            if (ImGui::IsItemHovered()) {
                const char* human = format_human(fmt);
                ImGui::SetTooltip("%s  -  %s\n%s%s%s%s\n"
                                  "Engine detail: DXGI fmt %u  -  %u binds, %u draws %s.\n"
                                  "draws = draw calls while bound (high = the main scene pass, low = a helper).",
                                  rv.label, probe_format_name(fmt),
                                  rv.use, rv.use[0] ? "\n" : "", human, human[0] ? "\n" : "",
                                  (unsigned)fmt, s->binds, s->draws,
                                  idle ? "on the last frame it was drawn" : "this frame");
            }
            if (s->bound_as & LIVE_UAV) {
                const bool both = (s->bound_as & LIVE_RTV) != 0;
                mpr_chip(mpr_u32(MPR_INFO), both ? "RT + UAV" : "UAV", true);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Written by a compute shader (unordered-access view)%s. Motion vectors,\n"
                    "SSAO/SSR/GI, bloom and auto-exposure are commonly compute outputs that never\n"
                    "appear as a color render target - this is the buffer class the RT list used to miss.",
                    both ? ",\nand also bound as a color render target" : "");
            }
            if (dup) {
                ImGui::TextColored(MPR_WARN, "Already added as '%s'", dup->name);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "This buffer is already captured by custom pass '%s' (%s).",
                    dup->name, dup->label);
            }
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("Preview")) { g_preview_handle = s->handle; g_preview_pass = nullptr; g_preview_open = true; g_debug_view = false; }
            ImGui::SameLine();
            if (dup) {
                if (ImGui::SmallButton("- Remove pass")) remove_custom_pass(rt, *dup);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Stops and removes custom pass '%s', freeing its slot.", dup->name);
            } else {
                // A pass's fingerprint is an index into the buffers bound THIS frame, so it can
                // only be taken while the game is drawing into this one (promote_rt refuses
                // otherwise). Say so up front instead of letting the click fail.
                ImGui::BeginDisabled(idle);
                if (ImGui::SmallButton("+ Add as pass")) promote_rt(rt, *s); // adds a NEW pass; never overrides
                ImGui::EndDisabled();
                if (idle && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(
                    "Can't be added while the game isn't drawing into it - a pass is pinned to a\n"
                    "buffer by its position among the targets bound each frame, which only exists\n"
                    "on frames it's actually bound.\n\n"
                    "Get the scene rendering this buffer again (the 'idle' tag clears) and add it then.");
            }
            if (idle) ImGui::PopStyleVar();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
}

// ---- Effects tab: the ReShade effect-layer gallery ----
// Every technique ReShade has loaded, and the ability to record one as its own pass. The addon
// renders the chosen technique into a target of its own (mpr_fxlayer.inc) rather than reading a
// buffer the game already produced -- so an effect's Debug View can be a recorded pass while the
// game view keeps its normal look.

// One candidate "Debug View" control found on an effect.
struct FxDbgVar {
    char   name[96]  = "";
    char   label[96] = "";
    char   items[512] = "";  // ui_items, '\0'-separated (ImGui::Combo eats this directly)
    bool   has_items = false;
    format base = format::unknown;
};

// Find the uniforms of `effect` that look like its debug/view-mode selector. Effect authors have no
// common name for it, so this matches on the one thing they do share: the word "debug" (or "view
// mode") in the variable name or its ui_label. Anything missed is reachable via the manual name box.
static int fx_collect_debug_vars(effect_runtime* rt, const char* effect, FxDbgVar* out, int max_out) {
    int n = 0;
    rt->enumerate_uniform_variables(effect && effect[0] ? effect : nullptr,
        [&](effect_runtime* r, effect_uniform_variable v) {
            if (n >= max_out) return;
            FxDbgVar d;
            r->get_uniform_variable_name(v, d.name);
            if (!r->get_annotation_string_from_uniform_variable(v, "ui_label", d.label)) d.label[0] = '\0';
            if (!(mpr_icontains(d.name, "debug")     || mpr_icontains(d.label, "debug") ||
                  mpr_icontains(d.name, "view_mode") || mpr_icontains(d.label, "view mode")))
                return;
            r->get_uniform_variable_type(v, &d.base);
            size_t isz = sizeof(d.items) - 2; // leave the trailing double-null Combo needs
            d.has_items = r->get_annotation_string_from_uniform_variable(v, "ui_items", d.items, &isz) && isz > 0;
            out[n++] = d;
        });
    return n;
}

// The two per-pass controls of an FX layer: WHICH debug output to record, and WHAT the technique
// composites onto. Shown inline on an added row (no expander -- two controls don't earn one).
static void draw_fx_pass_controls_inline(effect_runtime* rt, RecordPass& p) {
    FxDbgVar vars[8];
    const int n = fx_collect_debug_vars(rt, p.fx_effect, vars, 8);

    if (ImGui::Checkbox("Debug View", &p.fx_dbg_on)) {
        if (p.fx_dbg_on && !p.fx_dbg_var[0] && n > 0) strncpy_s(p.fx_dbg_var, vars[0].name, _TRUNCATE);
        save_addon_config(rt);
    }
    ImGui::SameLine();
    help_marker("Sets one of this effect's own settings -- normally its 'Debug' / 'Debug View' combo --\n"
                "for THIS PASS'S render only, then puts your value straight back. So the pass records\n"
                "the raw AO / GI / normals while the game view keeps its normal composited look.\n\n"
                "If nothing is listed, the effect names its debug control something unusual (type the\n"
                "variable name yourself), or ReShade's Performance Mode is on -- that compiles every\n"
                "uniform away into a constant, and nothing can drive it.");

    if (p.fx_dbg_on) {
        // Which control to drive. p.fx_dbg_var IS the selection, so an unrecognised name simply
        // falls through to the manual box -- no extra state to keep in sync.
        const FxDbgVar* sel = nullptr;
        for (int i = 0; i < n; ++i) if (strcmp(vars[i].name, p.fx_dbg_var) == 0) sel = &vars[i];

        ImGui::SetNextItemWidth(190.0f);
        const char* cur = sel ? (sel->label[0] ? sel->label : sel->name)
                              : (p.fx_dbg_var[0] ? p.fx_dbg_var : "(pick a control)");
        if (ImGui::BeginCombo("Control", cur)) {
            for (int i = 0; i < n; ++i) {
                const bool is_sel = (sel == &vars[i]);
                if (ImGui::Selectable(vars[i].label[0] ? vars[i].label : vars[i].name, is_sel)) {
                    strncpy_s(p.fx_dbg_var, vars[i].name, _TRUNCATE);
                    p.fx_dbg_val = 1;
                    save_addon_config(rt);
                }
                if (is_sel) ImGui::SetItemDefaultFocus();
            }
            if (ImGui::Selectable("Other (type a name)...", sel == nullptr)) {
                p.fx_dbg_var[0] = '\0';
                save_addon_config(rt);
            }
            ImGui::EndCombo();
        }
        if (n == 0) {
            ImGui::SameLine();
            ImGui::TextColored(MPR_WARN, "none auto-detected");
        }

        if (sel == nullptr) { // manual escape hatch
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::InputText("Variable", p.fx_dbg_var, sizeof(p.fx_dbg_var))) save_addon_config(rt);
        }

        // The value to write. A combo when the effect published ui_items (the usual case for a
        // debug selector), a plain toggle when it's a bool, a number otherwise.
        ImGui::SetNextItemWidth(190.0f);
        if (sel && sel->has_items) {
            if (ImGui::Combo("Shows", &p.fx_dbg_val, sel->items)) save_addon_config(rt);
        } else if (sel && sel->base == format::r32_typeless) {
            bool on = p.fx_dbg_val != 0;
            if (ImGui::Checkbox("Shows##bool", &on)) { p.fx_dbg_val = on ? 1 : 0; save_addon_config(rt); }
        } else {
            if (ImGui::DragInt("Shows", &p.fx_dbg_val, 0.1f, 0, 32)) save_cfg(rt);
        }
    }

    // Always available: the plate is what the technique composites onto, debug view or not.
    ImGui::SetNextItemWidth(190.0f);
    if (ImGui::Combo("Plate", &p.fx_seed, "Clean scene\0White\0Black\0")) save_addon_config(rt);
    ImGui::SameLine();
    help_marker("What the effect renders on top of.\n\n"
                "Clean scene - the game image before ANY ReShade effect ran. What the effect would\n"
                "  normally see, so the pass looks like 'the scene with just this effect applied'.\n"
                "White - a multiplicative effect (ambient occlusion) renders its own matte, straight\n"
                "  out, ready to composite.\n"
                "Black - an additive effect renders only what it adds.\n\n"
                "Most debug views overwrite the image entirely and ignore this.");
}

static void draw_effects_tab(effect_runtime* rt)
{
    ImGui::Spacing();

    ImGui::TextUnformatted("ReShade effects loaded in this game");
    ImGui::SameLine();
    help_marker("Every technique ReShade has loaded, grouped by shader file. '+ Add as pass' records\n"
                "one as its own capture pass.\n\n"
                "The addon does NOT screenshot the effect off the game view -- it runs the technique a\n"
                "second time into a buffer of its own. So the pass is unaffected by everything else in\n"
                "your chain, the game view is unaffected by the pass, and a technique switched OFF in\n"
                "the ReShade list can still be recorded (give it a frame or two to compile).\n\n"
                "This is what makes 'record the AO layer' work: set the pass's Debug View to the raw\n"
                "AO while the picture on screen stays a normal, fully-composited frame.");
    if (!g_fx_report.empty()) ImGui::TextWrapped("%s", g_fx_report.c_str());
    section_gap();

    // Snapshot the technique list. Handles are invalidated on any effect reload, so nothing here is
    // cached between frames -- rows carry names, and the pass stores names too.
    struct Row { char effect[128]; char tech[128]; bool on; };
    std::vector<Row> rows;
    rt->enumerate_techniques(nullptr, [&](effect_runtime* r, effect_technique t) {
        if (rows.size() >= 512) return;
        Row row = {};
        r->get_technique_name(t, row.tech);
        r->get_technique_effect_name(t, row.effect);
        row.on = r->get_technique_state(t);
        // Our own setup technique is plumbing for the depth/normals/world passes, not something you
        // would ever record as a layer.
        if (strcmp(row.effect, "MultipassRecorder.fx") == 0) return;
        rows.push_back(row);
    });

    if (rows.empty()) {
        ImGui::TextDisabled("No effects loaded - install some .fx shaders and press Reload in the ReShade Home menu.");
        return;
    }

    char cur_effect[128] = "";
    bool table_open = false;
    auto end_table = [&]() { if (table_open) { ImGui::EndTable(); table_open = false; } };

    for (size_t i = 0; i < rows.size(); ++i) {
        Row& r = rows[i];
        if (strcmp(r.effect, cur_effect) != 0) {   // new shader file -> its own headed group
            end_table();
            strncpy_s(cur_effect, r.effect, _TRUNCATE);
            ImGui::SeparatorText(cur_effect);
            ImGui::PushID(cur_effect);
            table_open = ImGui::BeginTable("fxt", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                                     ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_PadOuterX);
            ImGui::PopID();
            if (table_open) {
                ImGui::TableSetupColumn("thumb", ImGuiTableColumnFlags_WidthFixed, 126.0f);
                ImGui::TableSetupColumn("tech",  ImGuiTableColumnFlags_WidthFixed, 170.0f);
                ImGui::TableSetupColumn("info",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("act",   ImGuiTableColumnFlags_WidthFixed, 200.0f);
            }
        }
        if (!table_open) continue;

        RecordPass* pass = find_fx_pass(r.effect, r.tech);
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        if (pass) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(MPR_ROW_ADDED));

        ImGui::TableSetColumnIndex(0);
        if (pass && pass->fx_srv.handle) {
            ImGui::Image((ImTextureID)pass->fx_srv.handle, ImVec2(120, 68));
            if (ImGui::IsItemClicked()) { g_preview_pass = pass; g_preview_handle = 0; g_preview_open = true; save_addon_config(rt); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to preview");
        } else {
            ImGui::Dummy(ImVec2(120, 68));
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(r.tech);
        if (r.on) ImGui::TextColored(MPR_OK_DIM, "on in game view");
        else      ImGui::TextDisabled("off in game view");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Whether this technique is ticked in ReShade's own effect list.\n"
            "It has no bearing on recording it: an added pass renders the technique itself.");

        ImGui::TableSetColumnIndex(2);
        if (pass) {
            if (!pass->fx_fmt_ok)
                ImGui::TextColored(MPR_ERR, "Can't record: back buffer format %s isn't supported.",
                                   probe_format_name(pass->fx_fmt));
            else if (pass->fx_srv.handle == 0)
                ImGui::TextColored(MPR_WARN, "Warming up - the effect is still compiling.");
            draw_fx_pass_controls_inline(rt, *pass);
        } else {
            ImGui::TextDisabled("Not recorded.");
        }

        ImGui::TableSetColumnIndex(3);
        if (pass) {
            if (ImGui::SmallButton("Preview")) { g_preview_pass = pass; g_preview_handle = 0; g_preview_open = true; save_addon_config(rt); }
            ImGui::SameLine();
            if (ImGui::SmallButton("- Remove pass")) { remove_custom_pass(rt, *pass); g_fx_report.clear(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stops and removes pass '%s', freeing its slot.", pass->name);
        } else if (ImGui::SmallButton("+ Add as pass")) {
            promote_fx_technique(rt, r.effect, r.tech);
        }
        ImGui::PopID();
    }
    end_table();
    ImGui::Spacing();
}

// ---- "At a glance" strip: every pass previewed at once, above the pass list ----------------
// The pass list tells you how each pass is CONFIGURED; this tells you what each one is actually
// seeing right now, without opening them one at a time. The failures it is built to catch are the
// ones that cost you a whole take and are invisible in a table: a buffer gone black, a promoted
// engine RT that drifted onto the wrong thing, a layer that stopped updating, an encoder that
// died, and a frame whose range no longer fits the capture window.
// Pixels come from mpr_tiles.inc (correctly exposed, round-robin one pass per present).

// Range bar: crush | kept | clip as one proportional strip, in the exposure scope's own colors,
// so the reading transfers straight to the big scope in the Viewfinder. Mostly green = the frame
// fits the capture window. Ends with a Dummy so the caller can hang a tooltip on it.
static void glance_range_bar(float w, float h, float crush, float keep, float clip, bool dim) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const int a = dim ? 110 : 255;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), mpr_alpha(MPR_SCOPE_BG, dim ? 150 : 255), 2.0f);
    const float sum = crush + keep + clip;
    if (sum > 0.0f) {
        const float seg[3] = { crush / sum, keep / sum, clip / sum };
        const ImU32 col[3] = { MPR_CRUSH, MPR_KEPT, MPR_CLIP };
        float x = p.x + 1.0f;
        const float inner = w - 2.0f;
        for (int k = 0; k < 3; ++k) {
            // Last segment runs to the inner edge: accumulated float error would otherwise leave a
            // sliver of background that reads as a fourth zone.
            const float x1 = (k == 2) ? p.x + w - 1.0f : x + inner * seg[k];
            if (x1 > x) dl->AddRectFilled(ImVec2(x, p.y + 1), ImVec2(x1, p.y + h - 1), mpr_alpha(col[k], a));
            x = x1;
        }
    }
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), mpr_alpha(MPR_OUTLINE, dim ? 120 : 255), 2.0f);
    ImGui::Dummy(ImVec2(w, h));
}

// Draw a label clipped to `w`, cutting from the right rather than wrapping (a wrapped pass name
// would make one tile taller than its neighbours and break the grid).
static void glance_text_fit(const char* s, float w, const ImVec4& col) {
    char buf[160];
    strncpy_s(buf, s, _TRUNCATE);   // never sprintf_s here: pass labels are user/engine supplied
    size_t len = strlen(buf);
    if (ImGui::CalcTextSize(buf).x > w) {
        while (len > 1 && ImGui::CalcTextSize(buf).x > w) buf[--len] = '\0';
        if (len > 0) buf[len - 1] = '.';
    }
    ImGui::TextColored(col, "%s", buf);
}

static void draw_glance_tile(effect_runtime* rt, int idx, float tw) {
    RecordPass& p = g_passes[idx];
    ImGui::PushID(idx);
    ImGui::BeginGroup();

    // Disabled passes are shown anyway, dimmed -- "I forgot to tick normals" is a finding too.
    // Style alpha rather than a per-Image tint: ImGui::Image lost its tint overload in 1.91.9b,
    // and this dims the tile's labels in the same stroke. Draw-list output (the range bar) does
    // NOT honour style alpha, so that one still takes the flag explicitly.
    const bool dim = !p.enabled;
    if (dim) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);

    // Prefer the processed tile; fall back to the pass's raw SRV so a tile is never simply absent
    // (no GPU path, unsupported format, or the round-robin hasn't reached this pass yet).
    resource_view raw = pass_preview_srv(rt, p);
    uint32_t pw = 0, ph = 0;
    resource_view tex = tile_display_srv(idx, &pw, &ph);
    const bool processed = tex.handle != 0;
    if (!processed) {
        tex = raw;
        uint32_t bw = 0, bh = 0; format bf = format::unknown;
        if (srv_dims(rt->get_device(), raw, &bw, &bh, &bf)) { pw = bw; ph = bh; }
    }

    // Aspect from the buffer itself, bounded so a square or ultra-wide data buffer can't blow the
    // row height out.
    float ar = (pw && ph) ? (float)ph / (float)pw : 9.0f / 16.0f;
    if (ar < 0.35f) ar = 0.35f; else if (ar > 1.4f) ar = 1.4f;
    const float ih = tw * ar;

    // Only the RAW fallback can paint mono red — a processed tile has already splatted R across RGB
    // (tile_capture_slot passes flag bit2), so this must not fire on one and hide a working tile.
    const bool mono_raw = !processed && srv_is_mono(rt->get_device(), tex);
    if (tex.handle) {
        const bool hit = mono_raw ? mono_thumb_placeholder("##tile", ImVec2(tw, ih))
                                  : (ImGui::Image((ImTextureID)tex.handle, ImVec2(tw, ih)), ImGui::IsItemClicked());
        if (hit) {
            g_preview_pass = &p; g_preview_handle = 0; g_preview_open = true; g_debug_view = false;
            save_addon_config(rt);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(360.0f);
            ImGui::TextUnformatted(pass_source_caption(p));
            if (mono_raw)
                ImGui::TextColored(MPR_WARN, "Single channel - not drawn here (raw, it would be solid "
                                             "red). Open it in the Viewfinder for the real view.");
            else if (!processed)
                ImGui::TextColored(MPR_WARN, "Raw buffer contents - NOT exposure-corrected. "
                                             "Open it in the Viewfinder for the real view.");
            ImGui::TextDisabled("Click to preview.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    } else {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(c, ImVec2(c.x + tw, c.y + ih), mpr_alpha(MPR_SCOPE_BG, 200), 3.0f);
        dl->AddRect(c, ImVec2(c.x + tw, c.y + ih), mpr_alpha(MPR_OUTLINE, 180), 3.0f);
        ImGui::Dummy(ImVec2(tw, ih));
    }

    glance_text_fit(pass_display_name(p), tw, ImGui::GetStyleColorVec4(ImGuiCol_Text));

    // Status: the failures that don't show up in pixels. Order is by urgency, and DEAD is only
    // asked about once the pass actually has a worker (same gate as the stats table).
    const bool live = p.enabled && p.worker.joinable();
    const bool dead = live && p.enc_failed.load(std::memory_order_acquire);
    if (dead) {
        ImGui::TextColored(MPR_ERR, "DEAD");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.enc_error);
    } else if (live) {
        ImGui::TextColored(p.dropped_frames.load() ? MPR_ERR : MPR_OK, "REC %s",
                           format_duration(p.frame_count.load(), g_fps).c_str());
    } else if (!raw.handle) {
        ImGui::TextColored(MPR_WARN, "not bound");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Nothing is bound to this pass this frame. For a universal pass the "
                              "Setup technique may be off; for an engine buffer the game may not be "
                              "drawing it in this scene.");
    } else {
        ImGui::TextDisabled("idle");
    }

    // Range bar. The icon slot is reserved whether or not it's used, so every tile's bar starts at
    // the same offset and the row can be scanned across.
    float cr = 0, kp = 0, cl = 0;
    const bool have  = tile_range_split(idx, p, &cr, &kp, &cl);
    const bool warn  = have && (cl > MPR_RANGE_WARN || cr > MPR_RANGE_WARN);
    const float fs   = ImGui::GetFontSize();
    const float barw = tw - (fs * 0.55f + 9.0f);
    if (warn) mpr_icon_warn();
    else { ImGui::Dummy(ImVec2(fs * 0.55f + 5.0f, fs)); ImGui::SameLine(0, 4); }
    if (have) {
        glance_range_bar(barw, fs * 0.62f, cr, kp, cl, dim);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%.1f%% crushed to black, %.1f%% captured, %.1f%% clipped white.\n"
                              "%s", cr * 100.0f, kp * 100.0f, cl * 100.0f,
                              warn ? "More than a tenth of the frame is outside the capture window -\n"
                                     "adjust Exposure on this pass, or pick a wider HDR curve."
                                   : "The frame fits this pass's capture window.");
    } else {
        ImGui::Dummy(ImVec2(barw, fs * 0.62f));
    }

    if (dim) ImGui::PopStyleVar();
    ImGui::EndGroup();
    ImGui::PopID();
}

static void draw_glance_strip(effect_runtime* rt) {
    ImGui::TextUnformatted("At a glance");
    ImGui::SameLine();
    help_marker("Every pass, live, at once - so a black buffer, a wrong promotion, a stalled layer "
                "or a dead encoder is visible without opening each pass in turn.\n\n"
                "Thumbnails carry each pass's own exposure and the same tonemapped view as the "
                "Viewfinder, so a black tile means a black buffer - not just an HDR buffer shown raw.\n\n"
                "The bar under an HDR pass is its range split: blue = crushed to black, green = "
                "captured, red = clipped white. HDR passes only - depth, normals and 8-bit buffers "
                "have no capture window to fall out of, so they get no bar.\n\n"
                "Disabled passes are shown dimmed. Click a tile to open it in the Viewfinder.");
    ImGui::SameLine();
    if (ImGui::SmallButton(g_show_glance ? "Hide" : "Show")) { g_show_glance = !g_show_glance; save_addon_config(rt); }
    if (!g_show_glance) return;

    int idxs[N_FIXED + N_CUSTOM], n = 0;
    for (int i = 0; i < N_FIXED + N_CUSTOM; ++i) if (glance_shows(i)) idxs[n++] = i;
    if (n == 0) { ImGui::TextDisabled("No passes."); return; }

    // Fit as many ~150px tiles as the panel column allows, at least two across so the strip still
    // reads as a comparison at the narrowest layout.
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float availW = ImGui::GetContentRegionAvail().x;
    int cols = (int)((availW + sp) / (150.0f + sp));
    if (cols < 2) cols = 2;
    if (cols > n) cols = n;
    const float tw = (availW - sp * (cols - 1)) / cols;

    for (int k = 0; k < n; ++k) {
        if (k % cols) ImGui::SameLine();
        draw_glance_tile(rt, idxs[k], tw);
    }
}

// ---- Record tab: game profile + the pass list (what gets captured, how it's encoded) ----
static void draw_record_tab(effect_runtime* rt)
{
    ImGui::Spacing();
    // Game profile status: which engine fingerprints are in effect (universal passes always work).
    bool known = (g_profile.exe_match[0] != '\0');
    ImGui::Text("Game profile:");
    ImGui::SameLine();
    ImGui::TextColored(known ? MPR_OK : MPR_WARN, "%s", g_profile.name);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", g_exe_name);
    if (!known) {
        ImGui::SameLine();
        help_marker("Unknown game: universal passes (depth/normals/world) still work. For engine buffers, "
                    "run 'Scan & Auto-Profile This Game' in the Buffers tab, or add buffers from it as passes.");
    }
    ImGui::Spacing();

    // What every pass is seeing right now, before the table that says how each is configured.
    draw_glance_strip(rt);
    // Is any depth arriving at all -- the one question the tiles above cannot answer, because a
    // black tile looks the same whether the source is empty or the settings are wrong.
    draw_native_source_line();
    // The native subsystem's reactive report, beside the tiles it explains. It was written from the
    // day mpr_native.inc landed and displayed nowhere, so the one message that names why a depth
    // tile is black ("the Setup technique is switched off") only ever reached ReShade.log.
    if (!g_native_report.empty()) ImGui::TextColored(MPR_WARN, "%s", g_native_report.c_str());
    section_gap();

    ImGui::Text("Passes");
    ImGui::SameLine();
    help_marker("A pass = one buffer -> one file. Check a pass to record it; click its row to edit encoding and preview it.");

    // Group labels, NOT collapsing headers. With three universal passes and a handful of custom
    // ones there is nothing here worth hiding behind a click, and a collapsed section can hide the
    // very pass whose tile above it is telling you something is wrong.
    static const int UNI_IDX[] = { 0, 1, 2 };  // keep in step with glance_shows (mpr_tiles.inc)
    ImGui::SeparatorText("UNIVERSAL  (any game)");
    draw_pass_section(rt, UNI_IDX, 3);

    // ENGINE section (built-in normals_native, index 3) temporarily hidden to simplify the UI.
    // The pass still exists in the backend (default-disabled) and the Buffers tab still exposes
    // every engine buffer via '+ Add as pass'. Restore by adding a SeparatorText("ENGINE ...")
    // over draw_pass_section(rt, {3}, 1) -- and drop GLANCE_HIDDEN_FIXED in mpr_tiles.inc so the
    // strip shows it too.

    section_gap();
    // Custom passes added from the Buffers or Effects tabs. Always visible (even when empty) so the
    // add-a-pass flow has a discoverable entry point in the pass list itself.
    ImGui::SeparatorText("CUSTOM  (added from Buffers / Effects)");
    {
        int cust_idx[N_CUSTOM], n_cust = 0;
        for (int i = 0; i < N_CUSTOM; ++i) if (g_passes[N_FIXED + i].promoted) cust_idx[n_cust++] = N_FIXED + i;
        if (n_cust > 0) draw_pass_section(rt, cust_idx, n_cust);
        if (ImGui::SmallButton("+ Add from Buffers")) g_ui_goto_tab = TAB_BUFFERS; // jump to the gallery
        ImGui::SameLine();
        if (ImGui::SmallButton("+ Add from Effects")) g_ui_goto_tab = TAB_EFFECTS;
        ImGui::SameLine();
        help_marker("Two things can become their own capture pass, and both land here:\n"
                    "  Buffers - a render target the GAME produced (G-buffer, motion vectors, ...).\n"
                    "  Effects - a ReShade shader technique, rendered separately by the addon so its\n"
                    "            debug output can be a pass without changing the game view.");
    }
}

// ---- Overlays tab: everything drawn over the game (composition guides + the recording HUD) ----
static void draw_overlays_tab(effect_runtime* rt)
{
    ImGui::Spacing();
    bool changed = false;

    // Everything on this tab draws through the same master mute as the HK_OVERLAYS hotkey, so say
    // so here rather than letting the checkboxes below look broken.
    if (g_osd_hidden) {
        ImGui::TextColored(MPR_WARN, "All on-screen overlays are hidden.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Show them")) g_osd_hidden = false;
        ImGui::SameLine();
        help_marker("Toggled by the \"Hide/show on-screen overlays\" hotkey (Setup tab). "
                    "Session-only - overlays are back on the next game launch.");
        section_gap();
    }

    ImGui::SeparatorText("Composition guides");
    changed |= ImGui::Checkbox("Crosshair",      &g_hud.show_crosshair);
    ImGui::SameLine();
    help_marker("Guides are drawn on screen only - they are never captured in recordings.");
    changed |= ImGui::Checkbox("Rule of Thirds", &g_hud.show_thirds);
    changed |= ImGui::Checkbox("Letterbox",      &g_hud.show_letterbox);
    if (g_hud.show_letterbox) {
        ImGui::SameLine(0, 8);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("##lbox", &g_hud.letterbox_idx, LETTERBOX_LABELS, IM_ARRAYSIZE(LETTERBOX_LABELS)))
            changed = true;
        ImGui::SetNextItemWidth(180.0f);
        changed |= ImGui::SliderFloat("Bar Opacity", &g_hud.lbox_alpha, 0.0f, 1.0f, "%.2f");
    }
    ImGui::Spacing();
    changed |= ImGui::ColorEdit4("Guide Color", g_hud.guide_color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

    section_gap();
    ImGui::SeparatorText("Recording HUD");
    changed |= ImGui::Checkbox("Recording badge (dot + REC + pass count)", &g_hud.show_badge);
    changed |= ImGui::Checkbox("Duration timer",    &g_hud.show_duration);
    changed |= ImGui::Checkbox("Stability graph",   &g_hud.show_graph);
    changed |= ImGui::Checkbox("Per-pass stats (fps / queue / drops)", &g_hud.show_stats);
    changed |= ImGui::Checkbox("Exposure meter (HDR passes)", &g_hud.show_ev_meter);
    ImGui::SameLine();
    help_marker("Live camera-style exposure meter while an HDR pass is enabled. Bars slide from\n"
                "crushing shadows (left) to clipping highlights (right); mostly green = the scene's\n"
                "range fits the capture window.\n"
                "Needs an enabled r11g11b10 HDR pass and the Viewfinder closed.");

    ImGui::SeparatorText("HUD colors");
    changed |= ImGui::ColorEdit3("Recording color",  g_hud.rec_color, ImGuiColorEditFlags_NoInputs);
    changed |= ImGui::ColorEdit3("Warning color",    g_hud.warn_color, ImGuiColorEditFlags_NoInputs);

    if (changed) save_hud_config(rt);
}

// ---- Setup tab: set-once configuration (output, encoding backend, maintenance) ----
static void draw_setup_tab(effect_runtime* rt)
{
    ImGui::Spacing();
    ImGui::SeparatorText("Output & encoding");
    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::InputInt("Capture FPS", &g_fps, 0)) {
        // Clamp BEFORE the write: typing 0 used to persist 0, and the next launch divided by it
        // (see MPR_FPS_MIN/MAX in mpr_config.inc) long before anyone reopened this tab.
        if (g_fps < MPR_FPS_MIN)      g_fps = MPR_FPS_MIN;
        else if (g_fps > MPR_FPS_MAX) g_fps = MPR_FPS_MAX;
        save_addon_config(rt);
    }
    ImGui::SameLine();
    help_marker("Target capture rate for every pass, independent of the game's frame rate.");

    if (ImGui::Checkbox("Record Audio", &g_audio_enabled)) save_addon_config(rt);
    ImGui::SameLine();
    help_marker("Records system audio via a separate FFmpeg process to <session>_audio.flac.\n\n"
                "The input args below are FFmpeg dshow options (output is always -c:a flac):\n"
                "- virtual-audio-capturer (VB-Cable loopback):  -f dshow -i audio=\"virtual-audio-capturer\"\n"
                "- Stereo Mix (if enabled in Windows sound settings):  -f dshow -i audio=\"Stereo Mix (Realtek(R) Audio)\"\n"
                "- Game-only audio: route the game to VB-Cable Output in Windows Sound Settings > "
                "App volume and device preferences.");
    if (g_audio_enabled && g_recording && g_audio_proc) {
        ImGui::SameLine();
        ImGui::TextColored(MPR_OK, "Recording...");
    }
    if (g_audio_enabled) {
        ImGui::Text("Audio input args:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##audioargs", g_audio_input_args, sizeof(g_audio_input_args))) save_addon_config(rt);
    }

    ImGui::Text("FFmpeg Path:");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##ffmpeg", g_ffmpeg_path, sizeof(g_ffmpeg_path))) save_addon_config(rt);

    ImGui::Text("Output Directory (leave empty for default):");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##outdir", g_output_dir, sizeof(g_output_dir))) save_addon_config(rt);
    if (strlen(g_output_dir) == 0)
        ImGui::TextDisabled("Default: %s", captures_dir(false).c_str());

    section_gap();
    ImGui::SeparatorText("Hotkeys");
    ImGui::TextDisabled("Click a binding, then press the key (hold Ctrl/Shift/Alt for a chord). "
                        "Esc cancels, X unbinds.");
    // One row per action in HOTKEY_DEFS (mpr_hotkeys.inc) - adding an action here costs nothing.
    // Clicking a binding arms capture; the key itself is read in on_reshade_present (hotkey_tick)
    // so it works regardless of ImGui keyboard focus. Only the capture flag is session state -
    // the bindings persist per action.
    if (ImGui::BeginTable("##hotkeys", 4, ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < HK_COUNT; ++i) {
            const HotkeyDef& d = HOTKEY_DEFS[i];
            const bool capturing = (g_hotkey_capture == i);
            const int  clash     = hotkey_conflict(i);
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(d.label);

            ImGui::TableNextColumn();
            if (clash >= 0) ImGui::PushStyleColor(ImGuiCol_Text, MPR_WARN);
            if (ImGui::Button(capturing ? "press a key...  (Esc cancels)" : hotkey_label((HotkeyId)i), ImVec2(200, 0)))
                g_hotkey_capture = capturing ? -1 : i;   // second click cancels
            if (clash >= 0) ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            // Draw-list glyphs don't inherit BeginDisabled's alpha, so the cross fades by hand.
            const bool can_clear = (g_hotkeys[i].vk != 0 || capturing);
            ImGui::BeginDisabled(!can_clear);
            if (mpr_glyph_button("##unbind", MPR_GLY_CROSS,
                                 can_clear ? MPR_GLYPH_INK : mpr_alpha(MPR_GLYPH_INK, 70))) {
                g_hotkeys[i] = Hotkey{};
                if (capturing) g_hotkey_capture = -1;
                save_addon_config(rt);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unbind");

            ImGui::TableNextColumn();
            help_marker(d.help);
            if (clash >= 0) {
                ImGui::SameLine();
                ImGui::TextColored(MPR_WARN, "same key as \"%s\"", HOTKEY_DEFS[clash].label);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (g_osd_hidden)
        ImGui::TextColored(MPR_WARN, "On-screen overlays are hidden (%s to bring them back).",
                           hotkey_label(HK_OVERLAYS));

    section_gap();
    ImGui::SeparatorText("Appearance");
    if (ImGui::Checkbox("Camera UI theme (grey, sharp corners)", &g_ui_theme_camera)) save_addon_config(rt);
    ImGui::SameLine();
    help_marker("A minimal, neutral-grey, sharp-cornered theme for this addon's own windows, "
                "distinct from ReShade's default panel look. Turn off to fall back to ReShade's "
                "stock style instantly -- no rebuild or restart needed.");

    section_gap();
    ImGui::SeparatorText("Window layout");
    static const char* LAYOUT_LABELS[] = { "Snap to left edge", "Snap to right edge", "Remember last position" };
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("On game start", &g_layout_mode, LAYOUT_LABELS, IM_ARRAYSIZE(LAYOUT_LABELS))) {
        g_layout_force_frame = g_live_frame; // preview the choice immediately
        save_addon_config(rt);
    }
    ImGui::SameLine();
    help_marker("Where this panel (and the Viewfinder beside it) go the first time the overlay is "
                "opened after a game launch. Both are sized from the current resolution, so a "
                "different game or a resolution change can't leave them off-screen or tiny.\n\n"
                "You can still drag and resize them freely afterwards - the snap only happens once "
                "per launch. 'Remember last position' keeps whatever ReShade saved in this game's ini.\n\n"
                "ReShade's own window opens on the right, so 'left' keeps the two clear of each other.");
    ImGui::SameLine(0, 16);
    if (ImGui::Button("Apply now")) g_layout_force_frame = g_live_frame;
    if (ImGui::IsWindowDocked())
        ImGui::TextColored(MPR_WARN, "Panel is docked into another window - drag it out by the title bar to position it.");

    section_gap();
    ImGui::SeparatorText("Advanced");
    // destroy_pass_resources JOINS each pass's worker (queue drain, FFmpeg stdin close, process
    // wait) on the render thread -- the exact stall toggle_recording's detached join exists to
    // avoid, times every enabled pass -- and would leave g_recording true with no workers behind
    // it, which the frame loop's diagnostic tick then reads as encoder deaths. Not offered mid-take.
    ImGui::BeginDisabled(g_recording);
    if (ImGui::Button("Reset All GPU Resources")) {
        for (auto& p : g_passes) destroy_pass_resources(rt->get_device(), p);
        fx_free_all(rt->get_device());
        native_free_all(rt->get_device());
        live_destroy_all(rt->get_device());
        reshade::log::message(reshade::log::level::info, "MultipassRecorder: Pipeline reset by user.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    help_marker(g_recording
        ? "Destroys and recreates all staging textures, thumbnails and cached views.\n\n"
          "Unavailable while recording: it has to shut down every pass's encoder first, which "
          "would freeze the game until they all finish. Stop the take, then reset."
        : "Destroys and recreates all staging textures, thumbnails and cached views. "
          "Use if thumbnails/previews look stale or corrupted (e.g. after a resolution change).");

    section_gap();
    ImGui::TextDisabled("MultipassRecorder %s", g_version);
}

// ---- Stills tab: what one shutter press grabs, and how each buffer is written ----
// Combo plumbing: the SETTING is a ShotFmt value, the widget wants an index (see mpr_state.inc).
static int shot_fmt_index(const int* choices, int n, int v) {
    for (int i = 0; i < n; ++i) if (choices[i] == v) return i;
    return 0;
}
static bool shot_fmt_combo(const char* label, const int* choices, const char* const* labels, int n, int* value) {
    int idx = shot_fmt_index(choices, n, *value);
    if (!ImGui::Combo(label, &idx, labels, n)) return false;
    *value = choices[idx];
    return true;
}

static void draw_stills_tab(effect_runtime* rt)
{
    ImGui::Spacing();
    bool changed = false;

    ImGui::SeparatorText("What a shot grabs");
    changed |= ImGui::Checkbox("Screen", &g_shotcfg.src_screen);
    ImGui::SameLine();
    help_marker("The game view AFTER your ReShade effects but BEFORE any UI is drawn - so the "
                "ReShade menu, this panel, the recording OSD, Debug View and the composition "
                "guides are all absent from the pixels. The game's own HUD is still in it (that "
                "is part of the frame the engine rendered).");
    changed |= ImGui::Checkbox("Every enabled pass", &g_shotcfg.src_passes);
    ImGui::SameLine();
    help_marker("One still per pass ticked in the Record tab - depth, normals, engine buffers, "
                "effect layers - all grabbed from the SAME game frame as the screen, so they "
                "register with each other pixel for pixel. This is the thing a normal screenshot "
                "key can't do.");

    // Honest preview of what a press will produce. Pass formats are only known once a buffer is
    // resolved, so the HDR half is stated as a rule rather than a count.
    {
        int npass = 0, nhdr = 0;
        for (auto& p : g_passes) {
            if (!p.enabled || (p.is_custom && !p.promoted)) continue;
            npass++;
            if (pass_is_hdr_src(p)) nhdr++;
        }
        const int nsrc = (g_shotcfg.src_screen ? 1 : 0) + (g_shotcfg.src_passes ? npass : 0);
        if (nsrc == 0)
            ImGui::TextColored(MPR_WARN, "Nothing selected - a shot would write no files.");
        else if (g_shotcfg.src_passes && nhdr > 0 && g_shotcfg.hdr_preview && shot_fmt_is_exr(g_shotcfg.hdr_fmt))
            ImGui::TextDisabled("%d source%s per shot, plus a PNG preview beside each HDR one (%d).",
                                nsrc, nsrc == 1 ? "" : "s", nhdr);
        else
            ImGui::TextDisabled("%d source%s per shot.", nsrc, nsrc == 1 ? "" : "s");
    }

    section_gap();
    ImGui::SeparatorText("Format");
    ImGui::SetNextItemWidth(240.0f);
    changed |= shot_fmt_combo("Screen", SHOT_LDR_CHOICES, SHOT_LDR_LABELS, 4, &g_shotcfg.screen_fmt);
    ImGui::SameLine();
    help_marker("PNG 8-bit is lossless and what you want for sharing. An 8-bit source is written "
                "through untouched - no decode, no rounding.\n\n"
                "If the swapchain is an HDR one (float), the screen follows the HDR row below "
                "instead: it is not crushed to 8-bit just because this says PNG.");

    ImGui::SetNextItemWidth(240.0f);
    changed |= shot_fmt_combo("Pass buffers (0..1)", SHOT_LDR_CHOICES, SHOT_LDR_LABELS, 4, &g_shotcfg.ldr_fmt);
    ImGui::SameLine();
    help_marker("For passes whose buffer is fixed-range: 8-bit RGBA, 10-bit, 16-bit unorm. "
                "16-bit PNG/TIFF store those exactly; 8-bit PNG is fine for anything that was "
                "8-bit to begin with (depth, normals, world).");

    ImGui::SetNextItemWidth(240.0f);
    changed |= shot_fmt_combo("HDR buffers (float)", SHOT_HDR_CHOICES, SHOT_HDR_LABELS, 3, &g_shotcfg.hdr_fmt);
    ImGui::SameLine();
    help_marker("For float buffers (r11g11b10, rgba16f, rgba32f, r9e5).\n\n"
                "EXR is written SCENE-LINEAR with no exposure and no curve baked in - the whole "
                "range the engine rendered, gradeable in Resolve/Nuke/AE exactly like a render "
                "layer. Half is 16 bits per channel and is what film VFX ships; float is 32 and "
                "twice the size for precision you will not see.\n\n"
                "PNG 16-bit is the fallback if you need something an image viewer opens: linear "
                "fixed point, divide by 4096 to get linear back (same convention as the PNG "
                "sequence codec).");

    if (shot_fmt_is_exr(g_shotcfg.hdr_fmt)) {
        ImGui::SetNextItemWidth(240.0f);
        changed |= ImGui::Combo("EXR compression", &g_shotcfg.exr_comp, SHOT_EXR_COMP_LABELS, 4);
        ImGui::SameLine();
        help_marker("All four are LOSSLESS - this trades file size against CPU time while writing. "
                    "ZIP16 is the usual choice for renders; None writes fastest if a burst is "
                    "stalling.");
        changed |= ImGui::Checkbox("Also write a viewable PNG beside each EXR", &g_shotcfg.hdr_preview);
        ImGui::SameLine();
        help_marker("A linear EXR looks black in an ordinary image viewer. The companion PNG bakes "
                    "the pass's current exposure and the same view the Viewfinder shows, so you "
                    "can browse a folder of shots and still have the untouched EXR to grade.");
    }
    if (g_shotcfg.screen_fmt == SHOTF_JPEG || g_shotcfg.ldr_fmt == SHOTF_JPEG) {
        ImGui::SetNextItemWidth(240.0f);
        changed |= ImGui::SliderInt("JPEG quality", &g_shotcfg.jpeg_q, 1, 31, "q %d");
        ImGui::SameLine();
        help_marker("FFmpeg's -q:v scale, and it runs BACKWARDS: 1 is best quality, 31 is worst. "
                    "2 is visually lossless for stills.");
    }

    section_gap();
    ImGui::SeparatorText("Capture");
    ImGui::TextUnformatted("Shutter:");
    ImGui::SameLine();
    if (g_hotkeys[HK_STILL].vk != 0) ImGui::TextColored(MPR_OK, "%s", hotkey_label(HK_STILL));
    else                             ImGui::TextColored(MPR_WARN, "unbound");
    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("Rebind in Setup")) g_ui_goto_tab = TAB_SETUP;

    ImGui::SetNextItemWidth(240.0f);
    changed |= ImGui::SliderInt("Self-timer", &g_shotcfg.timer_sec, 0, 30, g_shotcfg.timer_sec == 0 ? "off" : "%d s");
    ImGui::SameLine();
    help_marker("Delay between the key press and the frame that is actually grabbed - time to let "
                "go of the keyboard, or to let an animation reach the pose you want. A countdown "
                "shows on screen.");

    ImGui::SetNextItemWidth(240.0f);
    changed |= ImGui::SliderInt("Burst", &g_shotcfg.burst, 1, 20, g_shotcfg.burst == 1 ? "single frame" : "%d frames");
    if (g_shotcfg.burst > 1) {
        ImGui::SetNextItemWidth(240.0f);
        changed |= ImGui::SliderInt("Burst spacing", &g_shotcfg.burst_ms, 0, 2000, "%d ms");
        ImGui::SameLine();
        help_marker("Wall-clock gap between frames of a burst. Each frame is a complete set (screen "
                    "+ every pass) and lands in the same folder, numbered _f01, _f02, ...\n\n"
                    "Spacing is a floor, not a guarantee: a frame is only grabbed on a game frame, "
                    "and the writer encodes them one at a time.");
    }

    section_gap();
    ImGui::SeparatorText("Files");
    ImGui::SetNextItemWidth(300.0f);
    changed |= ImGui::Combo("Own folder", &g_shotcfg.folder, SHOT_FOLDER_LABELS, 3);
    changed |= ImGui::Checkbox("Write an info .txt beside the files", &g_shotcfg.sidecar);
    ImGui::SameLine();
    help_marker("Records what each file in the shot actually is: which buffer it came from, its "
                "pixel format, and whether the pixels are linear or a baked view. That distinction "
                "is the thing you cannot recover by looking at the files a week later.");
    changed |= ImGui::Checkbox("Flash + confirmation on screen", &g_shotcfg.flash);
    ImGui::SameLine();
    help_marker("A brief shutter flash when the frame is grabbed. Never enters the capture - the "
                "still is taken before any overlay is drawn - and the confirmation appears whether "
                "or not the ReShade menu is open.");

    ImGui::TextDisabled("Saved to: %s", shot_dir(false).c_str());
    ImGui::SameLine();
    help_marker("A stills\\ subfolder of the capture directory (Setup tab). Kept separate from the "
                "recordings so a still is never mistaken for a take by post-processing tools.");

    // Reactive result line: what the last shot actually did, success or failure. Same discipline
    // as the encoder report - it names what happened, and only after it happened.
    {
        std::string msg; bool err = false;
        {
            std::lock_guard<std::mutex> lk(g_shot_msg_mx);
            msg = g_shot_msg; err = g_shot_msg_err;
        }
        if (!msg.empty()) {
            section_gap();
            if (err) mpr_icon_warn();
            ImGui::TextColored(err ? MPR_ERR : MPR_OK_DIM, "%s", msg.c_str());
        }
    }

    if (changed) save_cfg(rt); // several sliders on this tab -- see save_cfg
}

// One tab item: honors a pending programmatic switch (g_ui_goto_tab) and records the active tab
// so the panel reopens where the user left off (persisted as active_tab).
static bool mpr_tab_item(effect_runtime* rt, const char* label, int idx)
{
    const ImGuiTabItemFlags fl = (g_ui_goto_tab == idx) ? ImGuiTabItemFlags_SetSelected : 0;
    if (!ImGui::BeginTabItem(label, nullptr, fl)) return false;
    if (g_ui_active_tab != idx) { g_ui_active_tab = idx; save_addon_config(rt); }
    return true;
}

static void on_draw_settings(effect_runtime* rt)
{
    g_runtime = rt;
    if (imgui_function_table_instance() == nullptr) return;
    // ReShade itself already called Begin() for this window before invoking us, using whatever
    // style was active at that moment -- so this scope cannot restyle the window's own shell
    // (background/border/rounding/scrollbar), only what we draw live below (tabs, buttons, tables,
    // checkboxes, separators), which is the visible majority of the panel. See CLAUDE.md / the
    // theme plan for why.
    MprThemeScope theme;
    seed_window_layout();
    // Stamp the frame the launch snap ran on; from the NEXT frame on, only the user's own dragging
    // (or an explicit Apply now) moves the windows.
    if (g_layout_applied_frame == 0xffffffff) g_layout_applied_frame = g_live_frame;
    // Panel is open -> keep the live RT registry enumerating for a few frames (drives the
    // per-pass thumbnails and the Buffers gallery even when not recording).
    g_settings_ttl = 3;
    g_overlay_frame = g_live_frame; // menu is open THIS frame -> ReShade's dockspace is live now (see draw_buffer_preview)

    // ---- Recording cockpit: always visible above the tab bar, whichever tab is active ----
    if (g_recording) draw_recording_stats(rt);
    draw_header_strip(rt);
    ImGui::Spacing();

    // Reopen on the tab the user left off (one-shot after load; ImGui only keeps tab selection
    // for the session, our active_tab ini value carries it across runs).
    static bool tab_restored = false;
    if (!tab_restored) { g_ui_goto_tab = g_ui_active_tab; tab_restored = true; }

    // The Viewfinder window is drawn once per frame from on_reshade_overlay (mpr_osd.inc), not
    // here -- see draw_buffer_preview's header comment for why.
    if (ImGui::BeginTabBar("##mpr_tabs")) {
        if (mpr_tab_item(rt, "Record",   TAB_RECORD))   { draw_record_tab(rt);   ImGui::EndTabItem(); }
        if (mpr_tab_item(rt, "Buffers",  TAB_BUFFERS))  { draw_buffers_tab(rt);  ImGui::EndTabItem(); }
        if (mpr_tab_item(rt, "Effects",  TAB_EFFECTS))  { draw_effects_tab(rt);  ImGui::EndTabItem(); }
        if (mpr_tab_item(rt, "Stills",   TAB_STILLS))   { draw_stills_tab(rt);   ImGui::EndTabItem(); }
        if (mpr_tab_item(rt, "Overlays", TAB_OVERLAYS)) { draw_overlays_tab(rt); ImGui::EndTabItem(); }
        if (mpr_tab_item(rt, "Setup",    TAB_SETUP))    { draw_setup_tab(rt);    ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    g_ui_goto_tab = -1; // programmatic switches are one-shot
}

