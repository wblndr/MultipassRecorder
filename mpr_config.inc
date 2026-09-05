// === MultipassRecorder — mpr_config.inc ===
// Config persistence — load/save HUD + addon settings to the ReShade .ini.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.

// ---- HUD Config Persistence ----
static effect_runtime* g_runtime = nullptr;
static void on_draw_settings(effect_runtime* rt);
static void on_draw_osd(effect_runtime* rt);
static bool pass_is_hdr_src(const RecordPass& p); // defined with the encoding UI; used for live metering

static void load_hud_config(effect_runtime* rt) {
#if RESHADE_API_VERSION >= 13
    if (!rt) return;
    const char* S = "MultipassRecorderHUD";
    reshade::get_config_value(rt, S, "show_badge",      g_hud.show_badge);
    reshade::get_config_value(rt, S, "show_duration",   g_hud.show_duration);
    reshade::get_config_value(rt, S, "show_graph",      g_hud.show_graph);
    reshade::get_config_value(rt, S, "show_stats",      g_hud.show_stats);
    reshade::get_config_value(rt, S, "show_ev_meter",   g_hud.show_ev_meter);
    reshade::get_config_value(rt, S, "show_crosshair",  g_hud.show_crosshair);
    reshade::get_config_value(rt, S, "show_thirds",     g_hud.show_thirds);
    reshade::get_config_value(rt, S, "show_letterbox",  g_hud.show_letterbox);
    reshade::get_config_value(rt, S, "letterbox_idx",   g_hud.letterbox_idx);
    reshade::get_config_value(rt, S, "lbox_alpha",      g_hud.lbox_alpha);
    // Colors are stored as comma-separated strings
    char cbuf[64] = "";
    size_t csz = sizeof(cbuf);
    if (reshade::get_config_value(rt, S, "rec_color", cbuf, &csz))
        sscanf_s(cbuf, "%f,%f,%f", &g_hud.rec_color[0], &g_hud.rec_color[1], &g_hud.rec_color[2]);
    csz = sizeof(cbuf);
    if (reshade::get_config_value(rt, S, "warn_color", cbuf, &csz))
        sscanf_s(cbuf, "%f,%f,%f", &g_hud.warn_color[0], &g_hud.warn_color[1], &g_hud.warn_color[2]);
    csz = sizeof(cbuf);
    if (reshade::get_config_value(rt, S, "guide_color", cbuf, &csz))
        sscanf_s(cbuf, "%f,%f,%f,%f", &g_hud.guide_color[0], &g_hud.guide_color[1], &g_hud.guide_color[2], &g_hud.guide_color[3]);
#endif
}

static void save_hud_config(effect_runtime* rt) {
#if RESHADE_API_VERSION >= 13
    if (!rt) return;
    const char* S = "MultipassRecorderHUD";
    reshade::set_config_value(rt, S, "show_badge",      g_hud.show_badge);
    reshade::set_config_value(rt, S, "show_duration",   g_hud.show_duration);
    reshade::set_config_value(rt, S, "show_graph",      g_hud.show_graph);
    reshade::set_config_value(rt, S, "show_stats",      g_hud.show_stats);
    reshade::set_config_value(rt, S, "show_ev_meter",   g_hud.show_ev_meter);
    reshade::set_config_value(rt, S, "show_crosshair",  g_hud.show_crosshair);
    reshade::set_config_value(rt, S, "show_thirds",     g_hud.show_thirds);
    reshade::set_config_value(rt, S, "show_letterbox",  g_hud.show_letterbox);
    reshade::set_config_value(rt, S, "letterbox_idx",   g_hud.letterbox_idx);
    reshade::set_config_value(rt, S, "lbox_alpha",      g_hud.lbox_alpha);
    char cbuf[64];
    sprintf_s(cbuf, "%.3f,%.3f,%.3f", g_hud.rec_color[0], g_hud.rec_color[1], g_hud.rec_color[2]);
    reshade::set_config_value(rt, S, "rec_color", cbuf);
    sprintf_s(cbuf, "%.3f,%.3f,%.3f", g_hud.warn_color[0], g_hud.warn_color[1], g_hud.warn_color[2]);
    reshade::set_config_value(rt, S, "warn_color", cbuf);
    sprintf_s(cbuf, "%.3f,%.3f,%.3f,%.3f", g_hud.guide_color[0], g_hud.guide_color[1], g_hud.guide_color[2], g_hud.guide_color[3]);
    reshade::set_config_value(rt, S, "guide_color", cbuf);
#endif
}

// ---- Addon Config Persistence ----
// Capture-rate bounds, shared by the load clamp below and the Setup-tab input. The frame loop
// divides by g_fps (interval_us), so 0 makes the capture beat never fire and a negative makes
// every frame due — a hand-edited or half-typed value must not survive either end of the ini.
static const int MPR_FPS_MIN = 1, MPR_FPS_MAX = 480;

static void load_addon_config(effect_runtime* rt) {
#if RESHADE_API_VERSION >= 13
    if (!rt) return;
    const char* S = "MultipassRecorder";
    reshade::get_config_value(rt, S, "fps", g_fps);
    if (g_fps < MPR_FPS_MIN || g_fps > MPR_FPS_MAX) g_fps = 30;
    size_t path_sz = sizeof(g_ffmpeg_path);
    reshade::get_config_value(rt, S, "ffmpeg_path", g_ffmpeg_path, &path_sz);
    size_t out_sz = sizeof(g_output_dir);
    reshade::get_config_value(rt, S, "output_dir", g_output_dir, &out_sz);
    reshade::get_config_value(rt, S, "audio_enabled", g_audio_enabled);
    reshade::get_config_value(rt, S, "ui_theme_camera", g_ui_theme_camera);
    size_t audio_sz = sizeof(g_audio_input_args);
    reshade::get_config_value(rt, S, "audio_input_args", g_audio_input_args, &audio_sz);
    // Hotkeys: one packed int per action (see mpr_hotkeys.inc). A key that isn't in the ini falls
    // back to its shipped default rather than keeping whatever the previous preset had, so preset
    // switching is deterministic. Legacy migration: inis written before the table existed stored
    // only the record key, as a bare VK under "record_hotkey".
    for (int i = 0; i < HK_COUNT; ++i) {
        int packed = 0;
        if (reshade::get_config_value(rt, S, HOTKEY_DEFS[i].ini_key, packed)) {
            g_hotkeys[i] = hk_unpack(packed);
            continue;
        }
        g_hotkeys[i] = HOTKEY_DEFS[i].def;
        int legacy = 0;
        if (i == HK_RECORD && reshade::get_config_value(rt, S, "record_hotkey", legacy) && hk_vk_valid(legacy))
            g_hotkeys[i].vk = legacy;
    }

    // Record tab glance strip + last active panel tab. (The sec_universal / sec_engine /
    // sec_promoted keys are dead: the Record tab's collapsible pass sections were replaced by
    // plain group labels. A stale key in an existing ini is simply never read again.)
    reshade::get_config_value(rt, S, "show_glance",   g_show_glance);
    reshade::get_config_value(rt, S, "active_tab",    g_ui_active_tab);
    // TAB_MAX, not TAB_SETUP: tabs are appended to MprTab, so a hardcoded upper bound silently
    // reset everyone who left the panel on a tab added after it (Effects, 4 > TAB_SETUP's 3).
    if (g_ui_active_tab < TAB_RECORD || g_ui_active_tab > TAB_MAX) g_ui_active_tab = TAB_RECORD;
    reshade::get_config_value(rt, S, "layout_mode", g_layout_mode);
    if (g_layout_mode < LAYOUT_LEFT || g_layout_mode > LAYOUT_FREE) g_layout_mode = LAYOUT_LEFT;
    // "scope_native2": key renamed when the default flipped to curve view, so inis saved under
    // the old default don't silently pin everyone to the deprecated native-view default.
    reshade::get_config_value(rt, S, "scope_native2", g_scope_native);
    reshade::get_config_value(rt, S, "pv_show_clip",  g_pv_show_clip);
    reshade::get_config_value(rt, S, "scope_curve",   g_scope_curve);
    reshade::get_config_value(rt, S, "preview_fs",    g_preview_fs);
    reshade::get_config_value(rt, S, "wf_show",       g_wf_show);
    reshade::get_config_value(rt, S, "wf_mode",       g_wf_mode);
    reshade::get_config_value(rt, S, "wf_bright",     g_wf_bright);

    // Still capture (mpr_shot.inc). Every index is clamped on load for the same reason enc_hdr is:
    // a stale value from an older build must not select a format table entry that isn't there.
    reshade::get_config_value(rt, S, "shot_screen",     g_shotcfg.src_screen);
    reshade::get_config_value(rt, S, "shot_passes",     g_shotcfg.src_passes);
    reshade::get_config_value(rt, S, "shot_screen_fmt", g_shotcfg.screen_fmt);
    reshade::get_config_value(rt, S, "shot_ldr_fmt",    g_shotcfg.ldr_fmt);
    reshade::get_config_value(rt, S, "shot_hdr_fmt",    g_shotcfg.hdr_fmt);
    if (!shot_fmt_in(SHOT_LDR_CHOICES, 4, g_shotcfg.screen_fmt)) g_shotcfg.screen_fmt = SHOTF_PNG8;
    if (!shot_fmt_in(SHOT_LDR_CHOICES, 4, g_shotcfg.ldr_fmt))    g_shotcfg.ldr_fmt    = SHOTF_PNG8;
    if (!shot_fmt_in(SHOT_HDR_CHOICES, 3, g_shotcfg.hdr_fmt))    g_shotcfg.hdr_fmt    = SHOTF_EXR_HALF;
    reshade::get_config_value(rt, S, "shot_hdr_preview", g_shotcfg.hdr_preview);
    reshade::get_config_value(rt, S, "shot_exr_comp",    g_shotcfg.exr_comp);
    if (g_shotcfg.exr_comp < 0 || g_shotcfg.exr_comp > 3) g_shotcfg.exr_comp = 3;
    reshade::get_config_value(rt, S, "shot_jpeg_q",      g_shotcfg.jpeg_q);
    if (g_shotcfg.jpeg_q < 1 || g_shotcfg.jpeg_q > 31) g_shotcfg.jpeg_q = 2;
    reshade::get_config_value(rt, S, "shot_timer",       g_shotcfg.timer_sec);
    if (g_shotcfg.timer_sec < 0 || g_shotcfg.timer_sec > 60) g_shotcfg.timer_sec = 0;
    reshade::get_config_value(rt, S, "shot_burst",       g_shotcfg.burst);
    if (g_shotcfg.burst < 1 || g_shotcfg.burst > 60) g_shotcfg.burst = 1;
    reshade::get_config_value(rt, S, "shot_burst_ms",    g_shotcfg.burst_ms);
    if (g_shotcfg.burst_ms < 0 || g_shotcfg.burst_ms > 10000) g_shotcfg.burst_ms = 250;
    reshade::get_config_value(rt, S, "shot_folder",      g_shotcfg.folder);
    if (g_shotcfg.folder < SHOTDIR_MULTI || g_shotcfg.folder > SHOTDIR_NEVER) g_shotcfg.folder = SHOTDIR_MULTI;
    reshade::get_config_value(rt, S, "shot_sidecar",     g_shotcfg.sidecar);
    reshade::get_config_value(rt, S, "shot_flash",       g_shotcfg.flash);

    for (auto& p : g_passes) {
        char key[64];
        sprintf_s(key, "%s_enabled", p.name);
        reshade::get_config_value(rt, S, key, p.enabled);

        // Structured encoding config.
        // Every index is guarded: worker_func's codec/preset branches are coupled to these tables
        // by position, so a stale value picks a branch that doesn't match what the UI shows.
        sprintf_s(key, "%s_codec",    p.name); reshade::get_config_value(rt, S, key, p.enc_codec);
        if (p.enc_codec < ENC_HEVC || p.enc_codec > ENC_CUSTOM) p.enc_codec = ENC_HEVC;
        sprintf_s(key, "%s_bits",     p.name); reshade::get_config_value(rt, S, key, p.enc_bits);
        if (p.enc_bits < BITS_8 || p.enc_bits > BITS_10) p.enc_bits = BITS_8;
        sprintf_s(key, "%s_chroma",   p.name); reshade::get_config_value(rt, S, key, p.enc_chroma);
        if (p.enc_chroma < CHROMA_444 || p.enc_chroma > CHROMA_LUMA) p.enc_chroma = CHROMA_444;
        sprintf_s(key, "%s_hdr",      p.name); reshade::get_config_value(rt, S, key, p.enc_hdr);
        if (p.enc_hdr < 0 || p.enc_hdr > HDR_LOG2) p.enc_hdr = HDR_PQ; // guard stale/removed values
        sprintf_s(key, "%s_exposure", p.name); reshade::get_config_value(rt, S, key, p.enc_exposure);
        sprintf_s(key, "%s_ae",       p.name); reshade::get_config_value(rt, S, key, p.ae_auto);
        sprintf_s(key, "%s_evcomp",   p.name); reshade::get_config_value(rt, S, key, p.ev_comp);
        sprintf_s(key, "%s_lossless", p.name); reshade::get_config_value(rt, S, key, p.enc_lossless);
        sprintf_s(key, "%s_cq",       p.name); reshade::get_config_value(rt, S, key, p.enc_cq);
        if (p.enc_cq < 0 || p.enc_cq > 51) p.enc_cq = 18; // the CQ slider's range
        sprintf_s(key, "%s_preset",   p.name); reshade::get_config_value(rt, S, key, p.enc_preset);
        if (p.enc_preset < 1 || p.enc_preset > 7) p.enc_preset = 4;

        sprintf_s(key, "%s_args", p.name);
        size_t sz = sizeof(p.custom_args);
        reshade::get_config_value(rt, S, key, p.custom_args, &sz);

        // Engine-RT fingerprint (used by SRC_ENGINE_RT passes: normals_native + promoted customs).
        int fv = (int)p.rt_fmt, wv = (int)p.rt_min_w, ov = p.rt_ordinal, bv = (int)p.rt_bind;
        sprintf_s(key, "%s_rtfmt",  p.name); reshade::get_config_value(rt, S, key, fv);
        sprintf_s(key, "%s_rtminw", p.name); reshade::get_config_value(rt, S, key, wv);
        sprintf_s(key, "%s_rtord",  p.name); reshade::get_config_value(rt, S, key, ov);
        sprintf_s(key, "%s_rtbind", p.name); reshade::get_config_value(rt, S, key, bv);
        if (bv != LIVE_RTV && bv != LIVE_UAV) bv = LIVE_RTV; // guard stale/missing values
        p.rt_fmt = (format)fv; p.rt_min_w = (uint32_t)wv; p.rt_ordinal = ov; p.rt_bind = (uint8_t)bv;

        // FX-layer identity + controls (custom slots promoted from the Effects tab).
        {
            size_t sz2;
            sz2 = sizeof(p.fx_effect); sprintf_s(key, "%s_fxeffect", p.name); reshade::get_config_value(rt, S, key, p.fx_effect, &sz2);
            sz2 = sizeof(p.fx_tech);   sprintf_s(key, "%s_fxtech",   p.name); reshade::get_config_value(rt, S, key, p.fx_tech,   &sz2);
            sz2 = sizeof(p.fx_dbg_var);sprintf_s(key, "%s_fxdbgvar", p.name); reshade::get_config_value(rt, S, key, p.fx_dbg_var,&sz2);
            sprintf_s(key, "%s_fxdbgon",  p.name); reshade::get_config_value(rt, S, key, p.fx_dbg_on);
            sprintf_s(key, "%s_fxdbgval", p.name); reshade::get_config_value(rt, S, key, p.fx_dbg_val);
            sprintf_s(key, "%s_fxseed",   p.name); reshade::get_config_value(rt, S, key, p.fx_seed);
            if (p.fx_seed < FXSEED_SCENE || p.fx_seed > FXSEED_BLACK) p.fx_seed = FXSEED_SCENE;
        }

        // Native depth/normals controls (mpr_native.inc). These were MultipassRecorder.fx uniforms,
        // which meant they lived in ReShade's preset and could not differ per pass; now they are
        // ours. Every numeric one is clamped on load for the usual reason — a hand-edited or
        // stale ini must not put the compute shader somewhere its math stops meaning anything
        // (a zero/negative far plane divides the reverse projection into garbage).
        sprintf_s(key, "%s_nfar",    p.name);
        const bool had_native = reshade::get_config_value(rt, S, key, p.native_far_plane);
        if (!(p.native_far_plane >= NATIVE_FAR_MIN && p.native_far_plane <= NATIVE_FAR_MAX)) p.native_far_plane = 1000.0f;
        // MIGRATION. An ini written before the native passes existed carries depth at 8 bits,
        // because that was all the old RGBA8 .fx could produce. Loading it verbatim would quietly
        // re-impose the exact quantization this pass was rewritten to remove — the user would get
        // a "fixed" depth pass that still bands, with nothing on screen explaining why. The absence
        // of the nfar key is what identifies such an ini; lift depth to 10-bit once, and from then
        // on the saved value (whatever the user chooses) wins normally.
        if (!had_native && p.source == SRC_NATIVE_DEPTH && p.enc_bits == BITS_8) p.enc_bits = BITS_10;
        sprintf_s(key, "%s_ndmul",   p.name); reshade::get_config_value(rt, S, key, p.native_depth_mul);
        if (!(p.native_depth_mul >= NATIVE_DMUL_MIN && p.native_depth_mul <= NATIVE_DMUL_MAX)) p.native_depth_mul = 1.0f;
        sprintf_s(key, "%s_nrev",    p.name); reshade::get_config_value(rt, S, key, p.native_reversed);
        sprintf_s(key, "%s_nflip",   p.name); reshade::get_config_value(rt, S, key, p.native_upside_down);
        sprintf_s(key, "%s_nlog",    p.name); reshade::get_config_value(rt, S, key, p.native_log_curve);
        sprintf_s(key, "%s_nrluma",  p.name); reshade::get_config_value(rt, S, key, p.native_reverse_luma);
        sprintf_s(key, "%s_nlinear", p.name); reshade::get_config_value(rt, S, key, p.native_linear_output);
        sprintf_s(key, "%s_nfov",    p.name); reshade::get_config_value(rt, S, key, p.native_fov);
        if (!(p.native_fov >= NATIVE_FOV_MIN && p.native_fov <= NATIVE_FOV_MAX)) p.native_fov = 60.0f;
        sprintf_s(key, "%s_nnmul",   p.name); reshade::get_config_value(rt, S, key, p.native_normal_mul);
        if (!(p.native_normal_mul >= NATIVE_NMUL_MIN && p.native_normal_mul <= NATIVE_NMUL_MAX)) p.native_normal_mul = 1.0f;
        sprintf_s(key, "%s_nusefov", p.name); reshade::get_config_value(rt, S, key, p.native_use_fov);
        sprintf_s(key, "%s_nfx",     p.name); reshade::get_config_value(rt, S, key, p.native_flip_x);
        sprintf_s(key, "%s_nfy",     p.name); reshade::get_config_value(rt, S, key, p.native_flip_y);
        sprintf_s(key, "%s_nfz",     p.name); reshade::get_config_value(rt, S, key, p.native_flip_z);

        if (p.is_custom) {
            // Custom slot: restore whether it holds a promoted buffer + its display label.
            sprintf_s(key, "%s_promoted", p.name); reshade::get_config_value(rt, S, key, p.promoted);
            size_t lsz = sizeof(p.label);
            sprintf_s(key, "%s_label", p.name); reshade::get_config_value(rt, S, key, p.label, &lsz);
            // Which KIND of thing this slot holds. Slots used to be engine-RT-only, so a config
            // written before FX layers existed has no key here -- default to the old meaning.
            int sv = SRC_ENGINE_RT;
            sprintf_s(key, "%s_src", p.name); reshade::get_config_value(rt, S, key, sv);
            p.source = (sv == SRC_FX_LAYER) ? SRC_FX_LAYER : SRC_ENGINE_RT;
            if (p.source == SRC_FX_LAYER && !p.fx_tech[0]) { p.source = SRC_ENGINE_RT; p.promoted = false; }
            if (!p.promoted) p.enabled = false; // an empty slot is never recordable
        } else {
            // Fixed pass: source kind is LOCKED to its default. This ignores any stale saved
            // "assign" override (e.g. world accidentally pointed at the HDR buffer).
            p.source = p.def_source;
        }
    }
    // Viewfinder: restore which pass was last shown + whether the window was open, so the
    // viewfinder reopens exactly where you left it. Runs after the pass loop so a custom slot's
    // 'promoted' flag is already restored. A raw engine-buffer handle preview isn't persistable
    // (handles are per-session), so only pass previews are restored.
    reshade::get_config_value(rt, S, "preview_idx", g_preview_last_idx);
    bool prev_open = false;
    reshade::get_config_value(rt, S, "preview_open", prev_open);
    if (prev_open && g_preview_last_idx >= 0 && g_preview_last_idx < N_FIXED + N_CUSTOM &&
        (g_preview_last_idx < N_FIXED || g_passes[g_preview_last_idx].promoted)) {
        g_preview_pass = &g_passes[g_preview_last_idx];
        g_preview_handle = 0;
        g_preview_open = true;
    }
#endif
}

static void save_addon_config(effect_runtime* rt) {
#if RESHADE_API_VERSION >= 13
    if (!rt) return;
    const char* S = "MultipassRecorder";
    reshade::set_config_value(rt, S, "fps", g_fps);
    reshade::set_config_value(rt, S, "ffmpeg_path", g_ffmpeg_path);
    reshade::set_config_value(rt, S, "output_dir", g_output_dir);
    reshade::set_config_value(rt, S, "audio_enabled", g_audio_enabled);
    reshade::set_config_value(rt, S, "ui_theme_camera", g_ui_theme_camera);
    reshade::set_config_value(rt, S, "audio_input_args", g_audio_input_args);
    for (int i = 0; i < HK_COUNT; ++i)
        reshade::set_config_value(rt, S, HOTKEY_DEFS[i].ini_key, hk_pack(g_hotkeys[i]));

    // Record tab glance strip + last active panel tab.
    reshade::set_config_value(rt, S, "show_glance",   g_show_glance);
    reshade::set_config_value(rt, S, "active_tab",    g_ui_active_tab);
    reshade::set_config_value(rt, S, "layout_mode", g_layout_mode);
    reshade::set_config_value(rt, S, "scope_native2", g_scope_native);
    reshade::set_config_value(rt, S, "pv_show_clip",  g_pv_show_clip);
    reshade::set_config_value(rt, S, "scope_curve",   g_scope_curve);
    reshade::set_config_value(rt, S, "preview_fs",    g_preview_fs);
    reshade::set_config_value(rt, S, "wf_show",       g_wf_show);
    reshade::set_config_value(rt, S, "wf_mode",       g_wf_mode);
    reshade::set_config_value(rt, S, "wf_bright",     g_wf_bright);

    // Still capture (mpr_shot.inc).
    reshade::set_config_value(rt, S, "shot_screen",      g_shotcfg.src_screen);
    reshade::set_config_value(rt, S, "shot_passes",      g_shotcfg.src_passes);
    reshade::set_config_value(rt, S, "shot_screen_fmt",  g_shotcfg.screen_fmt);
    reshade::set_config_value(rt, S, "shot_ldr_fmt",     g_shotcfg.ldr_fmt);
    reshade::set_config_value(rt, S, "shot_hdr_fmt",     g_shotcfg.hdr_fmt);
    reshade::set_config_value(rt, S, "shot_hdr_preview", g_shotcfg.hdr_preview);
    reshade::set_config_value(rt, S, "shot_exr_comp",    g_shotcfg.exr_comp);
    reshade::set_config_value(rt, S, "shot_jpeg_q",      g_shotcfg.jpeg_q);
    reshade::set_config_value(rt, S, "shot_timer",       g_shotcfg.timer_sec);
    reshade::set_config_value(rt, S, "shot_burst",       g_shotcfg.burst);
    reshade::set_config_value(rt, S, "shot_burst_ms",    g_shotcfg.burst_ms);
    reshade::set_config_value(rt, S, "shot_folder",      g_shotcfg.folder);
    reshade::set_config_value(rt, S, "shot_sidecar",     g_shotcfg.sidecar);
    reshade::set_config_value(rt, S, "shot_flash",       g_shotcfg.flash);

    for (auto& p : g_passes) {
        char key[64];
        sprintf_s(key, "%s_enabled", p.name);
        reshade::set_config_value(rt, S, key, p.enabled);

        // Structured encoding config.
        sprintf_s(key, "%s_codec",    p.name); reshade::set_config_value(rt, S, key, p.enc_codec);
        sprintf_s(key, "%s_bits",     p.name); reshade::set_config_value(rt, S, key, p.enc_bits);
        sprintf_s(key, "%s_chroma",   p.name); reshade::set_config_value(rt, S, key, p.enc_chroma);
        sprintf_s(key, "%s_hdr",      p.name); reshade::set_config_value(rt, S, key, p.enc_hdr);
        sprintf_s(key, "%s_exposure", p.name); reshade::set_config_value(rt, S, key, p.enc_exposure);
        sprintf_s(key, "%s_ae",       p.name); reshade::set_config_value(rt, S, key, p.ae_auto);
        sprintf_s(key, "%s_evcomp",   p.name); reshade::set_config_value(rt, S, key, p.ev_comp);
        sprintf_s(key, "%s_lossless", p.name); reshade::set_config_value(rt, S, key, p.enc_lossless);
        sprintf_s(key, "%s_cq",       p.name); reshade::set_config_value(rt, S, key, p.enc_cq);
        sprintf_s(key, "%s_preset",   p.name); reshade::set_config_value(rt, S, key, p.enc_preset);

        sprintf_s(key, "%s_args", p.name);
        reshade::set_config_value(rt, S, key, p.custom_args);

        // Engine-RT fingerprint (normals_native + promoted custom passes).
        sprintf_s(key, "%s_rtfmt",  p.name); reshade::set_config_value(rt, S, key, (int)p.rt_fmt);
        sprintf_s(key, "%s_rtminw", p.name); reshade::set_config_value(rt, S, key, (int)p.rt_min_w);
        sprintf_s(key, "%s_rtord",  p.name); reshade::set_config_value(rt, S, key, p.rt_ordinal);
        sprintf_s(key, "%s_rtbind", p.name); reshade::set_config_value(rt, S, key, (int)p.rt_bind);
        // FX-layer identity + controls (Effects-tab passes).
        sprintf_s(key, "%s_fxeffect", p.name); reshade::set_config_value(rt, S, key, p.fx_effect);
        sprintf_s(key, "%s_fxtech",   p.name); reshade::set_config_value(rt, S, key, p.fx_tech);
        sprintf_s(key, "%s_fxdbgvar", p.name); reshade::set_config_value(rt, S, key, p.fx_dbg_var);
        sprintf_s(key, "%s_fxdbgon",  p.name); reshade::set_config_value(rt, S, key, p.fx_dbg_on);
        sprintf_s(key, "%s_fxdbgval", p.name); reshade::set_config_value(rt, S, key, p.fx_dbg_val);
        sprintf_s(key, "%s_fxseed",   p.name); reshade::set_config_value(rt, S, key, p.fx_seed);
        // Native depth/normals controls (mpr_native.inc).
        sprintf_s(key, "%s_nfar",    p.name); reshade::set_config_value(rt, S, key, p.native_far_plane);
        sprintf_s(key, "%s_ndmul",   p.name); reshade::set_config_value(rt, S, key, p.native_depth_mul);
        sprintf_s(key, "%s_nrev",    p.name); reshade::set_config_value(rt, S, key, p.native_reversed);
        sprintf_s(key, "%s_nflip",   p.name); reshade::set_config_value(rt, S, key, p.native_upside_down);
        sprintf_s(key, "%s_nlog",    p.name); reshade::set_config_value(rt, S, key, p.native_log_curve);
        sprintf_s(key, "%s_nrluma",  p.name); reshade::set_config_value(rt, S, key, p.native_reverse_luma);
        sprintf_s(key, "%s_nlinear", p.name); reshade::set_config_value(rt, S, key, p.native_linear_output);
        sprintf_s(key, "%s_nfov",    p.name); reshade::set_config_value(rt, S, key, p.native_fov);
        sprintf_s(key, "%s_nnmul",   p.name); reshade::set_config_value(rt, S, key, p.native_normal_mul);
        sprintf_s(key, "%s_nusefov", p.name); reshade::set_config_value(rt, S, key, p.native_use_fov);
        sprintf_s(key, "%s_nfx",     p.name); reshade::set_config_value(rt, S, key, p.native_flip_x);
        sprintf_s(key, "%s_nfy",     p.name); reshade::set_config_value(rt, S, key, p.native_flip_y);
        sprintf_s(key, "%s_nfz",     p.name); reshade::set_config_value(rt, S, key, p.native_flip_z);
        if (p.is_custom) {
            sprintf_s(key, "%s_promoted", p.name); reshade::set_config_value(rt, S, key, p.promoted);
            sprintf_s(key, "%s_label",    p.name); reshade::set_config_value(rt, S, key, p.label);
            sprintf_s(key, "%s_src",      p.name); reshade::set_config_value(rt, S, key, (int)p.source);
        }
    }
    // Viewfinder: which pass is shown + whether the window is open. g_preview_last_idx is
    // refreshed from the live selection here (and left untouched when nothing is selected, e.g.
    // the window is closed) so the last-viewed pass survives a close.
    for (int i = 0; i < N_FIXED + N_CUSTOM; ++i)
        if (g_preview_pass == &g_passes[i]) { g_preview_last_idx = i; break; }
    reshade::set_config_value(rt, S, "preview_idx",  g_preview_last_idx);
    reshade::set_config_value(rt, S, "preview_open", g_preview_open);
#endif
}

