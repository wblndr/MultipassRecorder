// === MultipassRecorder — mpr_control.inc ===
// Recording control & frame loop — runtime events, toggle_recording, preview/exposure metering, on_reshade_present.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.

// ---- RT Probe: dump one collected frame to <captures>/rt_probe/ ----
// Auto-enable the MultipassRecorder_Setup technique so depth/normals/world are never captured
// black just because the user didn't tick it in the ReShade shader list. The addon owns this
// technique's on-state now; re-applied on every effect (re)load via reshade_reloaded_effects
// (also attempted at runtime init, though effects usually aren't compiled that early).
static void mpr_enable_setup_technique(effect_runtime* rt) {
    if (!rt) return;
    effect_technique t = rt->find_technique(nullptr, "MultipassRecorder_Setup");
    if (t != 0 && !rt->get_technique_state(t)) rt->set_technique_state(t, true);
}
static void on_reshade_reloaded_effects(effect_runtime* rt) { mpr_enable_setup_technique(rt); }
static void on_set_current_preset_path(effect_runtime* rt, const char* path) { load_addon_config(rt); }

// ---- Stopping a take --------------------------------------------------------------------------
// ONE real stop, shared by the record hotkey and both runtime lifecycle handlers: signal every
// worker, stop the audio encoder, then join on the tracked background thread (g_stop_joiner) so
// the game doesn't freeze while FFmpeg finalises. Clearing g_recording on its own is NOT a stop —
// the workers stay parked with worker_stop false and joinable, so the next take skips the spawn
// and feeds frames into the PREVIOUS take's files under a stale g_session_time, and the audio
// FFmpeg keeps running (start_audio_recording no-ops while one is alive, so every later take
// would be silent).
static void stop_recording_async()
{
    stop_join_wait(); // a previous stop still owns this same state; never two joiners at once
    g_recording = false;
    bool any_worker = false;
    for (auto& p : g_passes) {
        if (!p.worker.joinable()) continue;
        { std::lock_guard<std::mutex> lk(p.q_mutex); p.worker_stop = true; }
        p.q_cv.notify_one();
        any_worker = true;
    }
    stop_audio_recording(); // non-blocking — finalises on its own tracked thread
    if (!any_worker) return;
    g_stop_joiner = std::thread([]() {
        for (auto& p : g_passes) {
            if (p.worker.joinable()) p.worker.join();
            // Clean up queue
            {
                std::lock_guard<std::mutex> lock(p.q_mutex);
                while (!p.queue.empty()) { p.release_buf(std::move(p.queue.front().buf)); p.queue.pop(); }
                p.queue_depth = 0;
                p.queue_bytes = 0;
            }
            p.drop_last_frame(); // held_mutex: the render thread moves out of this same buffer
        }
    });
}

static void on_init_effect_runtime(effect_runtime* rt) {
    // A runtime can be recreated mid-take (multi-swapchain title, device reset), so this has to be
    // a complete stop, not just g_recording = false — see stop_recording_async.
    stop_recording_async();
    g_runtime = rt; load_hud_config(rt); load_addon_config(rt); mpr_enable_setup_technique(rt);
}
static void on_destroy_effect_runtime(effect_runtime* rt) {
    stop_recording_async();
    stop_join_wait(); // the device dies with this runtime: the joiner must be done before we free
    for (auto& p : g_passes) destroy_pass_resources(rt->get_device(), p);
    fx_free_all(rt->get_device());
    native_free_all(rt->get_device()); // depth/normals/world targets live on this device too
    live_destroy_all(rt->get_device());
    pv_free(rt->get_device());
    waveform_free();
    tile_free();     // the glance strip's per-pass textures live on this device too
    shot_free(rt->get_device()); // a still mid-flight holds staging resources off this device
    g_runtime = nullptr;         // this runtime is gone; nothing may save config through it again
}

static void toggle_recording()
{
    if (g_recording) { stop_recording_async(); return; }
    // A stop that is still finalising owns every pass's worker handle, queue and held-frame cache,
    // and the take below reuses all three. Rendezvous with it here: the user pressed the key, so a
    // brief wait on THIS transition is fine — it's stopping that must never block the render thread.
    stop_join_wait();
    g_recording = true;
    {
        time_t t = time(nullptr);
        struct tm tm_info;
        localtime_s(&tm_info, &t);
        char stamp[64];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &tm_info);
        // Game prefix keeps captures from different games sorted/collision-free in one output dir
        // (g_game_tag is the exe stem, resolved once at startup -- see detect_game_profile).
        _snprintf_s(g_session_time, sizeof(g_session_time), _TRUNCATE, "%s_%s", g_game_tag, stamp);
        // Reset stability history for a fresh graph
        memset(g_stability_history, 0, sizeof(g_stability_history));
        g_stability_idx = 0;
        g_prev_total_frames = 0;
        g_prev_total_drops = 0;
        g_next_capture_time = std::chrono::high_resolution_clock::now();
        g_record_start_time = g_next_capture_time;
        g_drop_reason = ""; // fresh run -> fresh bottleneck verdict
        if (g_audio_enabled) {
            // sprintf_s would ABORT on overflow; g_session_time now carries the (exe-derived,
            // unbounded-length) game tag, so use the truncating form like the other filename sites.
            char filename[224]; _snprintf_s(filename, sizeof(filename), _TRUNCATE, "%s_audio.flac", g_session_time);
            start_audio_recording(captures_dir(true) + "\\" + filename);
        }
    }
}

static bool is_setup_active(effect_runtime* rt)
{
    bool active = false;
    rt->enumerate_techniques(nullptr, [&](effect_runtime* rt, effect_technique tech) {
        char name[128];
        rt->get_technique_name(tech, name);
        if (strcmp(name, "MultipassRecorder_Setup") == 0 || strcmp(name, "depthRecorder_Capture") == 0) {
            active = rt->get_technique_state(tech);
        }
    });
    return active;
}

// The preview/viewfinder pipeline itself (preview_capture_tick, preview_process, ae_update,
// pv_free and the DX11 GPU processor) lives in mpr_preview.inc, included just before this
// fragment; this file only drives it from the frame loop below.

// Push a held copy of a pass's last successfully-read frame onto its worker queue. Shared by two
// callers in on_reshade_present: backpressure (encoder overloaded -> the fresh GPU copy can't run
// this beat) and cadence-fill (the game rendered slower than g_fps, so we owe held frames to keep
// the exported frame count in step with real elapsed time). Bounded by max_queue so neither caller
// can grow the queue without limit. Returns false if there's nothing captured yet to duplicate or
// the queue is already at its dup ceiling.
static bool push_held_frame(RecordPass& p, size_t max_queue)
{
    if (p.queue_depth.load(std::memory_order_relaxed) >= max_queue) return false;
    // Byte budget as well as frame count: max_queue alone is ~10 GB per pass at 4K. Over it, the
    // caller's fallback IS the graceful one (a true, uncompensated drop) — better than the alloc.
    if (p.queue_bytes.load(std::memory_order_relaxed) >= MPR_MAX_QUEUE_BYTES) return false;
    FrameData fd;
    {
        // held_mutex: the stop joiner frees this cache from its own thread.
        std::lock_guard<std::mutex> hk(p.held_mutex);
        if (!p.has_last_frame) return false;
        fd.buf = p.acquire_buf(p.last_frame_buf.size());
        if (fd.buf.empty()) return false; // allocation failed -> drop, never unwind into the host
        memcpy(fd.buf.data(), p.last_frame_buf.data(), p.last_frame_buf.size());
        fd.width = p.last_frame_w;
        fd.height = p.last_frame_h;
        fd.src_pitch = p.last_frame_pitch;
        fd.bytes_per_px = p.last_frame_bpp;
    }
    const size_t nbytes = fd.buf.size();
    std::lock_guard<std::mutex> lock(p.q_mutex);
    p.queue.push(std::move(fd));
    p.queue_depth.fetch_add(1, std::memory_order_relaxed);
    p.queue_bytes.fetch_add(nbytes, std::memory_order_relaxed);
    p.q_cv.notify_one();
    return true;
}

// ---- Hotkeys ----------------------------------------------------------------------------------

// The passes the Viewfinder / Debug View can point at: the fixed ones always, plus any promoted
// custom slot (an empty slot has nothing to show). ONE definition of that rule, shared by the
// hotkeys below and the Viewfinder's pass picker (mpr_ui.inc). Returns the count and, via `cur`,
// the index of the currently previewed pass (-1 if the Viewfinder is on a raw engine buffer or
// nothing at all).
static int preview_pass_list(RecordPass** items, int& cur) {
    int n = 0; cur = -1;
    for (int i = 0; i < N_FIXED + N_CUSTOM; ++i) {
        RecordPass& p = g_passes[i];
        if (i >= N_FIXED && !p.promoted) continue;
        if (g_preview_pass == &p) cur = n;
        items[n++] = &p;
    }
    return n;
}

// Step the previewed pass by `delta` (0 = "just make sure something is selected"). Returns false
// when there is nothing previewable, which is what stops the Debug View hotkey from switching on
// with no subject.
static bool preview_select_delta(effect_runtime* rt, int delta) {
    RecordPass* items[N_FIXED + N_CUSTOM]; int cur = -1;
    const int n = preview_pass_list(items, cur);
    if (n == 0) return false;
    if (cur >= 0 && delta == 0) return true;
    const int idx = (cur < 0) ? 0 : ((cur + delta) % n + n) % n;
    g_preview_pass   = items[idx];
    g_preview_handle = 0;   // a pass preview and a raw engine-buffer preview are mutually exclusive
    save_addon_config(rt);
    return true;
}

// Consume this frame's keyboard: first the Setup tab's "press a key" capture, then the bindings.
// Scanned here rather than in the UI because is_key_pressed is per-frame runtime state — and this
// way both capture and the bindings work while the ReShade menu has keyboard focus, or is closed
// entirely. See mpr_hotkeys.inc for the action table.
static void hotkey_tick(effect_runtime* rt)
{
    const bool ui_owns_keyboard = g_hotkey_block_ttl > 0;
    if (g_hotkey_block_ttl > 0) g_hotkey_block_ttl--;

    // Capture mode: the next non-modifier key (plus whichever modifiers are held with it) becomes
    // the binding; Esc cancels. Deliberately NOT gated on ui_owns_keyboard — the click that armed
    // capture is itself a UI interaction — and it returns before dispatch so the frame that binds
    // a key can't also fire the action it just bound.
    if (g_hotkey_capture >= 0 && g_hotkey_capture < HK_COUNT) {
        if (rt->is_key_pressed(VK_ESCAPE)) { g_hotkey_capture = -1; return; }
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            if (hk_is_modifier(vk) || !rt->is_key_pressed((uint32_t)vk)) continue;
            Hotkey h;
            h.vk    = vk;
            h.ctrl  = rt->is_key_down(VK_CONTROL);
            h.shift = rt->is_key_down(VK_SHIFT);
            h.alt   = rt->is_key_down(VK_MENU);
            g_hotkeys[g_hotkey_capture] = h;
            g_hotkey_capture = -1;
            save_addon_config(rt);
            break;
        }
        return;
    }
    if (ui_owns_keyboard) return; // typing a path into a UI field must not toggle recording

    if (hotkey_pressed(rt, HK_RECORD))
        toggle_recording();
    if (hotkey_pressed(rt, HK_DEBUG_VIEW)) {
        if (g_debug_view) g_debug_view = false;
        else if (preview_select_delta(rt, 0)) g_debug_view = true;
    }
    if (hotkey_pressed(rt, HK_PASS_PREV)) preview_select_delta(rt, -1);
    if (hotkey_pressed(rt, HK_PASS_NEXT)) preview_select_delta(rt, +1);
    if (hotkey_pressed(rt, HK_OVERLAYS))  g_osd_hidden = !g_osd_hidden;
    if (hotkey_pressed(rt, HK_STILL))     shot_trigger();
}

static void on_reshade_present(effect_runtime* rt)
{
    // Measure the game's frame time every frame (idle included). The FX-layer hook runs earlier in
    // the frame than the capture-cadence decision below and uses this to predict it -- see
    // fx_beat_due in mpr_fxlayer.inc.
    {
        static std::chrono::high_resolution_clock::time_point prev = std::chrono::high_resolution_clock::now();
        const auto t = std::chrono::high_resolution_clock::now();
        const long long dt = std::chrono::duration_cast<std::chrono::microseconds>(t - prev).count();
        prev = t;
        // Clamp out alt-tabs / load-screen stalls: a multi-second "frame" would make the predictor
        // render an FX layer every single frame forever after.
        g_frame_dt_us = (dt > 0 && dt < 100000) ? dt : 16666;
    }

    hotkey_tick(rt);

    // Self-timer / burst spacing -> arm the grab frame. Runs BEFORE g_live_enum is recomputed
    // below, so the live-RT registry is already enumerating on the frame we're about to grab
    // (engine-buffer passes resolve their fingerprint against it and would otherwise find nothing).
    shot_arm_tick();

    // The Viewfinder lives inside the settings overlay, which ReShade only draws while
    // the menu is open. "Preview active" therefore means panel open AND on-screen (settings TTL not
    // yet drained). This keeps g_preview_open persistent across Home toggles — the preview reappears
    // when you reopen the menu — while letting the OSD meter take back over whenever the panel isn't
    // actually visible. (Without this the meter would stay suppressed forever after closing the menu
    // with Preview open, since its close box can't be clicked while the menu is hidden.)
    const bool preview_active = g_preview_open && g_settings_ttl > 0;
    // Debug View projects the previewed pass full-screen over the game via the background draw
    // list (mpr_osd.inc's draw_debug_view), which -- unlike the Viewfinder -- keeps
    // running with the ReShade menu closed. So its sampling/metering needs the same "always on"
    // treatment as preview_active, not the menu-gated one.
    const bool debug_view_active = g_debug_view && g_preview_pass != nullptr;
    // Both Debug View and the Viewfinder's own HDR sampling are PRE-ROLL monitoring tools --
    // once you're actually recording they buy you nothing (capture reads pass sources directly,
    // independent of this pipeline; both are screen-only and never enter the recorded pixels) but
    // still cost a GPU copy + CPU tonemap/histogram scan every 4th frame, competing with the
    // capture pipeline for the one thing that matters during a take. Suspend both the whole time
    // g_recording is true; they resume automatically on stop. The OSD exposure meter just below is
    // a DIFFERENT, deliberately-recording-aware live monitor (so you can see a shot clipping while
    // rolling, even though auto-exposure itself freezes) and is intentionally NOT suspended here.
    const bool hires_preview_wanted = (preview_active || debug_view_active) && !g_recording;

    // Pick the pass the exposure meter + auto-exposure read: while Debug View is showing an HDR
    // pass (and not recording), that pass IS the meter subject (so the OSD readout always matches
    // what's on screen); otherwise the first enabled HDR-source pass (while recording it must have
    // a live worker). This drives the always-on DSLR-style OSD meter and AE even before recording,
    // so you can expose while framing.
    g_meter_pass = nullptr;
    if (!g_recording && debug_view_active && pass_is_hdr_src(*g_preview_pass))
        g_meter_pass = g_preview_pass;
    else if (!hires_preview_wanted && g_hud.show_ev_meter)
        for (auto& p : g_passes)
            if (p.enabled && pass_is_hdr_src(p) && (!g_recording || p.worker.joinable())) { g_meter_pass = &p; break; }

    // Arm the live RT registry for the NEXT frame whenever the panel was drawn recently (the
    // Buffers gallery needs live thumbnails), an engine-RT pass is recording, or we're metering
    // an engine-RT pass.
    bool engine_recording = false;
    for (auto& p : g_passes)
        if (g_recording && p.enabled && p.source == SRC_ENGINE_RT) { engine_recording = true; break; }
    const bool meter_engine = g_meter_pass && g_meter_pass->source == SRC_ENGINE_RT;
    g_live_enum = (g_settings_ttl > 0) || engine_recording || meter_engine || g_shot_pending;
    if (g_settings_ttl > 0) g_settings_ttl--;
    // g_live_enum is the ONLY thing that toggles here. The push_descriptors / bind_render_targets /
    // draw / draw_indexed hooks used to be registered and unregistered from this spot to match it;
    // they are now registered once in AddonInit and never touched at runtime. reshade's event list
    // is a bare std::vector<void*> that invoke_addon_event iterates with no synchronization, so a
    // register (which can REALLOCATE) or unregister from the present thread while a D3D12 title's
    // worker threads are recording command lists — firing exactly these events — iterates a freed
    // buffer and calls a garbage function pointer. The old "present never overlaps a draw" reasoning
    // was DX11-only. Each hook body early-outs on g_live_enum, which is what the toggling bought.

    // Debug View and the Viewfinder's "Full screen" mode both stretch the preview bitmap across
    // the WHOLE screen, so they need it sized for that -- not the modest cap that's plenty for the
    // small in-panel image. Query the actual swapchain resolution (not a fixed guess) and use its
    // long side, up to a ceiling that bounds the per-tick CPU cost on very high-res displays.
    uint32_t pv_cap = 1024;
    if (hires_preview_wanted && (debug_view_active || g_preview_fs)) {
        uint32_t bw = 0, bh = 0;
        rt->get_screenshot_width_and_height(&bw, &bh);
        uint32_t long_side = bw > bh ? bw : bh;
        if (long_side > pv_cap) pv_cap = long_side > 1920 ? 1920 : long_side;
    }

    // Destroy thumbnail SRVs the game's buffers outlived, now that they're old enough that no
    // in-flight overlay draw list can still name them. Must run before g_live_frame advances (both
    // exit paths below bump it), and on EVERY present, not just menu-open ones — eviction happens
    // whether or not the panel is drawing.
    live_retire_tick(rt->get_device());

    // Own-pass sampling (Viewfinder / Debug View) is suspended during recording -- see
    // hires_preview_wanted above. The OSD meter's sampling (g_meter_pass) is NOT suspended: it
    // deliberately keeps running during a take as a live clip/crush monitor.
    if (hires_preview_wanted) preview_capture_tick(rt, nullptr, pv_cap);
    else if (g_meter_pass)    preview_capture_tick(rt, g_meter_pass, pv_cap);

    // Record tab glance strip: one pass processed per present, round-robin. Unlike the Viewfinder
    // above this is NOT suspended during a take -- a strip that goes dark the moment you start
    // rolling cannot tell you a pass went wrong mid-take, which is most of why it exists. The cost
    // is bounded instead by only running it while the strip is actually on screen: panel open
    // (g_settings_ttl), Record tab selected, strip not hidden. Closed panel = not a single dispatch.
    if (g_settings_ttl > 0 && g_ui_active_tab == TAB_RECORD && g_show_glance) tile_tick(rt);
    // Auto-exposure tracks whichever pass the meter is currently sampling (the previewed pass when
    // the panel is visible, else the OSD meter pass), and only while idle so recording freezes it.
    if (!g_recording) ae_update(hires_preview_wanted ? g_preview_pass : g_meter_pass);

    if (!g_recording) {
        // Stills are independent of recording, so they have to run on this path too (this is the
        // usual one: most stills are taken while not rolling). Mirrored at the end of the
        // recording path below, next to that path's own g_live_frame++.
        shot_grab_tick(rt);
        shot_drain_tick(rt);
        g_live_frame++;
        return;
    }

    device* dev = rt->get_device();
    command_queue* q = rt->get_command_queue();
    command_list* cmd = q->get_immediate_command_list();

    auto now = std::chrono::high_resolution_clock::now();
    // Fixed-cadence capture clock. Advancing g_next_capture_time by exactly one interval per beat
    // that has come due (rather than snapping it to "now" on each capture) stops the achieved rate
    // from quantizing to whole game-frame steps -- the old snap-to-now throttle turned a 50fps game
    // into a rock-solid 25fps capture (each 30fps beat needed 2 game frames, so it locked to 25).
    // When beats_due > 1 the game rendered slower than g_fps this interval, so we owe (beats_due - 1)
    // held frames (pad_frames) to keep the file's frame count in step with real elapsed time: FFmpeg
    // is told -r g_fps, so frame count IS playback duration -> a 10s take exports as 10s at any
    // game framerate.
    bool capture_this_frame = false;
    int  pad_frames = 0;
    const double interval_us = 1000000.0 / (double)g_fps;
    if (g_recording) {
        long long since = std::chrono::duration_cast<std::chrono::microseconds>(now - g_next_capture_time).count();
        if (since >= 0) {
            int beats_due = 1 + (int)((double)since / interval_us);
            // Being > MAX_CATCHUP beats behind isn't an fps mismatch, it's a stall (alt-tab, load
            // screen, a hitch). Don't backfill a burst of held frames for dead air -- that would just
            // desync against the real-time audio track and flood the queue; resync the clock to now.
            static constexpr int MAX_CATCHUP = 4;
            if (beats_due > MAX_CATCHUP) {
                beats_due = 1;
                g_next_capture_time = now + std::chrono::microseconds((long long)interval_us);
            } else {
                g_next_capture_time += std::chrono::microseconds((long long)(interval_us * beats_due));
            }
            capture_this_frame = true;
            pad_frames = beats_due - 1;
        }
    }

    // Diagnostic: Check if shader is active; also detect audio process crash
    static uint32_t diagnostic_counter = 0;
    if (g_recording && (diagnostic_counter++ % 60 == 0)) {
        if (!is_setup_active(rt)) {
            reshade::log::message(reshade::log::level::warning, "MultipassRecorder: Shader technique is NOT enabled in ReShade UI! Recording will be black.");
        }
        // FX-layer passes depend on the ReShade effect chain actually running this frame. If effects
        // are switched off globally, or the effect was unloaded/renamed since the pass was added,
        // reshade_begin_effects never refreshes the layer and the pass quietly records a frozen
        // image. Report it AFTER it happens, naming the pass -- never as a pre-emptive nag.
        for (auto& p : g_passes) {
            if (!(p.enabled && p.promoted && p.source == SRC_FX_LAYER)) continue;
            if (g_live_frame - p.fx_last_render_frame <= 2) continue;
            char msg[512]; // _TRUNCATE: sprintf_s would ABORT on overflow, and these interpolate
                           // arbitrary shader/technique names
            if (fx_find_technique(rt, p) == 0)
                _snprintf_s(msg, sizeof(msg), _TRUNCATE, "MultipassRecorder: pass '%s' is recording a frozen frame - technique '%s' (%s) isn't loaded.",
                            p.name, p.fx_tech, p.fx_effect[0] ? p.fx_effect : "any effect");
            else if (!p.fx_fmt_ok)
                _snprintf_s(msg, sizeof(msg), _TRUNCATE, "MultipassRecorder: pass '%s' can't record - the back buffer format %s isn't one this addon can encode.",
                            p.name, probe_format_name(p.fx_fmt));
            else
                _snprintf_s(msg, sizeof(msg), _TRUNCATE, "MultipassRecorder: pass '%s' is recording a frozen frame - ReShade effects aren't rendering (are they toggled off?).",
                            p.name);
            reshade::log::message(reshade::log::level::warning, msg);
            g_fx_report = msg + sizeof("MultipassRecorder: ") - 1;
        }
        // Catch-all for a worker that exited without publishing a reason. The worker's own paths
        // (FFmpeg failed to start, write failed, non-zero exit) all report via enc_fail, so this
        // only fires for an exit route that doesn't — but it means the pass is dead either way,
        // and the alternative is a pass that keeps showing "recording" while writing nothing.
        // worker_running is set on the render thread before the thread is created, so
        // "joinable but not running" can't be a spawn race.
        for (auto& p : g_passes) {
            if (!(p.enabled && p.worker.joinable())) continue;
            if (p.worker_running.load() || p.enc_failed.load(std::memory_order_acquire)) continue;
            const uint32_t wrote = p.frame_count.load();
            char msg[256];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        "encoder stopped after %u frame%s - see captures\\ffmpeg_log.txt.",
                        wrote, wrote == 1 ? "" : "s");
            enc_fail(&p, "%s", msg);
        }
        if (g_audio_proc && WaitForSingleObject(g_audio_proc, 0) == WAIT_OBJECT_0) {
            DWORD code = 0; GetExitCodeProcess(g_audio_proc, &code);
            char msg[128]; sprintf_s(msg, "MultipassRecorder: audio process exited unexpectedly, code %lu — check ffmpeg_log.txt", code);
            reshade::log::message(reshade::log::level::error, msg);
            CloseHandle(g_audio_proc); g_audio_proc = nullptr; g_audio_pid = 0;
            if (g_audio_stdin) { CloseHandle(g_audio_stdin); g_audio_stdin = nullptr; }
        }
    }

    if (g_recording && capture_this_frame) {
        static constexpr size_t MAX_DUP_QUEUE = 300;
        bool global_is_full = false;
        for (auto& p : g_passes) {
            // pass_is_live, not `enabled && joinable`: a pass whose FFmpeg died is no longer being
            // fed, so its stranded queue must not be read as backpressure — otherwise one dead
            // encoder would hold global_is_full true forever and drop every other pass's frames.
            if (pass_is_live(p)) {
                size_t qs = p.queue_depth.load(std::memory_order_relaxed);
                if (qs >= 60) { global_is_full = true; g_drop_reason = "encoder can't keep up (queue full)"; break; }
                // Also check if GPU pipeline is backed up (all 3 buffers in flight)
                if (p.copy_index - p.read_index >= 3) { global_is_full = true; g_drop_reason = "GPU readback backed up (copy/map)"; break; }
            }
        }
        if (global_is_full) {
            // Can't do a fresh GPU copy this beat. Re-push the last captured frame (push_held_frame)
            // instead of skipping it outright so the exported file's frame count stays in sync with
            // real elapsed time — still counted as dropped (the content wasn't captured fresh; motion
            // during this interval isn't represented), but no longer silently speeds up the video.
            // Bounded by MAX_DUP_QUEUE so a SUSTAINED overload (the encoder fundamentally can't
            // sustain the configured fps/quality, not just a momentary stall) can't grow the queue
            // forever — beyond that it falls back to a true, uncompensated drop.
            for (auto& p : g_passes) {
                if (!pass_is_live(p)) continue;
                p.dropped_frames++;
                push_held_frame(p, MAX_DUP_QUEUE);
            }
        } else {
            // ---- PHASE 1: GPU Copy ----
            for (auto& p : g_passes) {
                if (!p.enabled) continue;
                if (p.is_custom && !p.promoted) continue; // empty promotable slot
                // Encoder gone (reported by the worker, or a source resize caught below): stop
                // copying for it. Its failure stands for the rest of the take — nothing here can
                // restart FFmpeg mid-recording, and a silent retry loop would just respawn into the
                // same limit. Gated on the worker still EXISTING so the verdict can't outlive the
                // take it belongs to: once the stop path has joined it, the pass falls through to
                // the spawn below, which is the one place that clears the flag.
                if (p.enc_failed.load(std::memory_order_acquire) && p.worker.joinable()) continue;

                // Acquire this pass's source resource for the frame. Shared with the still path
                // (pass_source_resource, mpr_shot.inc) so a still can never capture a different
                // buffer than a recording of the same pass would. A zero handle means the .fx
                // texture / effect layer / engine fingerprint didn't resolve this frame.
                const resource tex = pass_source_resource(rt, dev, p);
                if (tex.handle == 0) continue;
                resource_desc desc = dev->get_resource_desc(tex);

                if (!check_and_update_staging(dev, p, desc)) continue; // sets p.width/height
                p.cap_format = desc.texture.format; // remember the real format for the worker's conversion

                // Spawn the worker only now that width/height are known (FFmpeg args need them).
                if (!p.worker.joinable()) {
                    // Filename stem: the slot name for normal passes, the technique name for FX
                    // layers (see pass_file_tag) -- "<session>_MXAO.mp4" reads far better in an
                    // NLE bin than "<session>_rt3.mp4".
                    char tag[96]; pass_file_tag(p, tag, sizeof(tag));
                    std::string out;
                    if (p.enc_codec == ENC_PNG) { // image sequence -> its own folder
                        std::string subdir = captures_dir(true) + "\\" + g_session_time + "_" + tag;
                        CreateDirectoryA(subdir.c_str(), nullptr);
                        out = subdir + "\\frame_%06d.png";
                    } else {
                        char filename[256]; _snprintf_s(filename, sizeof(filename), _TRUNCATE, "%s_%s.mp4", g_session_time, tag);
                        out = captures_dir(true) + "\\" + filename;
                    }
                    p.worker_stop = false; p.frame_count = 0; p.dropped_frames = 0;
                    // Fresh take -> fresh encoder verdict (the previous failure stays on screen
                    // until exactly here, so it survives the stop as a retrospective report).
                    p.enc_error[0] = '\0';
                    p.enc_claim.store(false, std::memory_order_relaxed); // re-arm enc_fail's ticket
                    p.enc_failed.store(false, std::memory_order_release);
                    // Set BEFORE the thread exists so there's no window where a just-spawned worker
                    // looks like an exited one to the diagnostic tick below.
                    p.worker_running = true;
                    p.worker = std::thread(worker_func, &p, out);
                }

                size_t wi = p.copy_index % 3;
                if (p.use_texture) {
                    cmd->copy_texture_region(tex, 0, nullptr, p.staging_buf[wi], 0, nullptr, filter_mode::min_mag_mip_point);
                } else {
                    cmd->copy_texture_to_buffer(tex, 0, nullptr, p.staging_buf[wi], 0, 0, 0);
                }
                p.copy_index++;
                q->signal(p.done_fence, p.copy_index);
            }
            q->flush_immediate_command_list(); // one submit for all passes' copies, not one per pass

            // Cadence fill: the game rendered slower than g_fps this interval (pad_frames = the beats
            // owed beyond the one fresh copy above), so emit the held frames we owe to keep the file's
            // frame count -> playback duration honest (a 10s take stays 10s). These are NOT drops: the
            // pipeline is healthy, the game just produced fewer unique frames than the target rate, so
            // they deliberately don't touch dropped_frames or the stability graph — a game-fps-limited
            // take stays green. Held frame = the last fully-read frame (the fresh copy issued just
            // above isn't read back yet). Passes stay frame-count-aligned (k outer, passes inner).
            for (int k = 0; k < pad_frames; ++k)
                for (auto& p : g_passes)
                    if (pass_is_live(p))
                        push_held_frame(p, MAX_DUP_QUEUE);
        }
    }

    // ---- PHASE 2: CPU Readback (Happens EVERY frame to drain GPU queue) ----
    for (auto& p : g_passes) {
        if (!p.active || p.read_index >= p.copy_index) continue;

        uint64_t target_val = p.read_index + 1;
        // Check if the GPU has finished THIS specific frame
        if (dev->wait(p.done_fence, target_val, 0)) {
            size_t ri = (target_val - 1) % 3;
            bool map_success = false;
            subresource_data out_data;
            void* mapped_ptr = nullptr;

            if (p.use_texture) {
                out_data = {}; // zero-init so a backend that leaves row_pitch unset is detectable
                map_success = dev->map_texture_region(p.staging_buf[ri], 0, nullptr, map_access::read_only, &out_data);
                mapped_ptr = out_data.data;
                // Guard: a valid pitch is always >= width*bpp. If the backend under-reports it
                // (0 or too small), copying width*bpp per row would read across rows / out of the
                // buffer -> swirled garbage. Clamp up to the dense pitch.
                if (out_data.row_pitch < p.width * p.bytes_per_px) out_data.row_pitch = p.width * p.bytes_per_px;
            } else {
                map_success = dev->map_buffer_region(p.staging_buf[ri], 0, UINT64_MAX, map_access::read_only, &mapped_ptr);
                out_data.row_pitch = p.row_pitch;
            }

            if (map_success) {
                // Still drained and unmapped for a failed pass (the copy was already in flight) —
                // just not queued: pass_is_live keeps frames out of a dead encoder's queue.
                const size_t padded_size = (size_t)out_data.row_pitch * p.height;
                if (g_recording && pass_is_live(p)) {
                    // Two ways this frame can't be committed: the queue is at its BYTE budget (the
                    // frame-count ceilings don't bound memory — one 4K frame is ~33 MB), or the
                    // allocation itself failed. Both take the true, uncompensated drop; the frame is
                    // still unmapped below either way, so the GPU pipeline keeps draining. Letting
                    // bad_alloc out instead would unwind through ReShade's dispatch into the game.
                    const bool over_budget =
                        p.queue_bytes.load(std::memory_order_relaxed) + padded_size > MPR_MAX_QUEUE_BYTES;
                    std::vector<uint8_t> buf;
                    if (!over_budget) buf = p.acquire_buf(padded_size);
                    if (buf.empty()) {
                        p.dropped_frames++;
                        g_drop_reason = over_budget ? "encoder can't keep up (queue memory cap)"
                                                    : "out of memory for the capture queue";
                    } else {
                        const uint8_t* src = static_cast<const uint8_t*>(mapped_ptr);
                        memcpy(buf.data(), src, padded_size);

                        // Refresh the duplicate-on-backpressure cache (see last_frame_buf / Phase 1).
                        // held_mutex: the stop joiner frees this same buffer from its own thread.
                        {
                            auto cache = p.acquire_buf(padded_size);
                            std::lock_guard<std::mutex> hk(p.held_mutex);
                            if (!cache.empty()) {
                                memcpy(cache.data(), buf.data(), padded_size);
                                if (p.has_last_frame) p.release_buf(std::move(p.last_frame_buf));
                                p.last_frame_buf = std::move(cache);
                                p.last_frame_w = p.width; p.last_frame_h = p.height;
                                p.last_frame_pitch = out_data.row_pitch; p.last_frame_bpp = p.bytes_per_px;
                                p.has_last_frame = true;
                            }
                        }

                        std::lock_guard<std::mutex> lock(p.q_mutex);
                        FrameData fd;
                        fd.buf = std::move(buf);
                        fd.width = p.width;
                        fd.height = p.height;
                        fd.src_pitch = out_data.row_pitch;
                        fd.bytes_per_px = p.bytes_per_px;
                        p.queue.push(std::move(fd));
                        p.queue_depth.fetch_add(1, std::memory_order_relaxed);
                        p.queue_bytes.fetch_add(padded_size, std::memory_order_relaxed);
                        p.q_cv.notify_one();
                    }
                }
                if (p.use_texture) dev->unmap_texture_region(p.staging_buf[ri], 0);
                else dev->unmap_buffer_region(p.staging_buf[ri]);
                p.read_index++;
            } else {
                reshade::log::message(reshade::log::level::warning, "MultipassRecorder: Map failed.");
            }
        }
    }

    // Stills last, AFTER both capture phases: a still must never push a capture beat later in the
    // frame than it would otherwise land. Its GPU copies are its own -- the recording is unaffected
    // beyond one extra copy on the single frame the shutter fires.
    shot_grab_tick(rt);
    shot_drain_tick(rt);

    g_live_frame++; // frame boundary for the live RT registry (mirrors the not-recording path)
}

