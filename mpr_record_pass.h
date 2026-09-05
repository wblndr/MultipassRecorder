// === MultipassRecorder — mpr_record_pass.h ===
// Core types & global state — FrameData, RecordPass, enums, g_passes[], HUD config, encode-label tables.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.

// ---- Frame data passed from render thread to worker ----
struct FrameData {
    std::vector<uint8_t> buf;        // raw pixel data (may include GPU row padding)
    uint32_t width = 0, height = 0;
    uint32_t src_pitch = 0;          // actual row pitch from GPU (may be > width*bpp)
    uint32_t bytes_per_px = 0;
};

// Where a pass sources its pixels from each frame:
//   SRC_NAMED     - a named ReShade .fx texture (find_texture_variable) [original behaviour]
//   SRC_ENGINE_RT - an arbitrary engine render target, matched by fingerprint against the
//                   live RT registry (format + min width + which match in bind order). Used
//                   for the native G-buffer normals; reassignable at runtime via the Buffers tab.
//   SRC_FX_LAYER  - a ReShade EFFECT rendered into this pass's OWN private render target, via
//                   effect_runtime::render_technique (see mpr_fxlayer.inc). Independent of the
//                   visible effect chain, so an effect's Debug View can be recorded as its own
//                   pass while the game view keeps its normal composited look. Added from the
//                   Effects tab.
//   SRC_NATIVE_*  - computed by the ADDON itself into its own target (mpr_native.inc), with no
//                   .fx doing the work. DEPTH and NORMALS are compute shaders reading the raw
//                   depth buffer that MultipassRecorder.fx reflects into texDepthRaw; WORLD is a
//                   straight copy of the pre-effect back buffer. These replaced the fat
//                   MultipassRecorder.fx, whose RGBA8 output quantized a 32-bit depth buffer to
//                   256 levels before the addon ever saw it.
// The engine kind is discovered by fingerprint (see g_live_* below), not by name.
// APPEND ONLY: custom slots persist this as an int under "<name>_src".
enum SourceKind { SRC_NAMED, SRC_ENGINE_RT, SRC_FX_LAYER,
                  SRC_NATIVE_DEPTH, SRC_NATIVE_NORMALS, SRC_NATIVE_WORLD };

// True for the three addon-computed sources, which share one target lifecycle (native_ensure_rt).
static inline bool src_is_native(int s) {
    return s == SRC_NATIVE_DEPTH || s == SRC_NATIVE_NORMALS || s == SRC_NATIVE_WORLD;
}

// What an FX-layer pass's private target holds before its technique runs. Most debug views
// overwrite the input entirely, but a compositing effect reads it: on a WHITE plate a
// multiplicative effect (AO) renders its own matte; on BLACK an additive one renders its
// contribution alone.
enum FxSeed : int { FXSEED_SCENE, FXSEED_WHITE, FXSEED_BLACK };

// How the engine binds a live buffer, tracked per registry entry and used to keep the two
// ordinal spaces separate when resolving a SRC_ENGINE_RT fingerprint (an RTV-fingerprinted pass
// must never resolve to a compute UAV that happens to share its format, and vice-versa).
enum LiveBind : uint8_t { LIVE_RTV = 1, LIVE_UAV = 2 }; // color render target / compute (UAV) output

// ---- Per-pass state ----
// Output encoding — HOW a pass is written to disk, independent of WHICH buffer it captures.
// These map to FFmpeg args in worker_func. Not every combination is valid; the UI greys out
// the impossible ones (H.264/NVENC has no 10-bit; PNG ignores bit-depth & chroma).
enum EncCodec  { ENC_HEVC, ENC_H264, ENC_PNG, ENC_CUSTOM };  // codec / container
enum EncBits   { BITS_8, BITS_10 };                          // video bit depth
enum EncChroma { CHROMA_444, CHROMA_420, CHROMA_LUMA };      // 4:4:4, 4:2:0, grayscale/luma
// How a packed-HDR source is shaped for a video codec. REC709 = SDR-range look, reads right on
// import. PQ / HDR10 = the standard HDR encoding (Rec.2100 PQ), one-click in any NLE — the default.
// LOG2 = fixed wide-range log2 for maximum info / cross-game consistency (advanced; needs the decode
// LUT). PQ and LOG2 imply HEVC 10-bit.
enum EncHdr    { HDR_REC709, HDR_PQ, HDR_LOG2 };

// Bounds for the native depth/normals tuning (mpr_native.inc). ONE definition, shared by the UI
// sliders in mpr_ui.inc and the load-time clamps in mpr_config.inc — see the note on the
// native_* fields below for why they drifted apart and what that cost.
//
// The two multipliers span six decades and are presented on LOGARITHMIC sliders. A linear slider
// over 0.001..1000 puts every value that actually works (order 0.1) inside the first pixel of
// travel: the control is nominally there, but the value cannot be dialled in, and the pass reads
// as broken when it is only mis-tuned. Keep these on ImGuiSliderFlags_Logarithmic.
static constexpr float NATIVE_FAR_MIN  = 1.0f,    NATIVE_FAR_MAX  = 100000.0f;
static constexpr float NATIVE_DMUL_MIN = 0.0001f, NATIVE_DMUL_MAX = 1000.0f;
static constexpr float NATIVE_NMUL_MIN = 0.0001f, NATIVE_NMUL_MAX = 100.0f;
static constexpr float NATIVE_FOV_MIN  = 1.0f,    NATIVE_FOV_MAX  = 170.0f;

struct RecordPass {
    // ---- Source (WHICH buffer) ----
    const char* name;
    const char* tex_name;
    const char* ffmpeg_fmt;    // raw input pix_fmt for 8-bit sources ("rgba")
    uint32_t    bytes_per_px;  // 4 = rgba
    SourceKind  source = SRC_NAMED;
    SourceKind  def_source = SRC_NAMED; // original source; fixed passes are forced back to this on load
    bool        is_custom = false;      // true = a promotable slot (added from the Buffers tab)
    bool        promoted  = false;      // true = this custom slot holds a live promoted buffer
    char        label[80] = "";         // display name for a promoted pass (e.g. "o02 r11g11b10 1920x1080")
    format      cap_format = format::unknown; // actual format being captured (set at worker spawn);
                                              // drives the r11g11b10->rgb48le unpack so a packed-HDR
                                              // engine RT assigned to any pass records correctly

    // ---- Output encoding (HOW it's written; see enums above) ----
    int  enc_codec    = ENC_HEVC;
    int  enc_bits     = BITS_8;
    int  enc_chroma   = CHROMA_444;
    int  enc_hdr      = HDR_REC709; // HDR curve for packed-HDR sources (Rec.709 / PQ-HDR10 / Log2)
    float enc_exposure = 0.0f;      // APPLIED exposure before the HDR curve, in stops. In AE mode this
                                    // is driven live by the meter (base + ev_comp) until recording
                                    // starts, then it's frozen (baked into the encoder LUT at spawn).
    // ---- Camera-style auto-exposure (AE) ----
    bool  ae_auto     = false;      // true: meter tracks the scene and drives enc_exposure while idle
    float ev_comp     = 0.0f;       // EV compensation dial (stops, snapped to 1/3), added on top of AE
    bool enc_lossless = true;
    int  enc_cq       = 18;      // constant-quality (NVENC -cq) when not lossless
    int  enc_preset   = 1;       // NVENC preset 1..7. NOT a quality knob: lossless is pixel-identical at
                                 // any speed and constqp holds the same QP - slower presets only compress
                                 // better (smaller files) at more GPU cost. p1 default = fewest drops.

    // For SRC_ENGINE_RT: fingerprint of the engine render target to capture, resolved against
    // the live RT registry each frame (RTs are transient handles, so identified by properties,
    // not addresses). rt_ordinal = which match among same-format >=rt_min_w RTs, in bind order.
    // Defaults target Warframe's native G-buffer normals; reassignable via the Buffers tab.
    format      rt_fmt     = format::unknown;
    uint32_t    rt_min_w   = 0;
    int         rt_ordinal = 0;
    uint8_t     rt_bind    = LIVE_RTV; // resolve the fingerprint against RTV- or UAV-bound entries only

    // ---- For SRC_FX_LAYER: which ReShade technique to render, and into what ----
    // Identified by NAME, not handle: effect_technique handles are invalidated on every effect
    // reload, so mpr_fxlayer.inc re-resolves this pair each frame (fx_find_technique).
    char        fx_effect[96] = "";   // source .fx file name, e.g. "qUINT_mxao.fx"
    char        fx_tech[96]   = "";   // technique name, e.g. "MXAO"
    // Debug-View override: this uniform is set to fx_dbg_val for OUR render only, then restored,
    // so the pass records the effect's debug output while the visible chain keeps the user's value.
    bool        fx_dbg_on     = false;
    char        fx_dbg_var[96] = "";  // uniform variable name ("" = no override)
    int         fx_dbg_val    = 0;
    int         fx_seed       = FXSEED_SCENE; // what the private target holds before the technique runs

    // The private layer target (created lazily to match the back buffer; see fx_ensure_rt).
    resource      fx_rt  = {};
    resource_view fx_rtv = {};
    resource_view fx_srv = {};
    uint32_t      fx_w = 0, fx_h = 0;
    format        fx_fmt = format::unknown;
    bool          fx_fmt_ok = true;   // false = back buffer format the encode path can't write
    // Frame stamp of the last successful layer render; on_reshade_present compares it against the
    // current frame to report (reactively) that a recording FX pass went stale.
    uint64_t      fx_last_render_frame = 0;

    // ---- For SRC_NATIVE_*: the addon's own computed target (mpr_native.inc) ----
    // Same shape as the fx_* block above, but the target is written by a compute dispatch (depth /
    // normals, hence a UAV) or a plain copy (world), never by render_technique — so there is no RTV.
    resource      native_rt  = {};
    resource_view native_uav = {};
    resource_view native_srv = {};
    uint32_t      native_w = 0, native_h = 0;
    format        native_fmt = format::unknown;
    uint64_t      native_last_render_frame = 0;

    // ---- Native DEPTH controls (were MultipassRecorder.fx uniforms; now ours, and persisted) ----
    // fFarPlane / fdepthMultiplier / bReversed / bUpsideDown / bDepthLogarithmic / bDepthReverseLuma
    // in the old shader. Two of these never had an addon UI at all before.
    //
    // BOUNDS LIVE IN ONE PLACE (below, NATIVE_*_MIN/MAX) because the UI sliders and the load-time
    // clamps in mpr_config.inc must agree: a clamp tighter than a slider silently rewrites what the
    // user set, and a slider tighter than a clamp makes a legal saved value unreachable. The second
    // one already happened — the normals multiplier was clamped and slidden to a 0.01 floor while
    // the shipped .fx calibration for at least one game was 0.001, so that game's own working value
    // could not be typed back in at any slider position.
    float native_far_plane    = 1000.0f;
    float native_depth_mul    = 1.0f;
    bool  native_reversed     = false;
    bool  native_upside_down  = false;
    bool  native_log_curve    = false;
    bool  native_reverse_luma = false;
    // Emit true scene-linear distance instead of the 0..1 remap — nothing baked in, the same
    // archival stance the EXR still path takes with exposure. Costs the 0..1 convenience.
    bool  native_linear_output = false;

    // ---- Native NORMALS controls (were fVerticalFOV / fNormalMultiplier / bUseFOVCalibration /
    // bFlipnormalX|Y|Z). The normals pass re-linearizes depth per tap using the depth fields above,
    // exactly as the old Getnormal() did — it does NOT read the depth pass's output. ----
    float native_fov        = 60.0f;
    float native_normal_mul = 1.0f;
    bool  native_use_fov    = true;
    bool  native_flip_x = false, native_flip_y = false, native_flip_z = false;

    // Enabled toggle
    bool enabled = false;

    // UI: this pass's encoding-details block is expanded in the pass table (multi-open,
    // session-only — not persisted).
    bool ui_expanded = false;

    // Derived from texture each frame
    bool     active = false;
    uint32_t width = 0, height = 0;
    uint32_t row_pitch = 0;



    // Staging readback (triple buffered for performance)
    resource staging_buf[3] = {};
    uint64_t copy_index = 0; // Number of copies initiated
    uint64_t read_index = 0; // Number of copies successfully read back
    fence    done_fence = {};

    // Worker thread
    std::thread              worker;
    std::mutex               q_mutex;
    std::condition_variable  q_cv;
    std::queue<FrameData>    queue;
    // Mirrors queue.size(), maintained alongside every push/pop. Readers that only want the
    // depth (OSD, stats panel, drop-threshold check) load this instead of locking q_mutex —
    // that mutex is also taken by the worker's pop on every frame, so a lock-free read here
    // avoids contending with the recording hot path just to render a number.
    std::atomic<size_t>      queue_depth{ 0 };
    // Total bytes held by the queued frames, maintained alongside queue_depth on every push/pop.
    // The frame count alone is not a bound: at 4K RGBA one frame is ~33 MB, so the dup ceiling
    // (MAX_DUP_QUEUE) would let a sustained overload commit ~10 GB per pass. Producers check this
    // against MPR_MAX_QUEUE_BYTES and take the true-drop path instead of allocating.
    std::atomic<size_t>      queue_bytes{ 0 };
    bool                     worker_stop = false;
    std::atomic<uint32_t>    frame_count{ 0 };
    std::atomic<uint32_t>    dropped_frames{ 0 };
    // Set on the RENDER thread immediately before the worker thread is created, cleared by the
    // worker as its last act. So `worker.joinable() && !worker_running` unambiguously means "the
    // worker exited on its own" — no spawn race, no timing guard. The frame loop's diagnostic tick
    // uses it as the catch-all for any silent worker exit (see on_reshade_present).
    std::atomic<bool>        worker_running{ false };

    // ---- Encoder failure (worker -> UI, reactive) ----
    // The pass's FFmpeg child can die at any point: never start at all (wrong ffmpeg path), or
    // exit seconds in (NVENC's concurrent-session cap — every pass opens its own session — an
    // unsupported pixel format, a full disk). Either way the pass silently writes nothing while
    // the rest of the pipeline keeps reporting a healthy recording, and its queue then fills and
    // trips the GLOBAL frame lock, so one dead encoder drops frames on every other pass too.
    // The worker records what happened here; pass_is_live() below takes the pass out of the
    // capture path, and the panel/OSD report it naming the pass (never predictively).
    // Written once before enc_failed flips with release ordering; readers load the flag with
    // acquire, so the message is complete by the time they see it. See enc_fail for why its two
    // callers (worker, render-thread catch-all) can never overlap on enc_error.
    char                     enc_error[192] = "";
    std::atomic<bool>        enc_failed{ false };
    // Claim ticket for enc_fail: whoever wins the exchange writes enc_error, every later caller
    // returns. enc_failed can't double as the claim — it must only flip once the message is
    // complete. Needed because enc_fail has callers that genuinely can overlap: the pass's own
    // worker, and the render thread's mid-take source-resize check in check_and_update_staging.
    std::atomic<bool>        enc_claim{ false };

    // Buffer pool: recycle allocations instead of malloc/free every frame
    std::mutex               pool_mutex;
    std::vector<std::vector<uint8_t>> free_bufs;


    // Returns an EMPTY vector when the allocation fails instead of letting bad_alloc escape: a
    // frame is ~33 MB at 4K and the callers run on the RENDER thread, so an unwind would go out
    // through ReShade's event dispatch and take the game with it. Every caller treats an empty
    // buffer as "drop this frame".
    std::vector<uint8_t> acquire_buf(size_t needed) {
        std::lock_guard<std::mutex> lk(pool_mutex);
        try {
            if (!free_bufs.empty()) {
                auto b = std::move(free_bufs.back());
                free_bufs.pop_back();
                b.resize(needed); // no-op if capacity >= needed
                return b;
            }
            return std::vector<uint8_t>(needed);
        } catch (const std::bad_alloc&) {
            return std::vector<uint8_t>();
        }
    }
    void release_buf(std::vector<uint8_t>&& b) {
        std::lock_guard<std::mutex> lk(pool_mutex);
        if (free_bufs.size() < 6) // keep at most 6 buffers in pool
            free_bufs.push_back(std::move(b));
    }

    // Cache of the last successfully captured frame. Raw video has no per-frame timestamp — FFmpeg
    // infers duration purely from frame count at the fixed -r fps — so when backpressure would
    // otherwise force a silently-skipped frame, on_reshade_present re-pushes this cached frame
    // instead: the recording holds on an image for a beat rather than losing real time (which would
    // make the export play back faster than real-time).
    // Guarded by held_mutex: the render thread refreshes it (Phase 2) and reads it (push_held_frame),
    // but the STOP path frees it from its background joiner thread, and those two moving out of the
    // same vector is a double free.
    std::mutex           held_mutex;
    std::vector<uint8_t> last_frame_buf;
    uint32_t             last_frame_w = 0, last_frame_h = 0, last_frame_pitch = 0, last_frame_bpp = 0;
    bool                 has_last_frame = false;

    // Return the held frame to the pool. Called by the stop joiner and by destroy_pass_resources —
    // after a source resize the cached geometry no longer matches what FFmpeg was told to expect,
    // and pushing it would byte-shift the rest of the file into garbage.
    void drop_last_frame() {
        std::lock_guard<std::mutex> lk(held_mutex);
        if (!has_last_frame) return;
        release_buf(std::move(last_frame_buf));
        has_last_frame = false;
    }

    char                     custom_args[1024] = ""; // used when enc_codec == ENC_CUSTOM
    bool                     use_texture = false;

    void init(const char* n, const char* tn, const char* ff, uint32_t bpp,
              SourceKind src = SRC_NAMED) {
        name = n;
        tex_name = tn;
        ffmpeg_fmt = ff;
        bytes_per_px = bpp;
        custom_args[0] = '\0';
        use_texture = false;
        worker_running = false;
        source = src;
        def_source = src;
        // Encoding defaults (overridable per pass in AddonInit): lossless 8-bit HEVC 4:4:4.
        enc_codec = ENC_HEVC; enc_bits = BITS_8; enc_chroma = CHROMA_444; enc_hdr = HDR_REC709; enc_exposure = 0.0f; enc_lossless = true; enc_cq = 18;
    }
};

// "This pass is currently taking frames": enabled, its worker exists, and its encoder hasn't
// died. Every producer-side loop in the frame path (backpressure scan, GPU copy, held-frame
// padding) tests this instead of `enabled && worker.joinable()`, so a pass whose FFmpeg exited
// stops being fed — otherwise its queue climbs to the drop threshold and the global frame lock
// makes one dead encoder cost every other pass its frames. Consumers that only REPORT (the stats
// table) deliberately still list a failed pass, marked, rather than dropping the row.
static inline bool pass_is_live(const RecordPass& p) {
    return p.enabled && p.worker.joinable() && !p.enc_failed.load(std::memory_order_acquire);
}

// 4 fixed built-in passes (indices 0-3) + a pool of promotable custom slots. The pool is shared by
// BOTH promotion routes — engine buffers (Buffers tab) and ReShade effects (Effects tab) — so it's
// sized generously enough that adding effect layers can't crowd out buffer promotions. Slot names
// are ini keys, so rt1..rt8 must keep their meaning when the pool grows.
static constexpr int N_FIXED  = 4;
static constexpr int N_CUSTOM = 12;
static RecordPass g_passes[N_FIXED + N_CUSTOM];
static const char* CUSTOM_NAMES[N_CUSTOM] = { "rt1", "rt2", "rt3",  "rt4",  "rt5",  "rt6",
                                              "rt7", "rt8", "rt9", "rt10", "rt11", "rt12" };

// Hard ceiling on the pixel data ONE pass may hold in its worker queue. The frame-count ceilings
// (the 60-frame backpressure trip, MAX_DUP_QUEUE) don't bound memory: at 4K RGBA a frame is ~33 MB,
// so 300 queued frames is ~10 GB per pass. Deliberately a constant and not a setting — it's a
// crash guard, not a knob. Producers over budget take the true-drop path instead of allocating.
static constexpr size_t MPR_MAX_QUEUE_BYTES = 512ull * 1024 * 1024;

// ---- Stop rendezvous --------------------------------------------------------------------------
// Stopping a take joins the pass workers on a BACKGROUND thread so the render thread never blocks
// on FFmpeg's shutdown (that join costs seconds). The handle is tracked, not detached: the joiner
// owns each pass's worker handle, queue and held-frame cache, all of which the render thread also
// touches. So stopping still doesn't block — but every LATER lifecycle transition (starting the
// next take, runtime teardown, AddonUninit) must rendezvous here first, or it races the joiner
// into a double join / double free.
static std::thread g_stop_joiner;
static void stop_join_wait() { if (g_stop_joiner.joinable()) g_stop_joiner.join(); }


static bool     g_recording       = false;
static int      g_fps             = 30;
static std::chrono::high_resolution_clock::time_point g_next_capture_time; // wall-clock time the next capture beat is due (fixed cadence; see on_reshade_present)
static std::chrono::high_resolution_clock::time_point g_record_start_time;
static const char* g_version      = "v0.5.0";
static char     g_session_time[160]= "1970-01-01 00-00-00"; // "<game>_<date> <time>" once recording starts, see toggle_recording
// Hotkeys (record start/stop, Debug View, ...) live in mpr_hotkeys.inc — one bindable action per
// table row, all rebindable in the Setup tab and persisted per action.
static char     g_ffmpeg_path[512]= "C:\\Program Files\\FFmpeg\\bin\\ffmpeg.exe";
static char     g_output_dir[512] = "";
static bool     g_audio_enabled  = false;
// Grey "camera UI" chrome theme for the addon's own ImGui windows (see MprThemeScope,
// mpr_theme.inc) vs. ReShade's default panel look. Defaults on; the Setup-tab checkbox is a
// one-click, no-rebuild fallback to the stock look.
static bool     g_ui_theme_camera = true;
static char     g_audio_input_args[512] = "-f dshow -i audio=\"virtual-audio-capturer\"";
static HANDLE   g_audio_proc     = nullptr;
static DWORD    g_audio_pid      = 0;       // FFmpeg PID — target for the Ctrl-Break stop signal
static HANDLE   g_audio_stdin    = nullptr; // write end of stdin pipe — write "q\n" as a fallback stop

// Job object that tethers every FFmpeg child to this (game) process's lifetime.
// With KILL_ON_JOB_CLOSE, if the game is force-killed or crashes (so AddonUninit
// never runs), the OS closes this handle and terminates all FFmpeg children. This
// is the only thing that stops the AUDIO encoder on an abnormal exit: its input is
// a dshow device, independent of the game, so unlike the video encoders (which see
// EOF on their stdin pipe when the game dies) it would otherwise record forever.
static HANDLE   g_ffmpeg_job     = nullptr;

// Lazily create the kill-on-close job and assign a freshly-spawned FFmpeg process to it.
// call_once, not a bare null check: this runs on EVERY pass's worker thread and Phase 1 spawns them
// all in one loop, so the first multi-pass take races two threads through the creation. One job
// handle would be overwritten and leaked, and the children assigned to the lost job would no longer
// be tethered to the game (an orphaned audio encoder recording forever after a crash).
static std::once_flag g_ffmpeg_job_once;
static void tether_ffmpeg_to_game(HANDLE proc) {
    std::call_once(g_ffmpeg_job_once, []() {
        g_ffmpeg_job = CreateJobObjectA(nullptr, nullptr);
        if (g_ffmpeg_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_ffmpeg_job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        }
    });
    if (g_ffmpeg_job)
        AssignProcessToJobObject(g_ffmpeg_job, proc); // harmless if it fails; clean-shutdown paths still stop FFmpeg
}

// ---- HUD Configuration ----
struct HUDConfig {
    // Recording badge: pulsing dot + "REC" + "| N passes" collapse into one toggle (they were
    // 4 separate switches for what is visually a single status line).
    bool show_badge       = true;
    bool show_duration    = true;   // wall-clock timer (kept separate — the one badge sub-option worth it)
    // Diagnostics
    bool show_graph       = true;
    bool show_stats       = true;   // per-pass fps/dropped/queue telemetry on the OSD
    bool show_ev_meter    = true;   // live camera-style exposure meter (HDR passes only)
    float rec_color[3]    = { 1.0f, 0.2f, 0.2f };
    float warn_color[3]   = { 1.0f, 0.3f, 0.3f };
    // Composition guides (screen-only, never captured)
    bool show_crosshair   = false;
    bool show_thirds      = false;
    bool show_letterbox   = false;
    int  letterbox_idx    = 0;       // index into LETTERBOX_RATIOS
    float guide_color[4]  = { 1.0f, 1.0f, 1.0f, 0.40f };  // RGBA for crosshair/thirds lines
    float lbox_alpha      = 0.80f;   // opacity of letterbox bars
};
static HUDConfig g_hud;

// ---- Stability Graph History ----
static constexpr int STABILITY_HISTORY_SIZE = 120; // ~2 seconds at 60fps
static float g_stability_history[STABILITY_HISTORY_SIZE] = {};
static int   g_stability_idx = 0;
static uint32_t g_prev_total_frames = 0;
static uint32_t g_prev_total_drops  = 0;

// Labels for the encoding dropdowns (indices match the EncCodec/EncBits/EncChroma enums).
const char* ENC_CODEC_LABELS[]  = { "HEVC (NVENC)", "H.264 (NVENC)", "PNG sequence (16-bit)", "Custom FFmpeg args" };
const char* ENC_BITS_LABELS[]   = { "8-bit", "10-bit" };
const char* ENC_CHROMA_LABELS[] = { "4:4:4 (full color)", "4:2:0 (subsampled)", "Luma (grayscale)" };
const char* ENC_HDR_LABELS[]    = { "Rec.709 (SDR view)", "PQ / HDR (Rec.709 primaries) - recommended", "Log2 (wide range, max info)" };

