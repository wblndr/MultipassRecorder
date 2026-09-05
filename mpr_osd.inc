// === MultipassRecorder — mpr_osd.inc ===
// On-screen overlay — composition guides + the always-on recording/exposure OSD.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.

static bool srv_dims(device* dev, resource_view srv, uint32_t* w, uint32_t* h, format* f); // defined with the pass table UI; used by draw_debug_view below
static bool srv_is_mono(device* dev, resource_view srv); // same file; a raw 1-channel view drawn as RGB is solid red, which draw_debug_view must not paint over the game
static void draw_buffer_preview(effect_runtime* rt); // defined with the pass table UI; called once per frame below so it works regardless of which overlay window triggered it
static void draw_pass_stats_table(bool compact); // defined with the settings UI; the OSD shows its compact form so panel + OSD numbers always match

// ---- Composition Guides (screen-only, never enter the captured textures) ----
static const float LETTERBOX_RATIOS[] = { 2.39f, 1.85f, 2.35f };
static const char* LETTERBOX_LABELS[] = { "2.39:1  Cinemascope", "1.85:1  Flat", "2.35:1  Anamorphic" };

static void draw_composition_guides()
{
    if (imgui_function_table_instance() == nullptr) return;
    if (!g_hud.show_crosshair && !g_hud.show_thirds && !g_hud.show_letterbox) return;

    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Line color for crosshair and thirds
    auto& gc = g_hud.guide_color;
    ImU32 line_col = IM_COL32(
        (int)(gc[0] * 255), (int)(gc[1] * 255),
        (int)(gc[2] * 255), (int)(gc[3] * 255));

    // Letterbox bars — drawn first so lines render on top
    if (g_hud.show_letterbox) {
        int li = g_hud.letterbox_idx;
        if (li < 0 || li >= (int)(sizeof(LETTERBOX_RATIOS)/sizeof(LETTERBOX_RATIOS[0])))
            li = 0;
        float bar_h = (ds.y - ds.x / LETTERBOX_RATIOS[li]) * 0.5f;
        if (bar_h > 0.0f) {
            ImU32 bar_col = IM_COL32(0, 0, 0, (int)(g_hud.lbox_alpha * 255));
            dl->AddRectFilled(ImVec2(0.f, 0.f),        ImVec2(ds.x, bar_h),         bar_col);
            dl->AddRectFilled(ImVec2(0.f, ds.y - bar_h), ImVec2(ds.x, ds.y),         bar_col);
        }
    }

    // Rule of thirds — 2 vertical + 2 horizontal lines
    if (g_hud.show_thirds) {
        float x1 = ds.x / 3.0f, x2 = ds.x * 2.0f / 3.0f;
        float y1 = ds.y / 3.0f, y2 = ds.y * 2.0f / 3.0f;
        dl->AddLine(ImVec2(x1, 0.f), ImVec2(x1, ds.y), line_col, 1.0f);
        dl->AddLine(ImVec2(x2, 0.f), ImVec2(x2, ds.y), line_col, 1.0f);
        dl->AddLine(ImVec2(0.f, y1), ImVec2(ds.x, y1), line_col, 1.0f);
        dl->AddLine(ImVec2(0.f, y2), ImVec2(ds.x, y2), line_col, 1.0f);
    }

    // Crosshair — broken cross with center circle
    if (g_hud.show_crosshair) {
        float cx = ds.x * 0.5f, cy = ds.y * 0.5f;
        const float arm = 18.0f, gap = 5.0f;
        dl->AddLine(ImVec2(cx - arm - gap, cy), ImVec2(cx - gap, cy), line_col, 1.0f);
        dl->AddLine(ImVec2(cx + gap, cy),       ImVec2(cx + arm + gap, cy), line_col, 1.0f);
        dl->AddLine(ImVec2(cx, cy - arm - gap), ImVec2(cx, cy - gap), line_col, 1.0f);
        dl->AddLine(ImVec2(cx, cy + gap),       ImVec2(cx, cy + arm + gap), line_col, 1.0f);
        dl->AddCircle(ImVec2(cx, cy), gap, line_col, 16, 1.0f);
    }
}

// ---- Debug View: project the previewed pass full-screen directly over the game itself, like a
// renderer's built-in buffer-visualization mode (e.g. a debug depth view) -- instead of the
// Viewfinder's small/maximized-in-menu image. Toggled from the "Debug View" button on a pass,
// or the checkbox inside the Viewfinder (mpr_ui.inc); see g_debug_view (mpr_state.inc).
//
// Drawn on the BACKGROUND draw list, which Dear ImGui always renders *behind* every window --
// so it sits over the game but under the ReShade menu. That's what makes it predictable to turn
// off: however you got here, opening the ReShade menu (Home) always puts the toggle back on top.
// Screen-only, like the composition guides: this never touches the captured pixels.
//
// Suspended while g_recording is true (see hires_preview_wanted in on_reshade_present): it's a
// pre-roll framing tool, buys nothing once you're actually rolling, and would otherwise keep
// costing a GPU copy + CPU tonemap scan every 4th frame that the capture pipeline could use
// instead. Resumes automatically the moment recording stops.
static void draw_debug_view(effect_runtime* rt)
{
    if (!(g_debug_view && g_preview_pass)) return;
    MprThemeScope theme(true); // hud: stays borderless; covers both Begin/End branches below
    RecordPass& p = *g_preview_pass;
    const char* nm = pass_display_name(p);
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(10.0f, ds.y - 10.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;

    if (g_recording) {
        ImGui::Begin("##MPR_DEBUGVIEW_TAG", nullptr, flags);
        ImGui::TextDisabled("DEBUG VIEW: %s -- paused while recording (resumes on stop)", nm);
        ImGui::End();
        return;
    }

    device* dev = rt->get_device();
    resource_view srv = pass_preview_srv(rt, p);
    uint32_t pw = 0, ph = 0; format pf = format::unknown;
    const bool bound = srv_dims(dev, srv, &pw, &ph, &pf);
    // Prefer the processed viewfinder frame (either backend, see pv_display_srv): exposed WYSIWYG
    // for HDR sources, and on the GPU backend LDR passes too (so channel isolation projects over
    // the game). Falls back to the raw SRV when nothing processed is available.
    resource_view show_srv = pv_display_srv();
    const bool processed = show_srv.handle != 0;
    if (!processed) show_srv = srv;
    // A 1-channel source drawn raw samples as float4(v,0,0,1) and is SOLID RED (srv_is_mono,
    // mpr_ui.inc) — and this draw covers the whole screen, so the raw fallback would paint the
    // game red and read as a catastrophic failure of a pass that is working fine. The native depth
    // pass is exactly that case. Only the processed frame can show it as grayscale, so without one,
    // draw nothing and say what is missing.
    const bool mono_raw = !processed && srv_is_mono(dev, show_srv);

    if (bound && show_srv.handle && !mono_raw)
        ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)show_srv.handle, ImVec2(0.0f, 0.0f), ds);

    ImGui::Begin("##MPR_DEBUGVIEW_TAG", nullptr, flags);
    ImGui::Text("DEBUG VIEW: %s", nm);
    ImGui::TextDisabled("%s", !bound   ? "not bound this frame"
                            : mono_raw ? "single channel - no processed frame yet (open the Viewfinder to build one)"
                                       : "Home -> Debug View checkbox to exit");
    ImGui::End();
}

static void on_reshade_overlay(effect_runtime* rt)
{
    // The settings panel is provided as a ReShade overlay (register_overlay in AddonInit),
    // so it shows/hides together with the ReShade UI and ReShade persists its open state.
    // Here we only draw the always-on recording OSD and composition guides over the game.
    //
    // If ReShade could not hand us a matching ImGui function table (e.g. our bundled ImGui
    // is newer than this ReShade runtime supports), the table is null and ANY ImGui:: call
    // dereferences null and crashes the game. Bail out. Recording is unaffected: the F9
    // hotkey lives in on_reshade_present.
    if (imgui_function_table_instance() == nullptr) return;

    // Any widget of ours holding the keyboard (a focused text field, a slider being dragged) must
    // not let a hotkey fire too — same idea as ReShade's own _ignore_shortcuts. Set from here
    // because ImGui's "is anything active" state is context-wide, so one check per frame covers
    // the settings panel and the Viewfinder both. Consumed/decayed by hotkey_tick.
    if (ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput)
        g_hotkey_block_ttl = 3;

    draw_debug_view(rt);   // deliberately not gated by g_osd_hidden — it has its own hotkey
    draw_buffer_preview(rt);
    // Shutter flash / self-timer countdown / "saved" toast. Also deliberately ahead of the
    // g_osd_hidden gate: it is transient confirmation of something you just asked for, not
    // standing screen furniture, and hiding the overlays to take a clean shot shouldn't also
    // hide whether the shot worked. It can never reach the pixels — a still is grabbed at
    // reshade_finish_effects, before any of this is drawn.
    shot_draw_feedback();
    if (g_osd_hidden) return;
    draw_composition_guides();
    // OSD draws while recording, or as an always-on DSLR-style exposure meter when idle (an HDR
    // pass is enabled and metering). g_meter_pass is only set while the Viewfinder is closed.
    if (g_recording || (g_hud.show_ev_meter && g_meter_pass))
        on_draw_osd(rt);
}

// ---- On-Screen Display (always visible, like FPS counter) ----
static void on_draw_osd(effect_runtime* rt)
{
    if (imgui_function_table_instance() == nullptr) return; // null table => ImGui unusable, see on_reshade_overlay

    // Recording-only telemetry: pass counts + stability history. Skipped when the OSD is up purely
    // as the idle DSLR-style exposure meter (no workers running, nothing to tally).
    uint32_t max_frames = 0;
    int active_passes = 0;
    bool any_drops = false;
    uint32_t total_drops = 0;
    if (g_recording) {
        sample_pass_fps();
        for (auto& p : g_passes) {
            if (p.enabled && p.worker.joinable()) {
                active_passes++;
                uint32_t f = p.frame_count.load();
                if (f > max_frames) max_frames = f;
                uint32_t d = p.dropped_frames.load();
                total_drops += d;
                if (d > 0) any_drops = true;
            }
        }
        // Update stability history (1.0 = perfect, 0.0 = all dropped)
        uint32_t new_frames = max_frames - g_prev_total_frames;
        uint32_t new_drops  = total_drops - g_prev_total_drops;
        float stability = (new_frames + new_drops) > 0 ? (float)new_frames / (float)(new_frames + new_drops) : 1.0f;
        g_stability_history[g_stability_idx % STABILITY_HISTORY_SIZE] = stability;
        g_stability_idx++;
        g_prev_total_frames = max_frames;
        g_prev_total_drops  = total_drops;
    }

    // Single always-on indicator pinned to the top-right corner. A fixed, named, borderless
    // window means any duplicate draw sources collapse into ONE window (the OSD was previously
    // drawn window-less and appeared multiple times).
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x - 10.0f, 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags osd_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
    MprThemeScope theme(true); // hud: stays borderless -- see draw_buffer_preview for the Begin ordering
    ImGui::Begin("##MPR_REC_OSD", nullptr, osd_flags);

    ImVec4 rec_col = ImVec4(g_hud.rec_color[0], g_hud.rec_color[1], g_hud.rec_color[2], 1.0f);
    ImVec4 warn_col = ImVec4(g_hud.warn_color[0], g_hud.warn_color[1], g_hud.warn_color[2], 1.0f);

    // Recording badge: pulsing dot + REC + duration + pass count on one chained line. The old
    // [!] drop flag is gone — the stability graph (turns red) and per-pass stats (names the
    // culprit pass) already report drops, so a third bare indicator was pure redundancy.
    // The whole badge/graph/stats block is recording-only; the exposure meter draws either way.
    bool line_started = false;
    if (g_recording && g_hud.show_badge) {
        float pulse = (sinf((float)clock() / CLOCKS_PER_SEC * 6.0f) * 0.5f) + 0.5f;
        ImGui::TextColored(ImVec4(rec_col.x, rec_col.y, rec_col.z, pulse), "*");
        ImGui::SameLine(0, 4);
        ImGui::TextColored(rec_col, "REC");
        line_started = true;
    }
    if (g_recording && g_hud.show_duration) {
        if (line_started) ImGui::SameLine(0, 6);
        ImGui::Text("%s", format_hms(recording_elapsed_sec()).c_str());
        line_started = true;
    }
    if (g_recording && g_hud.show_badge) {
        if (line_started) ImGui::SameLine(0, 6);
        if (g_audio_proc) ImGui::Text("| %d passes +audio", active_passes);
        else              ImGui::Text("| %d passes", active_passes);
        line_started = true;
    }
    if (line_started) ImGui::NewLine(); // end the SameLine chain

    if (g_recording && g_hud.show_graph) {
        // Reorder history into a linear array for PlotHistogram
        float ordered[STABILITY_HISTORY_SIZE];
        int count = g_stability_idx < STABILITY_HISTORY_SIZE ? g_stability_idx : STABILITY_HISTORY_SIZE;
        int start = g_stability_idx % STABILITY_HISTORY_SIZE;
        for (int i = 0; i < count; ++i)
            ordered[i] = g_stability_history[(start + i) % STABILITY_HISTORY_SIZE];
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, any_drops ? warn_col : mpr_va(MPR_OK, 0.8f));
        ImGui::PlotHistogram("##stability", ordered, count, 0, nullptr, 0.0f, 1.0f, ImVec2(160, 20));
        ImGui::PopStyleColor();
    }

    // Live camera-style exposure meter for the metered HDR pass: shows highlight/shadow headroom
    // (in stops) against that pass's capture window, plus live clip/crush. Needle sweeps from
    // crushing shadows (left) to clipping highlights (right); centered + green = the range fits.
    if (g_hud.show_ev_meter) {
      ImGui::SeparatorText("EXPOSURE");
      // The meter needs a live sample of an r11g11b10 HDR pass. When there's nothing to show,
      // say WHY rather than rendering blank — the silent no-op reads as "the meter is broken".
      if (!(g_meter_pass && g_pv.have_frame && g_pv.n_total > 0)) {
        const char* why;
        if (g_preview_open)      why = "using Viewfinder scope";
        else if (!g_meter_pass)  why = "enable an HDR pass to meter";
        else {
            bool r11 = g_meter_pass->cap_format == format::r11g11b10_float
                    || g_meter_pass->rt_fmt     == format::r11g11b10_float;
            why = r11 ? "sampling HDR buffer..." : "meter needs an r11g11b10 buffer";
        }
        ImGui::TextDisabled("%s", why);
      } else {
        float fp, wp2, cp; curve_window_stops(g_meter_pass->enc_hdr, &fp, &wp2, &cp);
        const float e = g_meter_pass->enc_exposure;
        // Population split against the exposure-shifted capture window (shared with the
        // Viewfinder scope -- scope_population in mpr_state.inc -- so the %'s always agree).
        double crush, kept, clip, tot;
        scope_population(e, fp, cp, &crush, &kept, &clip, &tot);
        const float clippct  = tot > 0 ? (float)(100.0 * clip  / tot) : 0.0f;
        const float crushpct = tot > 0 ? (float)(100.0 * crush / tot) : 0.0f;
        const float keptpct  = tot > 0 ? (float)(100.0 * kept  / tot) : 0.0f;

        // DSLR-style status line: metering mode + EV compensation + applied exposure. While
        // recording, AE is frozen — shown as "AE-LOCK" so it's clear the value can't drift.
        char pshort[16]; pass_short_name(*g_meter_pass, pshort, sizeof(pshort));
        if (g_meter_pass->ae_auto) {
            ImGui::TextColored(g_recording ? MPR_WARN : MPR_INFO, g_recording ? "AE-LOCK" : "AE");
            if (g_meter_pass->ev_comp != 0.0f) { ImGui::SameLine(0, 8); ImGui::Text("%+.1f EV", g_meter_pass->ev_comp); }
        } else {
            ImGui::TextDisabled("M");
        }
        ImGui::SameLine(0, 10); ImGui::TextDisabled("%s  %+.1f st", pshort, e);

        // Mini exposure scope: the same histogram/window primitives as the Viewfinder scope
        // (scope_draw_bars/marklines, curve view — bars slide with exposure, the capture window
        // stays put), condensed for the viewfinder with flat zone fills instead of hatching.
        // White line = diffuse white (0 st); blue/red lines = floor/ceil.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetCursorScreenPos(); o.y += 2.0f;
        const ScopeRect r{ o, 200.0f, 40.0f, fp - 1.5f, cp + 1.5f };
        dl->AddRectFilled(o, ImVec2(o.x + r.W, o.y + r.H), mpr_alpha(MPR_SCOPE_BG, 235), 3.0f);
        dl->AddRectFilled(ImVec2(o.x, o.y),           ImVec2(scope_x(r, fp), o.y + r.H), MPR_ZONE_CRUSH);
        dl->AddRectFilled(ImVec2(scope_x(r, fp), o.y), ImVec2(scope_x(r, cp), o.y + r.H), MPR_ZONE_KEPT);
        dl->AddRectFilled(ImVec2(scope_x(r, cp), o.y), ImVec2(o.x + r.W, o.y + r.H), MPR_ZONE_CLIP);
        scope_draw_bars(dl, r, e); // curve view: bars shift by exposure
        scope_draw_marklines(dl, r, fp, cp, 0.0f);
        ImGui::Dummy(ImVec2(r.W, r.H + 2));

        // Compact readout under the scope (mirrors the panel's zone %'s).
        ImGui::TextColored(MPR_OK_DIM, "%.0f%% kept", keptpct);
        ImGui::SameLine(0, 12); ImGui::TextColored(clippct  > 1.0f ? MPR_WARN      : MPR_DIM, "clip %.1f%%", clippct);
        ImGui::SameLine(0, 12); ImGui::TextColored(crushpct > 1.0f ? MPR_CRUSH_TXT : MPR_DIM, "crush %.0f%%", crushpct);
      }
    }

    // Per-pass telemetry: the same shared table as the settings panel (compact form: short
    // names, no 'written' column) so the bottleneck is visible while playing with the panel
    // closed -- and the two surfaces can never show different numbers.
    if (g_recording && g_hud.show_stats) {
        ImGui::SeparatorText("PASSES");
        draw_pass_stats_table(/*compact=*/true);
        if (total_drops && g_drop_reason[0])
            ImGui::TextColored(MPR_WARN, "%s", g_drop_reason);
    }

    // Encoder death, on the always-on overlay and NOT behind the stats toggle: this is the one
    // condition where the take is being lost right now rather than merely degraded, and it has to
    // reach you mid-shot with the panel closed. The full reason lives in the panel and the log;
    // here it's just which pass, so the line stays narrow. Recording-only — after a stop the
    // panel's retrospective report takes over.
    if (g_recording) {
        for (auto& p : g_passes) {
            if (!(p.enabled && p.worker.joinable())) continue;
            if (!p.enc_failed.load(std::memory_order_acquire)) continue;
            char snm[16]; pass_short_name(p, snm, sizeof(snm));
            ImGui::TextColored(MPR_ERR, "! %s encoder failed - not recording", snm);
        }
    }

    ImGui::End();
}

