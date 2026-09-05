// === MultipassRecorder — mpr_state.inc ===
// Shared runtime state — live-RT registry, per-game profiles, preview/scope globals, time/path helpers.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.

// ---------------------------------------------------------------------------
// Live RT registry — a continuous, low-cost version of the F8 probe. It keeps a
// per-frame list of every render target the game binds, plus a cached shader-
// resource view for each, so the settings overlay can show live thumbnails (the
// Buffers gallery) and let the user assign any engine buffer to a capture pass
// (SRC_ENGINE_RT). Populated from the shared bind/draw hooks whenever the panel
// is open or an engine-RT pass is recording.
//
// THREADING. On DX11 every bind/draw/present arrives on the immediate-context
// thread, and the registry was written for exactly that. D3D12 titles (BO6)
// record command lists on several worker threads, so the bind hooks fire
// concurrently and the registry needs real protection. The rules:
//   - g_live_mutex serialises every MUTATION (live_touch, eviction, teardown).
//   - The vector is reserve()d to its 64-slot cap up front and slots are BLANKED
//     rather than erased, so no element ever moves. That is what lets the overlay
//     hold LiveRT* across a frame's draw, and read them, without taking the lock.
//   - A new slot publishes `handle` LAST; eviction retracts `handle` FIRST. Every
//     reader skips handle == 0, so a half-built or half-blanked slot is invisible
//     instead of showing as a junk row -- or, far worse, handing the capture path
//     a live-looking handle sitting beside an already-zeroed desc/last_frame.
//     `handle` and `last_frame` are the two fields those lockless readers test, so
//     they carry that ordering in the TYPE (AtomicField below): a comment cannot
//     stop the compiler from sinking a plain store past its neighbours.
// Not fixed by any of this: g_live_current (draw attribution) is one global for
// what is really per-command-list state, so `draws` counts are approximate on a
// multi-threaded recorder. It is a hint in a tooltip, not something a pass binds
// to, so it is left as-is rather than paying for per-command-list tracking.
// ---------------------------------------------------------------------------

// A registry field read WITHOUT g_live_mutex. std::atomic is neither copyable nor movable and
// LiveRT lives in a std::vector that is assigned whole (`slot = LiveRT{}` is how a slot is
// blanked), so the ordering is packaged here rather than spelled at every use: the field still
// reads and writes like a plain value, and every copy it makes happens under the lock.
// publish()/retract() are the two halves of the slot protocol above. Plain assignment is a
// release store, which is exactly right for publishing (nothing built before it can sink past
// it) but says nothing about the stores that FOLLOW -- so retract() is a full barrier, which is
// what keeps the blanking after it from being hoisted above the handle going to 0.
template <typename T> struct AtomicField {
    std::atomic<T> a;
    AtomicField(T x = T{}) : a(x) {}
    AtomicField(const AtomicField& o) : a(o.a.load(std::memory_order_relaxed)) {}
    AtomicField& operator=(const AtomicField& o) {
        a.store(o.a.load(std::memory_order_relaxed), std::memory_order_release); return *this;
    }
    AtomicField& operator=(T x) { a.store(x, std::memory_order_release); return *this; }
    operator T() const { return a.load(std::memory_order_acquire); }
    void publish(T x) { a.store(x, std::memory_order_release); }
    void retract()    { a.store(T{}, std::memory_order_seq_cst); }
};

struct LiveRT {
    AtomicField<uint64_t> handle;   // 0 = free slot (evicted buffers are BLANKED, never erased)
    resource_desc desc   = {};
    uint32_t      seq    = 0;       // first-seen sequence, assigned once and never re-assigned
    int           order  = 0;       // de-jittered bind order (see live_touch)
    int           order_raw = 0;    // bind order observed THIS frame (jitters, see below)
    uint8_t       order_hold = 0;   // consecutive frames order_raw has agreed with itself
    uint32_t      binds  = 0, draws = 0;
    AtomicField<uint32_t> last_frame; // g_live_frame value when last bound
    resource_view srv    = { 0 };   // cached SRV for the ImGui preview thumbnail
    bool          srv_tried = false;// attempted SRV creation (don't retry failures each frame)
    uint8_t       bound_as = 0;     // bitmask of LiveBind: seen as a render target and/or a compute UAV
    // Last known "this buffer is already a capture pass" answer. Fingerprints only resolve on
    // frames the buffer is actually bound, so the gallery refreshes this while the row is fresh
    // and reuses it while idle -- otherwise the "Already added" badge blinks off and the row
    // offers to add a second pass for a buffer that already has one.
    RecordPass*   dup_pass = nullptr;
};
static std::vector<LiveRT> g_live_rts;
static std::mutex g_live_mutex;                // guards registry mutation AND every srv field (see THREADING above)
static uint32_t g_live_seq_ctr = 0;            // monotonic slot-creation counter (the gallery's sort key)

// ---- Retired thumbnail SRVs -------------------------------------------------------------------
// An SRV handed to ImGui::Image is NOT sampled inside our draw callback: it goes into a draw list
// ReShade renders at the end of the frame. Destroying one the instant the game frees its resource
// therefore leaves the overlay sampling a destroyed descriptor — on D3D12 a GPU page fault, which
// surfaces as a device-removed hang rather than a clean crash, so it never looks like our bug.
// Evicted views are parked here instead and destroyed once no in-flight draw list can still name
// them. Guarded by g_live_mutex, like the registry itself.
struct RetiredView {
    resource_view view  = { 0 };
    uint32_t      frame = 0;                   // g_live_frame when it was retired
};
static std::vector<RetiredView> g_live_retired;
static const uint32_t LIVE_RETIRE_FRAMES = 4;  // presents to hold a view before destroying it

// ---- Gallery stability windows ----------------------------------------------------------------
// A buffer's bind order is NOT stable frame to frame: engines skip conditional passes (shadow
// cascade updates, SSR, bloom, a probe refresh) and D3D12 titles record command lists on several
// worker threads, so the same buffer can be the 7th target bound one frame and the 5th the next.
// Sorting the gallery on that number made every row hop around, and buffers bound every Nth frame
// vanished and came back. Rows are therefore sorted on `seq` (stable for the session) and kept
// listed for LIVE_LIST_FRAMES after their last bind, dimmed once they go stale rather than
// disappearing; the displayed bind order only moves once it has held still for LIVE_ORDER_HOLD
// frames. Nothing here widens what capture/fingerprint resolution considers live -- those still
// use LIVE_FRESH_FRAMES, so a pass can never bind to a buffer the game stopped drawing.
static const uint32_t LIVE_FRESH_FRAMES = 2;   // "the game is drawing into this right now"
static const uint32_t LIVE_LIST_FRAMES  = 45;  // stays in the gallery this long after its last bind (~0.5s)
static const uint8_t  LIVE_ORDER_HOLD   = 8;   // frames a bind order must agree with itself before it's shown
// Enumeration active (panel open, recording an engine pass, still pending). Atomic because the
// bind/draw hooks that read it are permanently registered and fire on D3D12 worker threads, while
// present writes it -- a plain bool there is a data race, and it is the ONE thing those hooks look
// at before returning. A plain mov on x86, so the always-registered idle cost stays a flag test.
static std::atomic<bool> g_live_enum{ false };
// Bookkeeping only: these events are registered ONCE and never unregistered (see the hook block
// at the bottom of mpr_livert.inc for why toggling them at runtime was a D3D12 crash).
static bool     g_push_desc_hooked = false;    // push_descriptors event registered
static bool     g_scene_hooks_hooked = false;   // bind_render_targets/draw/draw_indexed registered
// Bumped once per present (frame boundary). Atomic for the same reason as g_live_enum: live_touch
// reads it from whichever worker thread recorded the bind, and every freshness test in the
// registry is arithmetic against it. Increments/reads spell the same as a plain uint32_t.
static std::atomic<uint32_t> g_live_frame{ 0 };
static uint32_t g_live_seen_frame= 0xffffffff; // frame we last reset per-frame ordering for
static int      g_live_order_ctr = 0;
static int      g_live_current   = -1;         // primary RT index this frame (draw attribution)
static int      g_settings_ttl   = 0;          // frames since either overlay panel was last drawn
static uint32_t g_overlay_frame  = 0xffffffff;  // g_live_frame value on which an overlay panel was drawn THIS
                                                // present (i.e. the ReShade menu is genuinely open right now).
                                                // Distinct from g_settings_ttl's 3-frame countdown: docking a
                                                // window requires ReShade's dockspace host to be submitted the
                                                // SAME frame, so the menu-gated Viewfinder keys off this
                                                // exact-frame stamp, not the loose TTL (see draw_buffer_preview).

// ReShade overlay window title (registered via register_overlay in AddonInit): ONE tabbed
// settings window -- Record | Buffers | Overlays | Setup -- that shows/hides with the ReShade
// "Home" overlay (position/collapse persisted by ReShade). The Buffers tab gives the tall
// engine-buffer gallery the full window height. The floating Viewfinder is deliberately NOT a
// tab: it stays visible beside the panel while you edit encode settings in the Record tab.
static const char* const MPR_MAIN_TITLE = "MultipassRecorder";

// Panel tabs. g_ui_active_tab persists (the panel reopens on the tab you left off);
// g_ui_goto_tab is a session-only programmatic switch (e.g. "+ Add from Buffers"), -1 = none.
// NOTE: these values are persisted (active_tab), so only APPEND — inserting a tab in the middle
// would silently reopen everyone's panel on a different tab.
enum MprTab { TAB_RECORD = 0, TAB_BUFFERS, TAB_OVERLAYS, TAB_SETUP, TAB_EFFECTS, TAB_STILLS };
static const int TAB_MAX = TAB_STILLS; // highest valid value — the load-time clamp keys on this
static int g_ui_active_tab = TAB_RECORD;
static int g_ui_goto_tab   = -1;

// ---------------------------------------------------------------------------
// Per-game profiles. The engine pass (normals_native) reads a raw engine render
// target, so its fingerprint is game-specific. This table — keyed by exe name,
// same philosophy as hot_reload.py's GAMES dict — provides good DEFAULTS for
// known games so they work on first launch; anything the user changes (Buffers
// tab) then persists to that game's own ReShade.ini (config is per-game). One
// binary: universal .fx passes (depth/normals/world) work on ANY game with no
// profile; engine passes just fall back to "unassigned" until a profile or the
// Buffers tab supplies a fingerprint.
// ---------------------------------------------------------------------------
struct GameProfile {
    const char* exe_match;   // lowercase substring matched against the host exe filename
    const char* name;        // display name
    format      nrm_fmt;     // native G-buffer normals fingerprint (format / min width / ordinal)
    uint32_t    nrm_min_w;
    int         nrm_ordinal;
};
static const GameProfile PROFILES[] = {
    // Warframe (OpenWF offline DX11): native normals = 2nd full-res r8g8b8a8_typeless in bind
    // order (idx03 albedo, idx04 normals).
    { "warframe", "Warframe", format::r8g8b8a8_typeless, 1280, 1 },
};
// Unknown game: universal passes only. Native normals start UNASSIGNED (rt_fmt=unknown never
// resolves) until set via the Buffers tab.
static const GameProfile PROFILE_DEFAULT =
    { "", "none (universal passes only)", format::unknown, 0, 0 };
static GameProfile g_profile   = PROFILE_DEFAULT;
static char        g_exe_name[128] = "";
static char        g_game_tag[128] = ""; // g_exe_name with ".exe" stripped -- the output filename prefix

// Viewfinder sources -- a raw engine buffer (from the Buffers gallery) or a pass -- and the
// result string from the last one-button auto-profile scan.
static uint64_t    g_preview_handle = 0;      // engine buffer being previewed (LiveRT handle)
static RecordPass* g_preview_pass = nullptr;  // OR a pass being previewed (universal/engine/promoted)
static RecordPass* g_meter_pass   = nullptr;  // HDR pass driving the live OSD exposure meter while recording
static bool        g_preview_open   = false;
static bool        g_preview_fs     = false;
// Index into g_passes of the last pass shown in the Viewfinder. Persisted with g_preview_open
// so the viewfinder reopens on the same pass next session (see load/save_addon_config). -1 = none.
static int         g_preview_last_idx = -1;
// Debug View: project the previewed PASS full-screen directly over the game itself (background
// draw list), independent of whether the ReShade menu is open -- like a renderer's built-in
// buffer-visualization mode. Session-only (not persisted): a "look now" toggle, not a saved
// preference. Only ever meaningful alongside a non-null g_preview_pass; see draw_debug_view
// (mpr_osd.inc) and the "Debug View" controls in mpr_ui.inc.
static bool        g_debug_view     = false;
// Master mute for the addon's own on-screen furniture (recording OSD, exposure meter, composition
// guides) — the "clean screen" hotkey, HK_OVERLAYS. Session-only for the same reason g_debug_view
// is: a persisted "everything hidden" reads as a broken addon on the next launch. Debug View is
// deliberately NOT gated by it (it has its own key, and it IS the thing you asked to see).
static bool        g_osd_hidden     = false;
static std::string g_scan_report;
static std::string g_fx_report;   // same, for the Effects tab (add/remove result + reactive warnings)

// ---- Still capture (mpr_shot.inc) ----
// The frame every source must be grabbed on for a still. Declared HERE, rather than with the rest
// of the still machinery, because mpr_fxlayer.inc is included earlier and has to force its layer
// render on exactly that frame -- an effect layer rendered a frame late would put the AO still out
// of register with the beauty still, which is the one thing a multipass still has to get right.
// 0xffffffff = no still pending.
static uint32_t    g_shot_capture_frame = 0xffffffff;
// A still is pending (self-timer running, or a burst mid-flight). Keeps the live-RT registry
// enumerating for the duration: an SRC_ENGINE_RT pass resolves its fingerprint against the fresh
// window, so without this a still of an engine buffer taken with the panel closed finds nothing.
static bool        g_shot_pending = false;

// Still output formats. The stored setting is the ShotFmt VALUE, never a combo index, so
// reordering a menu can't silently repoint an existing ini at a different format.
enum ShotFmt : int {
    SHOTF_PNG8 = 0,   // 8-bit PNG (rgb24)
    SHOTF_PNG16,      // 16-bit PNG — unorm sources exact, float sources in HDR_PNG_SCALE fixed point
    SHOTF_JPEG,       // MJPEG 4:4:4
    SHOTF_TIFF16,     // 16-bit TIFF, LZW
    SHOTF_EXR_HALF,   // OpenEXR, 16-bit half, scene-linear
    SHOTF_EXR_FLOAT,  // OpenEXR, 32-bit float, scene-linear
};
static const int   SHOT_LDR_CHOICES[] = { SHOTF_PNG8, SHOTF_PNG16, SHOTF_TIFF16, SHOTF_JPEG };
static const char* SHOT_LDR_LABELS[]  = { "PNG 8-bit", "PNG 16-bit", "TIFF 16-bit", "JPEG" };
static const int   SHOT_HDR_CHOICES[] = { SHOTF_EXR_HALF, SHOTF_EXR_FLOAT, SHOTF_PNG16 };
static const char* SHOT_HDR_LABELS[]  = { "EXR 16-bit half  (linear)", "EXR 32-bit float  (linear)",
                                          "PNG 16-bit  (scaled linear)" };
static const char* SHOT_EXR_COMP[]        = { "none", "rle", "zip1", "zip16" };  // FFmpeg -compression values
static const char* SHOT_EXR_COMP_LABELS[] = { "None (fastest, biggest)", "RLE", "ZIP1", "ZIP16 (default)" };
// Is this value one the menu can actually offer? (Load-time guard, see load_addon_config.)
static bool shot_fmt_in(const int* choices, int n, int v) {
    for (int i = 0; i < n; ++i) if (choices[i] == v) return true;
    return false;
}
// When a shot gets its own folder.
enum ShotFolder : int { SHOTDIR_MULTI = 0, SHOTDIR_ALWAYS, SHOTDIR_NEVER };
static const char* SHOT_FOLDER_LABELS[] = { "Only when a shot writes several files",
                                            "Always", "Never (flat in stills\\)" };

struct ShotConfig {
    bool src_screen  = true;          // grab the game view
    bool src_passes  = true;          // grab every enabled capture pass
    int  screen_fmt  = SHOTF_PNG8;    // format for the screen grab (when it's an 8-bit/unorm buffer)
    int  ldr_fmt     = SHOTF_PNG8;    // ...for unorm pass buffers
    int  hdr_fmt     = SHOTF_EXR_HALF;// ...for float pass buffers
    bool hdr_preview = true;          // write a viewable tonemapped PNG beside each EXR
    int  exr_comp    = 3;             // index into SHOT_EXR_COMP
    int  jpeg_q      = 2;             // FFmpeg -q:v, 1 best .. 31 worst
    int  timer_sec   = 0;             // self-timer before the first frame
    int  burst       = 1;             // frames per press
    int  burst_ms    = 250;           // spacing between burst frames
    int  folder      = SHOTDIR_MULTI;
    bool sidecar     = true;          // write a .txt describing what each file is
    bool flash       = true;          // screen flash + toast confirmation
};
static ShotConfig g_shotcfg;

// Measured length of the last game frame, in microseconds. Published by on_reshade_present and read
// by the FX-layer hook (mpr_fxlayer.inc), which runs EARLIER in the same frame and has to predict
// whether present is about to declare a capture beat -- rendering a frame's effect layer one beat
// late would put the layer out of register with the other passes.
static long long   g_frame_dt_us  = 16666;

// ---- Live preview capture + exposure scope ----------------------------------------
// The old preview handed ImGui the RAW engine SRV, so a linear/HDR buffer looked black
// and the Exposure slider changed nothing you could see. Instead, while the preview
// window is open the viewfinder pipeline (mpr_preview.inc) processes the previewed
// buffer with the SAME exposure gain the recorder uses and builds two things:
//   (a) an SDR reference bitmap (Reinhard view) -> ImGui::Image, so the slider is WYSIWYG
//   (b) a luminance histogram in *stops* (camera-style) -> so you can set exposure per game
// Two backends fill this struct: a DX11 compute shader (full-res, any color-like format,
// only a 528-byte histogram readback touches the CPU — see PvGpu in mpr_preview.inc), or
// the CPU staging path below it (HDR float formats only; LDR keeps the raw-SRV view).
struct PreviewCap {
    static constexpr int   HIST_BINS = 128;
    static constexpr float HIST_LO = -20.0f, HIST_HI = 8.0f; // NATIVE (pre-exposure) stops covered
    // CPU-backend plumbing (unused while the GPU backend is active):
    resource      staging[2] = {};       // gpu_to_cpu ping-pong of the source buffer
    fence         done       = {};
    uint64_t      copy_index = 0, read_index = 0;
    uint32_t      w = 0, h = 0;           // source/staging dims
    format        src_fmt    = format::unknown; // staging format key (re-alloc on source change)
    int           throttle   = 0;         // frames until next copy (shared by both backends)
    resource      disp       = {};        // rgba8 dynamic texture shown by ImGui (CPU backend)
    resource_view disp_srv   = {};
    uint32_t      disp_w = 0, disp_h = 0;
    // Shared outputs (either backend; consumers: scope, OSD meter, AE, Fit):
    bool          have_frame = false;
    float         hist[HIST_BINS] = {};   // RAW pixel counts per native-stop bin (peak-normalized at draw)
    float         n_total = 0, n_under = 0, n_over = 0; // sampled px; below-axis/true-black; above-axis
    float         span_lo = 0, span_hi = 0, median_native = 0; // buffer data extent, native stops (0.5/99.5 pctile)
};
static PreviewCap g_pv;
// Scope display mode (persisted): true = native buffer range with a sliding capture window;
// false = curve view (bars shift with exposure, the window stays fixed).
// Scope defaults to CURVE view: bars slide right as exposure rises (camera/Lightroom convention,
// bright = right). Native view (bars fixed, window slides) is the opt-in secondary mode.
static bool g_scope_native  = false;
static bool g_pv_show_clip  = true;  // tint clipped (red) / crushed (blue) pixels in the HDR preview image
static bool g_scope_curve   = true;  // overlay the encode transfer curve (+ dim SDR baseline) on the scope
// Channel isolation in the Viewfinder image (GPU backend only): 0 rgb, 1 r, 2 g, 3 b,
// 4 alpha, 5 luma. Session-only, like Debug View — a "look now" tool, not a preference.
static int  g_pv_channel    = 0;

// RGB waveform / parade scope (mpr_waveform.inc): a per-screen-column value distribution, GPU-built
// into a texture and shown as an ImGui image in the Viewfinder. Independent of the
// exposure scope and works on ANY pass (on the depth pass it reads as a near->far ramp across the
// frame). Persisted, since it's a viewfinder preference rather than a one-shot look.
static bool  g_wf_show   = false; // waveform panel shown in the Viewfinder (gates the GPU build)
static int   g_wf_mode   = 0;     // 0 overlay RGB, 1 parade (R|G|B), 2 luma
static float g_wf_bright = 12.0f; // trace brightness (drives the resolve pass's intensity falloff)

// Why the most recent frame drop happened, surfaced in the live recording stats.
static const char* g_drop_reason = "";

// Shared per-pass write-rate sampler (0.5s cadence). Both the settings-panel stats table and
// the always-on OSD read g_pass_fps so they show identical numbers. Idempotent within a sample
// window: whichever caller crosses the 0.5s mark first advances the counters; the other no-ops.
static double g_pass_fps[N_FIXED + N_CUSTOM] = {};
static void sample_pass_fps() {
    static uint32_t last_count[N_FIXED + N_CUSTOM] = {};
    static std::chrono::steady_clock::time_point last_t{};
    static bool init = false;
    auto now = std::chrono::steady_clock::now();
    if (!init) { init = true; last_t = now; return; }
    double dt = std::chrono::duration<double>(now - last_t).count();
    if (dt < 0.5) return;
    for (int i = 0; i < N_FIXED + N_CUSTOM; ++i) {
        uint32_t fc = g_passes[i].frame_count.load();
        g_pass_fps[i] = (fc >= last_count[i]) ? (fc - last_count[i]) / dt : 0.0;
        last_count[i] = fc;
    }
    last_t = now;
}

// Display name of a pass: the promoted buffer's label when set, else the fixed pass name.
static const char* pass_display_name(const RecordPass& p) {
    return (p.is_custom && p.label[0]) ? p.label : p.name;
}
// Short leading token of the display name (e.g. "o01" out of "o01 r11g11b10_float 1920x1080"),
// for narrow OSD columns.
static void pass_short_name(const RecordPass& p, char* out, size_t outsz) {
    const char* nm = pass_display_name(p);
    size_t k = 0;
    for (; nm[k] && nm[k] != ' ' && k + 1 < outsz; ++k) out[k] = nm[k];
    out[k] = '\0';
}

// ---- Shared exposure-scope primitives ----
// The Viewfinder's 96px scope and the OSD's condensed meter render the SAME sampled data
// (g_pv) against the same capture-window math; these primitives are that shared core, so the
// two instruments can never drift apart. Each caller composes its own dressing around them
// (graticule/hazard-hatching/labels in the Viewfinder, flat zone fills in the OSD).

// Axis rect of one scope draw: origin, pixel size, and the stops-axis extents it maps.
struct ScopeRect { ImVec2 o; float W, H; float axLO, axHI; };
static float scope_x(const ScopeRect& r, float s) {
    float x = r.o.x + r.W * (s - r.axLO) / (r.axHI - r.axLO);
    return x < r.o.x ? r.o.x : (x > r.o.x + r.W ? r.o.x + r.W : x);
}

// Physical population split of a sampled frame against an exposure-shifted capture window
// (native-stop bins): crush = below the window floor, clip = above the ceil, kept = between.
// The histogram is passed in rather than read from g_pv, because two instruments consume this:
// the Viewfinder scope / OSD meter (one pass at a time, from g_pv) and the Record tab's glance
// strip (every pass at once, each from its own round-robin histogram — mpr_tiles.inc). One
// implementation means the strip and the scope can never disagree about what "clipped" means.
// n_under/n_over are the counts that fell outside the histogram's own -20..+8 stop domain; they
// are unconditionally crushed/clipped regardless of where the capture window sits.
static void hist_population(const float* hist, float n_under, float n_over, float n_total,
                            float exposure, float floor_st, float ceil_st,
                            double* crush, double* kept, double* clip, double* total) {
    const float wln = floor_st - exposure, wrn = ceil_st - exposure;
    double cr = n_under, cl = n_over, tot = n_total;
    for (int i = 0; i < PreviewCap::HIST_BINS; ++i) {
        float ns = PreviewCap::HIST_LO + (PreviewCap::HIST_HI - PreviewCap::HIST_LO) * (i + 0.5f) / PreviewCap::HIST_BINS;
        if (ns < wln) cr += hist[i]; else if (ns > wrn) cl += hist[i];
    }
    double kp = tot - cr - cl; if (kp < 0) kp = 0;
    *crush = cr; *kept = kp; *clip = cl; *total = tot;
}

// The scope/meter's view of the above: the currently-previewed pass, from g_pv.
static void scope_population(float exposure, float floor_st, float ceil_st,
                             double* crush, double* kept, double* clip, double* total) {
    hist_population(g_pv.hist, g_pv.n_under, g_pv.n_over, g_pv.n_total,
                    exposure, floor_st, ceil_st, crush, kept, clip, total);
}

// Peak-normalized histogram bars from g_pv.hist, shifted by `shift` stops on the axis
// (curve view shifts bars by exposure; native view passes 0).
static void scope_draw_bars(ImDrawList* dl, const ScopeRect& r, float shift) {
    float peak = 1.0f; for (float c : g_pv.hist) if (c > peak) peak = c;
    const float axSpan = r.axHI - r.axLO;
    const float bwStops = (PreviewCap::HIST_HI - PreviewCap::HIST_LO) / PreviewCap::HIST_BINS;
    const float barPx = r.W * bwStops / axSpan;
    for (int i = 0; i < PreviewCap::HIST_BINS; ++i) {
        float ns = PreviewCap::HIST_LO + (PreviewCap::HIST_HI - PreviewCap::HIST_LO) * (i + 0.5f) / PreviewCap::HIST_BINS;
        float ds = ns + shift;
        if (ds < r.axLO || ds > r.axHI) continue;
        float bh = r.H * (g_pv.hist[i] / peak);
        float x = r.o.x + r.W * (ds - r.axLO) / axSpan;
        dl->AddRectFilled(ImVec2(x, r.o.y + r.H - bh), ImVec2(x + (barPx > 1 ? barPx : 1.0f), r.o.y + r.H), MPR_HIST);
    }
}

// The three reference verticals: capture-window floor/ceil (crush blue / clip red) and diffuse white.
static void scope_draw_marklines(ImDrawList* dl, const ScopeRect& r, float wl, float wr, float ww) {
    dl->AddLine(ImVec2(scope_x(r, ww), r.o.y), ImVec2(scope_x(r, ww), r.o.y + r.H), mpr_alpha(MPR_WHITE, 220), 1.5f);
    dl->AddLine(ImVec2(scope_x(r, wl), r.o.y), ImVec2(scope_x(r, wl), r.o.y + r.H), mpr_alpha(MPR_CRUSH, 200), 1.2f);
    dl->AddLine(ImVec2(scope_x(r, wr), r.o.y), ImVec2(scope_x(r, wr), r.o.y + r.H), mpr_alpha(MPR_CLIP, 200), 1.2f);
}

// pv_free lives with the rest of the viewfinder pipeline in mpr_preview.inc (it also has to
// release the GPU backend's native objects, which are declared there).

// PERSISTENCE RULE: capture/encode settings, panel layout, section/tab state and HUD config
// persist to the game's ReShade ini; transient "look now" VIEW state (preview zoom/pan, channel
// isolate, Debug View, the goto-tab latch) is session-only. New state picks a bucket deliberately.

// Record tab "At a glance" strip: a thumbnail of EVERY pass at once, above the pass list, so a
// buffer that has gone black / bound to the wrong thing / stopped updating is visible without
// opening each pass in turn. Persisted (it costs vertical space, so hiding it is a preference,
// not a look-now toggle). The pass list itself no longer has collapsible sections -- with three
// universal passes and a handful of custom ones, a header was a click that only hid things.
static bool g_show_glance = true;
// Fraction of a frame in one out-of-window zone that earns the amber attention glyph on a tile.
// Deliberately generous: a blown sky or a specular hit is normal and must not cry wolf; this is
// for "a tenth of the frame is landing outside what the encode window can hold".
static constexpr float MPR_RANGE_WARN = 0.10f;
// Panel/Viewfinder window anchoring. ReShade restores ImGui window geometry from the game's ini,
// so where the panel lands on a fresh launch is whatever was saved -- which after a resolution
// change, a different game, or a stray drag is usually somewhere useless. The anchor modes
// re-apply a known column layout ONCE PER GAME LAUNCH (see layout_cond()): the panel snaps to a
// screen edge when the overlay first draws, and the user can still drag/resize it freely for the
// rest of the session. LAYOUT_FREE opts out and keeps whatever ImGui restored.
enum LayoutMode { LAYOUT_LEFT = 0, LAYOUT_RIGHT = 1, LAYOUT_FREE = 2 };
static int  g_layout_mode  = LAYOUT_LEFT;   // persisted (layout_mode)
// g_live_frame on which the panel first drew this run; the launch anchor stays in force for the
// whole of that frame so the Viewfinder -- drawn from the later on_reshade_overlay callback --
// still gets it. 0xffffffff = the panel has not drawn yet. Session-only, nothing to persist.
static uint32_t g_layout_applied_frame = 0xffffffff;
// Frame on which "Apply now" was pressed. Anchoring is forced while g_live_frame is within one
// frame of it, because the two windows are drawn from different callbacks: the panel already ran
// its seed before the button was clicked (so it re-anchors next frame), the Viewfinder draws after
// it (same frame). Re-anchoring is idempotent, so covering both frames is harmless.
static uint32_t g_layout_force_frame = 0xffffffff;

static void detect_game_profile() {
    char path[MAX_PATH] = ""; GetModuleFileNameA(nullptr, path, sizeof(path));
    std::string s(path);
    size_t slash = s.find_last_of("\\/");
    std::string exe = (slash == std::string::npos) ? s : s.substr(slash + 1);
    for (char& c : exe) if (c >= 'A' && c <= 'Z') c += 32; // lowercase, no <cctype> dependency
    strncpy_s(g_exe_name, exe.c_str(), sizeof(g_exe_name) - 1);
    // Output filename prefix: the exe stem, ".exe" stripped. Always populated -- unlike
    // g_profile.name, it doesn't depend on the exe having a GameProfile entry -- and stable
    // across games/sessions the same way hot_reload.py's GAMES keys are, so recordings from
    // different games never collide or need manual sorting in the captures folder.
    std::string stem = exe;
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".exe") == 0) stem.erase(stem.size() - 4);
    strncpy_s(g_game_tag, stem.c_str(), sizeof(g_game_tag) - 1);
    for (const auto& p : PROFILES)
        if (exe.find(p.exe_match) != std::string::npos) { g_profile = p; return; }
    g_profile = PROFILE_DEFAULT;
}



// ---- Helpers ----

// Human-readable name of a virtual-key code for UI labels ("F9", "K", "Num 5", ...).
// Static buffer: UI-thread only, consume before the next call.
static const char* vk_name(int vk) {
    static char buf[32];
    if (vk >= VK_F1 && vk <= VK_F24) { sprintf_s(buf, "F%d", vk - VK_F1 + 1); return buf; }
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    switch (vk) { // extended-key bit for keys whose base scancode collides with the numpad
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            sc |= 0x100; break;
    }
    if (sc != 0 && GetKeyNameTextA((LONG)(sc << 16), buf, sizeof(buf)) > 0) return buf;
    sprintf_s(buf, "VK 0x%02X", vk);
    return buf;
}

static std::string format_hms(uint32_t total_sec) {
    uint32_t h = total_sec / 3600;
    uint32_t m = (total_sec % 3600) / 60;
    uint32_t s = total_sec % 60;
    char buf[64];
    if (h > 0) sprintf_s(buf, "%02u:%02u:%02u", h, m, s);
    else sprintf_s(buf, "%02u:%02u", m, s);
    return buf;
}

static std::string format_duration(uint32_t frames, int fps) {
    if (fps <= 0) return "00:00";
    return format_hms(frames / fps);
}

// Wall-clock seconds since recording started (independent of frames encoded/dropped).
static uint32_t recording_elapsed_sec() {
    auto now = std::chrono::high_resolution_clock::now();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(now - g_record_start_time).count();
    return sec > 0 ? (uint32_t)sec : 0;
}

static std::string captures_dir(bool create = false)
{
    if (strlen(g_output_dir) > 0) {
        if (create) CreateDirectoryA(g_output_dir, nullptr);
        return std::string(g_output_dir);
    }
    char base[MAX_PATH]; size_t sz = sizeof(base);
#if RESHADE_API_VERSION >= 12
    reshade::get_reshade_base_path(base, &sz);
#else
    GetModuleFileNameA(nullptr, base, (DWORD)sz);
    std::string s(base);
    size_t last = s.find_last_of("\\/");
    if (last != std::string::npos) {
        s = s.substr(0, last);
        strcpy_s(base, sizeof(base), s.c_str());
    }
#endif
    std::string d = std::string(base) + "\\captures";
    if (create) CreateDirectoryA(d.c_str(), nullptr);
    return d;
}

