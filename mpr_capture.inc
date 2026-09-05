// === MultipassRecorder — mpr_capture.inc ===
// Capture backend — staging resources, audio FFmpeg process, HDR unpack/tonemap math, per-pass worker thread.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.


// Defined below (worker-side failure reporting); check_and_update_staging needs it to report a
// mid-take source resize through the same channel as an encoder death.
static void enc_fail(RecordPass* p, const char* fmt, ...);

static void destroy_pass_resources(device* dev, RecordPass& p)
{
    // A stop still finalising in the background owns these same worker handles, so rendezvous with
    // it first: joining a std::thread from two threads at once is UB. Reached from the UI (removing
    // a pass) as well as the lifecycle handlers, so the guard belongs here, not at the call sites.
    stop_join_wait();
    // Wait for worker to finish before destroying things it might be using
    if (p.worker.joinable()) {
        { std::lock_guard<std::mutex> lk(p.q_mutex); p.worker_stop = true; }
        p.q_cv.notify_one();
        p.worker.join();
    }
    // Clean up queue
    {
        std::lock_guard<std::mutex> lock(p.q_mutex);
        while (!p.queue.empty()) { p.release_buf(std::move(p.queue.front().buf)); p.queue.pop(); }
        p.queue_depth = 0;
        p.queue_bytes = 0;
    }
    // The held frame's width/height/pitch describe the staging config being torn down here. The
    // usual reason for that is a source resize, after which the respawned FFmpeg expects the NEW
    // geometry — pushing this buffer then would byte-shift the rest of the stream into garbage.
    p.drop_last_frame();

    for (int i=0; i<3; ++i) {
        if (p.staging_buf[i].handle) { dev->destroy_resource(p.staging_buf[i]); p.staging_buf[i] = { 0 }; }
    }
    if (p.done_fence.handle) { dev->destroy_fence(p.done_fence); p.done_fence = { 0 }; }
    p.active = false;
    p.copy_index = 0;
    p.read_index = 0;
}

static bool check_and_update_staging(device* dev, RecordPass& p, resource_desc source_desc)
{
    uint32_t w = source_desc.texture.width;
    uint32_t h = source_desc.texture.height;
    uint32_t rp = (w * p.bytes_per_px + 3) & ~3;
    if (p.active && p.width == w && p.height == h && p.staging_buf[0].handle) return true;

    // Mid-take resize: the pass ENDS here, it does not restart. Rebuilding would join the worker on
    // the render thread (draining up to 300 frames, then waiting up to 30s on FFmpeg) and then
    // respawn against the unchanged g_session_time — same filename, `-y`, so FFmpeg would truncate
    // everything recorded so far with no failure anywhere to show for it. Report it the way an
    // encoder death is reported instead: enc_failed takes the pass out of pass_is_live, so the
    // global frame lock can't make it cost the other passes their frames, and the panel/OSD/stats
    // table name it. Resizes while idle still rebuild normally.
    if (g_recording && p.active && p.worker.joinable()) {
        enc_fail(&p, "the source resized to %ux%u mid-take (was %ux%u) - the pass stopped there; "
                     "restart recording to capture at the new size.", w, h, p.width, p.height);
        return false;
    }

    destroy_pass_resources(dev, p);

    p.width = w; p.height = h; p.row_pitch = rp;
    
    // DX11 ReShade backend lacks copy_texture_to_buffer, requiring texture-to-texture copies.
    // OpenGL ReShade backend has slow blocking map_texture_region, requiring buffer-to-buffer PBOs.
    p.use_texture = (dev->get_api() != device_api::opengl && dev->get_api() != device_api::vulkan);

    resource_desc sd;
    if (p.use_texture) {
        sd = source_desc;
        sd.type = resource_type::texture_2d;
        sd.heap = memory_heap::gpu_to_cpu;
        sd.usage = resource_usage::copy_dest;
        sd.flags = resource_flags::none;
        sd.texture.levels = 1;
    } else {
        sd = resource_desc(rp * h, memory_heap::gpu_to_cpu, resource_usage::copy_dest);
    }

    for (int i=0; i<3; ++i) {
        if (!dev->create_resource(sd, nullptr, resource_usage::copy_dest, &p.staging_buf[i])) {
            reshade::log::message(reshade::log::level::error, "MultipassRecorder: Failed to create staging resource.");
            return false;
        }
    }
    if (!dev->create_fence(0, fence_flags::none, &p.done_fence)) return false;

    p.copy_index = 0;
    p.read_index = 0;
    p.active = true;
    return true;
}

// ---- Audio capture (dshow/WASAPI → FLAC, separate FFmpeg process) ----
// The stop path finalises the FLAC on a background thread (it waits up to 15s on FFmpeg). That
// thread is TRACKED, not detached: it runs code in this DLL, so AddonUninit has to wait for it or
// the DLL unloads out from under it — an access violation on game exit. Only one audio encoder
// exists at a time, so one slot is enough; the START of the next take is where a still-running
// finalizer is waited out (user-initiated, like the video stop rendezvous), which keeps the
// wait off the stop path.
static std::thread g_audio_finalizer;
static void audio_finalize_wait() { if (g_audio_finalizer.joinable()) g_audio_finalizer.join(); }

static void start_audio_recording(const std::string& out_path) {
    if (g_audio_proc) return;
    audio_finalize_wait(); // the previous take's finalizer owns g_audio_finalizer

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    // Stdin pipe: we write "q\n" to this to trigger FFmpeg's interactive quit,
    // which flushes and finalises the FLAC. GenerateConsoleCtrlEvent doesn't work
    // from a console-less game process.
    HANDLE pipe_read, pipe_write;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) return;
    SetHandleInformation(pipe_write, HANDLE_FLAG_INHERIT, 0);

    HANDLE h_log     = CreateFileA((captures_dir(true) + "\\ffmpeg_log.txt").c_str(),
                           FILE_APPEND_DATA, FILE_SHARE_READ, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE h_nul_out = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOA si = {};
    si.cb         = sizeof(si);
    si.hStdInput  = pipe_read;
    si.hStdError  = (h_log != INVALID_HANDLE_VALUE) ? h_log : h_nul_out;
    si.hStdOutput = h_nul_out;
    si.dwFlags    = STARTF_USESTDHANDLES;

    char args[2048];
    // _TRUNCATE, never plain sprintf_s: the FFmpeg path (511) + the user's input args (511) + an
    // arbitrarily long output path can overrun this, and sprintf_s ABORTS THE GAME on overflow.
    _snprintf_s(args, sizeof(args), _TRUNCATE, "\"%s\" -y %s -c:a flac \"%s\"",
        g_ffmpeg_path, g_audio_input_args, out_path.c_str());

    reshade::log::message(reshade::log::level::info, (std::string("MultipassRecorder audio cmd: ") + args).c_str());

    PROCESS_INFORMATION pi = {};
    // CREATE_NEW_PROCESS_GROUP: gives FFmpeg its own group so we can later target it with
    //   GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid) without disturbing the game.
    // CREATE_NO_WINDOW: FFmpeg still gets a (hidden) console, which is what we attach to on stop.
    if (CreateProcessA(nullptr, args, nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
        g_audio_proc  = pi.hProcess;
        g_audio_pid   = pi.dwProcessId;
        g_audio_stdin = pipe_write;
        tether_ffmpeg_to_game(pi.hProcess); // die with the game if it's killed/crashes (dshow input never EOFs on its own)
        CloseHandle(pi.hThread);
        reshade::log::message(reshade::log::level::info, "MultipassRecorder: audio capture started.");
    } else {
        char err[128]; sprintf_s(err, "MultipassRecorder: audio CreateProcess failed, error %lu", GetLastError());
        reshade::log::message(reshade::log::level::error, err);
        CloseHandle(pipe_write);
    }
    CloseHandle(pipe_read);
    if (h_log != INVALID_HANDLE_VALUE && h_log != h_nul_out) CloseHandle(h_log);
    CloseHandle(h_nul_out);
}

// Send FFmpeg a Ctrl-Break, which its signal handler treats like Ctrl-C: flush, write the
// FLAC footer, exit cleanly. From a console-less game we must briefly borrow FFmpeg's own
// console: AttachConsole(pid) -> disable our own handler so the event can't kill the game ->
// raise CTRL_BREAK for FFmpeg's group -> detach.
static void signal_ffmpeg_break(DWORD pid) {
    // AttachConsole only works from a process that has NO console, which a game normally is. If the
    // host already owns one (console-subsystem host, or a game that allocated one for its logging),
    // freeing it to borrow FFmpeg's would steal it PERMANENTLY on the first audio stop — so give up
    // the signal instead and let the stdin "q" already sent, or the finalize timeout, stop FFmpeg.
    // GetConsoleProcessList (not GetConsoleWindow) is the exact test: it returns 0 only when the
    // caller is attached to no console at all, windowless pseudoconsoles included.
    DWORD own_console = 0;
    if (GetConsoleProcessList(&own_console, 1) != 0) return;
    if (AttachConsole(pid)) {
        SetConsoleCtrlHandler(nullptr, TRUE);          // ignore the event in our (game) process
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
        FreeConsole();
        SetConsoleCtrlHandler(nullptr, FALSE);         // restore default handling
    }
}

static void stop_audio_recording(bool synchronous = false) {
    if (!g_audio_proc) return;
    HANDLE proc = g_audio_proc;
    DWORD  pid  = g_audio_pid;
    g_audio_proc = nullptr;
    g_audio_pid  = 0;

    // Fallback path: ask FFmpeg to quit via stdin too (harmless if it ignores it).
    if (g_audio_stdin) {
        DWORD written = 0;
        WriteFile(g_audio_stdin, "q\n", 2, &written, nullptr);
        CloseHandle(g_audio_stdin);
        g_audio_stdin = nullptr;
    }
    reshade::log::message(reshade::log::level::info, "MultipassRecorder: audio stop signalled.");

    auto finalize = [proc, pid]() {
        signal_ffmpeg_break(pid);
        // Give FFmpeg time to finalise the FLAC; hard-kill only if it hangs.
        if (WaitForSingleObject(proc, 15000) == WAIT_TIMEOUT) TerminateProcess(proc, 1);
        CloseHandle(proc);
    };
    // The wait is a no-op in practice — start_audio_recording already rendezvoused with the previous
    // finalizer, and there can't be a live g_audio_proc to stop without a start — but assigning over
    // a joinable std::thread calls terminate(), so it isn't optional.
    audio_finalize_wait();
    if (synchronous) finalize();
    else g_audio_finalizer = std::thread(finalize);
}

// ---- HDR (r11g11b10_float) -> 16-bit PNG helpers ----
// Linear fixed-point scale into the 16-bit PNG (range 0..~16). Post-processing
// divides by this to recover linear HDR.
static constexpr float HDR_PNG_SCALE = 4096.0f;
static inline float hdr_unpack_f11(uint32_t u) { // 5-bit exp, 6-bit mantissa, unsigned
    uint32_t e = (u >> 6) & 0x1F, m = u & 0x3F;
    return e == 0 ? (m / 64.0f) * ldexpf(1.0f, -14) : (1.0f + m / 64.0f) * ldexpf(1.0f, (int)e - 15);
}
static inline float hdr_unpack_f10(uint32_t u) { // 5-bit exp, 5-bit mantissa, unsigned
    uint32_t e = (u >> 5) & 0x1F, m = u & 0x1F;
    return e == 0 ? (m / 32.0f) * ldexpf(1.0f, -14) : (1.0f + m / 32.0f) * ldexpf(1.0f, (int)e - 15);
}
static inline uint16_t hdr_to_u16(float v) {
    float s = v * HDR_PNG_SCALE;
    if (s < 0.0f) s = 0.0f;
    if (s > 65535.0f) s = 65535.0f;
    return (uint16_t)(s + 0.5f);
}
// The video HDR curves below take a scene-linear value that the worker has ALREADY multiplied by
// the pass's exposure gain (2^stops). They are otherwise pure transfer functions.
//
// Rec.709 view: Reinhard highlight rolloff + gamma 2.2 — an SDR-range look that reads correctly on
// import (no color management needed). Highlights are compressed, not recoverable. PNG stays linear.
static inline uint16_t hdr_tonemap_u16(float v) {
    if (v < 0.0f) v = 0.0f;
    v = v / (1.0f + v);                 // Reinhard highlight rolloff
    v = powf(v, 1.0f / 2.2f);           // perceptual gamma
    if (v > 1.0f) v = 1.0f;
    return (uint16_t)(v * 65535.0f + 0.5f);
}
// PQ / HDR10 (SMPTE ST 2084, Rec.2100). The standard HDR encoding every NLE reads in one click.
// Scene-linear v=1.0 maps to g_hdr_pq_paper_nits, so highlights above diffuse white keep their range
// up to 10000 nits, and the 10 bits are spent perceptually. File is tagged Rec.2100 PQ.
static float g_hdr_pq_paper_nits = 100.0f; // display luminance assigned to scene-linear 1.0
static inline uint16_t hdr_pq_u16(float v) {
    if (v < 0.0f) v = 0.0f;
    float L = v * (g_hdr_pq_paper_nits / 10000.0f); // normalize to PQ's 0..1 == 0..10000 nits
    if (L > 1.0f) L = 1.0f;
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float Lp = powf(L, m1);
    float N = powf((c1 + c2 * Lp) / (1.0f + c3 * Lp), m2);
    if (N < 0.0f) N = 0.0f; else if (N > 1.0f) N = 1.0f;
    return (uint16_t)(N * 65535.0f + 0.5f);
}
// MAX-INFO / CONSISTENT: a fixed wide-range log2. One mapping for every game and scene (NO exposure,
// NO auto-anything) so results are directly comparable and nothing clips at either end. Uniform code
// allocation per stop across FLOOR..CEIL, with a low floor so very dark engine buffers keep detail
// (unlike LogC/PQ, which starve the darks). Decode to linear in post: linear = 2^(code*SPAN + FLOOR).
static constexpr float HDR_LOG2_FLOOR = -14.0f;  // darkest captured = 2^-14 (~6e-5 linear)
static constexpr float HDR_LOG2_CEIL  = 6.0f;    // brightest captured = 2^+6 (64x diffuse white)
static constexpr float HDR_LOG2_SPAN  = HDR_LOG2_CEIL - HDR_LOG2_FLOOR; // 20 stops
static inline uint16_t hdr_log2_u16(float v) {
    float e = (v > 0.0f) ? log2f(v) : HDR_LOG2_FLOOR;         // true black -> floor -> code 0
    float y = (e - HDR_LOG2_FLOOR) / HDR_LOG2_SPAN;           // uniform per-stop over the fixed range
    if (y < 0.0f) y = 0.0f; else if (y > 1.0f) y = 1.0f;
    return (uint16_t)(y * 65535.0f + 0.5f);
}

// ---- Encoder failure reporting ----
// Publish a one-shot failure for this pass: log it, and leave a message the panel/OSD can show.
// The FIRST cause wins — once an encoder is gone every later write fails too, and "FFmpeg exited
// (code 1)" is the useful line, not the broken-pipe fallout behind it.
// Three callers, two of which CAN overlap: the pass's own worker, the render thread's mid-take
// source-resize check (check_and_update_staging, fires while that worker is still alive), and the
// render thread's catch-all in on_reshade_present (that one can't overlap — it only runs once
// worker_running reads false, which the worker stores as its last act after any enc_fail of its
// own). enc_claim is what makes the overlap safe: one winner writes enc_error, and enc_failed
// flips only after the message is whole.
static void enc_fail(RecordPass* p, const char* fmt, ...) {
    if (p->enc_claim.exchange(true, std::memory_order_acq_rel)) return;
    va_list ap; va_start(ap, fmt);
    // _vsnprintf_s/_TRUNCATE, never the plain secure CRT call: these interpolate the user's FFmpeg
    // path and output paths, and sprintf_s ABORTS THE GAME on overflow.
    _vsnprintf_s(p->enc_error, sizeof(p->enc_error), _TRUNCATE, fmt, ap);
    va_end(ap);
    char msg[320];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "MultipassRecorder: pass '%s' - %s", p->name, p->enc_error);
    reshade::log::message(reshade::log::level::error, msg);
    p->enc_failed.store(true, std::memory_order_release); // release: message is complete first
}

// ---- Worker thread ----
static void worker_func(RecordPass* p, std::string out_path)
{
    const char* exe = g_ffmpeg_path;
    // Security attributes for inheritable handles
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE pipe_read, pipe_write;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 1024 * 1024 * 32)) {
        enc_fail(p, "couldn't create the FFmpeg pipe (error %lu) - nothing was recorded.", GetLastError());
        p->worker_running = false;
        return;
    }
    SetHandleInformation(pipe_write, HANDLE_FLAG_INHERIT, 0); // Ensure pipe_write is not inherited by child

    HANDLE h_log = CreateFileA((captures_dir(true) + "\\ffmpeg_log.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE h_nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdInput = pipe_read;
    si.hStdError = (h_log != INVALID_HANDLE_VALUE) ? h_log : h_nul; // Use h_nul if h_log is invalid
    si.hStdOutput = h_nul;
    si.dwFlags = STARTF_USESTDHANDLES;

    // Build FFmpeg args from the structured encoding fields. Input pix_fmt follows the SOURCE
    // (rgb48le 16-bit for HDR passes, rgba 8-bit otherwise); the codec/bits/chroma/quality pick
    // the OUTPUT. FFmpeg converts bit depth internally, so a 16-bit input can feed a 10-bit encode.
    // A packed-HDR source (r11g11b10_float, 4 bytes/px) is unpacked to 16-bit rgb48le below, so
    // FFmpeg must be told rgb48le for it — regardless of whether this is a built-in HDR pass or a
    // user-assigned engine RT. Any other format feeds through as-is (8-bit rgba).
    const bool unpack_hdr = (p->cap_format == format::r11g11b10_float);
    // Native depth (mpr_native.inc) arrives as single-channel 32-bit float. It is ALSO 4 bytes per
    // pixel, so nothing downstream would have caught this: handing it to FFmpeg as "rgba" passes
    // every size check and silently encodes float bit patterns as colour noise. Converted to
    // gray16le below. (NVENC still tops out at p010le, so the file carries 10 of those bits — the
    // full float only survives into stills/EXR. See mpr_native.inc's header.)
    const bool unpack_depth = (p->cap_format == format::r32_float);
    // For a real video codec, shape the HDR into a viewable/gradeable perceptual encode; PNG (and
    // custom) keep the raw linear fixed-point so the effects-diff math stays exact.
    const bool tonemap_hdr = unpack_hdr && (p->enc_codec == ENC_HEVC || p->enc_codec == ENC_H264);
    // PQ/HDR10 and Log2 both need the 10-bit p010 path; HEVC only.
    const bool pq    = tonemap_hdr && (p->enc_hdr == HDR_PQ)   && (p->enc_codec == ENC_HEVC);
    const bool log2e = tonemap_hdr && (p->enc_hdr == HDR_LOG2) && (p->enc_codec == ENC_HEVC);
    const float hdr_gain = powf(2.0f, p->enc_exposure); // exposure (stops) applied before the curve
    char args[2048];
    const char* in_pf = unpack_hdr ? "rgb48le" : unpack_depth ? "gray16le" : p->ffmpeg_fmt;
    char inspec[256];
    sprintf_s(inspec, "-y -f rawvideo -pix_fmt %s -s %ux%u -r %d -i pipe:0", in_pf, p->width, p->height, g_fps);

    // _TRUNCATE on every command line below, never plain sprintf_s: the FFmpeg path (511), the
    // output path and — for ENC_CUSTOM — the user's own args (1023) together overrun args[2048],
    // and sprintf_s ABORTS THE GAME on overflow. On the worker thread, mid-take, from legitimate
    // UI input. A truncated command line fails to start and reports through enc_fail instead.
    if (p->enc_codec == ENC_PNG) {
        // PNG sequence via the image2 muxer (out_path is a %06d.png pattern). 16-bit from an HDR
        // source, 8-bit from an LDR source — bit depth follows the input.
        _snprintf_s(args, sizeof(args), _TRUNCATE, "\"%s\" %s \"%s\"", exe, inspec, out_path.c_str());
    } else if (p->enc_codec == ENC_CUSTOM) {
        _snprintf_s(args, sizeof(args), _TRUNCATE, "\"%s\" %s %s \"%s\"", exe, inspec, p->custom_args, out_path.c_str());
    } else {
        const char* enc = (p->enc_codec == ENC_H264) ? "h264_nvenc" : "hevc_nvenc";
        // PQ / Log2 force the 10-bit p010 path regardless of the Bits dropdown (both band in 8-bit).
        const bool ten = ((p->enc_bits == BITS_10) || pq || log2e) && (p->enc_codec == ENC_HEVC); // NVENC H.264: no 10-bit
        // IMPORTANT: only emit NVENC-native pixel formats. If FFmpeg is handed an unsupported one
        // (e.g. yuv444p10le) it SILENTLY falls back to yuv444p16le, which many drivers encode as
        // garbage (rainbow noise). NVENC's only reliable 10-bit path is p010le (4:2:0), so 10-bit
        // always uses that; true 10-bit+ 4:4:4 must go through the PNG (16-bit) codec instead.
        const char* out_pf;
        if (ten)                                out_pf = "p010le";  // 10-bit 4:2:0
        else if (p->enc_chroma == CHROMA_444)   out_pf = "yuv444p"; // 8-bit 4:4:4
        else                                    out_pf = "yuv420p"; // 8-bit 4:2:0 / luma
        char qual[64];
        if (p->enc_lossless) strcpy_s(qual, "-tune lossless");
        else                 sprintf_s(qual, "-rc constqp -qp %d", p->enc_cq);
        // Tag the color pipeline so players/AE/Resolve don't have to guess. CRITICAL: the engine
        // buffer is Rec.709-primaries scene-linear and we do NOT convert its gamut, so we must tag
        // the primaries we ACTUALLY have (bt709). Tagging bt2020 without a real 709->2020 conversion
        // is a lie that shifts every color (cyan cast) in any color-managed HDR viewer. PQ therefore
        // keeps the ST 2084 transfer (for the HDR range) but in honest Rec.709 primaries + matrix.
        // (True HDR10 delivery would need a 709->2020 matrix on linear RGB before PQ; not done here -
        // convert to Rec.2020 at delivery in the NLE instead.) Log -> 709 primaries, transfer left
        // UNSPECIFIED so no app applies a gamma that fights the log curve. Otherwise Rec.709 SDR view.
        // -color_range pc (full, 0-255/0-1023) is the RANGE TAG only, and a tag on its own is a lie:
        // measured on ffmpeg 7.0 (rawvideo rgba/rgb48le -> hevc_nvenc), the implicit RGB->YUV
        // conversion writes LIMITED/TV values whether -color_range says pc, tv, or nothing at all —
        //   8-bit "lossless" 4:4:4 : 0/128/255 in -> Y 16/126/235 stored   (not pixel-exact)
        //   10-bit PQ p010         : 0/32768/65535 in -> Y 64/504/941      (PQ code words shifted)
        // i.e. every pass was written squeezed AND tagged full, so a color-managed reader expanded it
        // a second time (lifted blacks, dull whites) and mpr_convert.py inherited the double squash.
        // Only the scale filter's out_range actually converts, so MPR_VF_RANGE below is what makes the
        // pc tag true; with it the same tests give 0/128/255 and 0/512/1023. Load-bearing for
        // depth/normals precision, for "Lossless" meaning it, and for PQ landing on real nits.
        static const char* const MPR_VF_RANGE = "-vf scale=in_range=full:out_range=full";
        // Absence of -color_trc is now the SIGNATURE of the Log2 curve (mpr_convert.py's build_args
        // keys on exactly that to drop the colr atom instead of lying with bt709), so every non-log
        // pass has to state a transfer — including the plain 8-bit ones, which used to state nothing
        // and were therefore indistinguishable from a log pass on the converter side.
        const char* ctag =
            pq    ? "-color_range pc -color_primaries bt709 -color_trc smpte2084 -colorspace bt709 " :
            log2e ? "-color_range pc -color_primaries bt709 -colorspace bt709 " : // custom curve, transfer left unspecified
                    // tonemapped HDR and the universal 8-bit passes (world/depth/normals/promoted)
                    "-color_range pc -color_primaries bt709 -color_trc bt709 -colorspace bt709 ";
        char preset[8]; int pv = p->enc_preset; if (pv < 1) pv = 1; else if (pv > 7) pv = 7;
        sprintf_s(preset, "p%d", pv); // p1 fastest/most efficient encode .. p7 slowest/best quality
        // -bf 0 -rc-lookahead 0: no B-frames, no lookahead. Both cost encoder throughput and add
        // latency/reordering a real-time capture doesn't want (all-intra-ish stream is also easier to
        // scrub in an NLE). Small compression cost for a meaningful speed-up on the shared GPU.
        _snprintf_s(args, sizeof(args), _TRUNCATE,
            "\"%s\" %s %s -c:v %s -preset %s -bf 0 -rc-lookahead 0 %s -pix_fmt %s %s\"%s\"",
            exe, inspec, MPR_VF_RANGE, enc, preset, qual, out_pf, ctag, out_path.c_str());
    }

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, args, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        // ERROR_FILE_NOT_FOUND (2) / ERROR_PATH_NOT_FOUND (3) is the overwhelmingly common case:
        // FFmpeg isn't where the Setup tab says it is. Name the path so the fix is obvious.
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            enc_fail(p, "FFmpeg not found at '%s' - set the path in the Setup tab.", exe);
        else
            enc_fail(p, "FFmpeg didn't start (error %lu) - check the path in the Setup tab.", e);
        CloseHandle(pipe_read); CloseHandle(pipe_write);
        // h_log is INVALID_HANDLE_VALUE if the log couldn't be opened, and closing that raises
        // EXCEPTION_INVALID_HANDLE under a debugger (same guard as the audio path above).
        if (h_log != INVALID_HANDLE_VALUE && h_log != h_nul) CloseHandle(h_log);
        CloseHandle(h_nul);
        p->worker_running = false;
        return;
    }
    tether_ffmpeg_to_game(pi.hProcess); // also tether video encoders (they EOF on pipe close, but this guarantees no orphan)
    CloseHandle(pipe_read);

    // Scratch buffers (reused across frames, no alloc per frame)
    std::vector<uint8_t> dense_buf;
    std::vector<uint8_t> png_buf;   // r11g11b10 -> rgb48le staging for packed-HDR passes

    // Per-channel conversion LUTs: the r11g11b10 source has only 2048 (11-bit R/G) / 1024 (10-bit B)
    // possible channel codes, so unpack + exposure + curve collapses to a table lookup. This removes
    // ~12M powf() per frame from this worker (the CPU bottleneck that caps HDR real-time regardless
    // of NVENC preset) — the inner loop below becomes three array reads. Exposure gain is snapshotted
    // once here (consistent per recording), matching how hdr_gain was already captured above.
    std::vector<uint16_t> lut_rg, lut_b;
    if (unpack_hdr) {
        uint16_t (*enc16)(float) = hdr_to_u16;
        if (tonemap_hdr) enc16 = log2e ? hdr_log2_u16 : pq ? hdr_pq_u16 : hdr_tonemap_u16;
        lut_rg.resize(2048); lut_b.resize(1024);
        for (uint32_t c = 0; c < 2048; ++c) lut_rg[c] = enc16(hdr_unpack_f11(c) * hdr_gain);
        for (uint32_t c = 0; c < 1024; ++c) lut_b[c]  = enc16(hdr_unpack_f10(c) * hdr_gain);
    }

    // (worker_running was set by the render thread before this thread was created — see the spawn
    // site in on_reshade_present — so that "joinable but not running" can only mean "exited".)
    while (true) {
        FrameData fd;
        {
            std::unique_lock<std::mutex> lk(p->q_mutex);
            p->q_cv.wait(lk, [p] { return !p->queue.empty() || p->worker_stop; });
            if (p->queue.empty()) break;
            fd = std::move(p->queue.front());
            p->queue.pop();
            p->queue_depth.fetch_sub(1, std::memory_order_relaxed);
            p->queue_bytes.fetch_sub(fd.buf.size(), std::memory_order_relaxed);
        }

        // Strip GPU row padding on the worker thread (not the render thread)
        uint32_t dense_pitch = fd.width * fd.bytes_per_px;
        const uint8_t* write_ptr = fd.buf.data();
        DWORD write_size = dense_pitch * fd.height;

        if (fd.src_pitch != dense_pitch) {
            dense_buf.resize(write_size);
            for (uint32_t y = 0; y < fd.height; ++y)
                memcpy(dense_buf.data() + y * dense_pitch, fd.buf.data() + y * fd.src_pitch, dense_pitch);
            write_ptr = dense_buf.data();
        }

        // Packed-HDR source: convert dense r11g11b10_float -> rgb48le (16-bit) before FFmpeg.
        // The unpack+exposure+curve was baked into lut_rg/lut_b above, so this is pure table lookups.
        if (unpack_hdr) {
            size_t npx = (size_t)fd.width * fd.height;
            png_buf.resize(npx * 6);
            const uint32_t* srcpx = reinterpret_cast<const uint32_t*>(write_ptr);
            uint16_t* dst = reinterpret_cast<uint16_t*>(png_buf.data());
            const uint16_t* lrg = lut_rg.data();
            const uint16_t* lb  = lut_b.data();
            for (size_t i = 0; i < npx; ++i) {
                uint32_t u = srcpx[i];
                dst[i * 3 + 0] = lrg[u & 0x7FF];
                dst[i * 3 + 1] = lrg[(u >> 11) & 0x7FF];
                dst[i * 3 + 2] = lb[(u >> 22) & 0x3FF];
            }
            write_ptr  = png_buf.data();
            write_size = (DWORD)(npx * 6);
        }

        // Native depth: r32_float -> gray16le. No LUT, unlike the packed-HDR path above — an f32
        // domain has 2^32 codes, so there is nothing to tabulate; it's a scale+clamp per pixel,
        // still on the worker thread and never the render one.
        // NOTE the 0..1 assumption: that is what the depth shader emits normally. With the pass's
        // "scene-linear output" option on it emits WORLD UNITS instead, which clamp to white here.
        // That option is meant for the EXR still path, which carries float and needs no scaling.
        if (unpack_depth) {
            const size_t npx = (size_t)fd.width * fd.height;
            png_buf.resize(npx * 2);
            const float* srcpx = reinterpret_cast<const float*>(write_ptr);
            uint16_t* dst = reinterpret_cast<uint16_t*>(png_buf.data());
            for (size_t i = 0; i < npx; ++i) {
                const float v = srcpx[i] * 65535.0f;
                dst[i] = (uint16_t)(v <= 0.0f ? 0.0f : (v >= 65535.0f ? 65535.0f : v + 0.5f));
            }
            write_ptr  = png_buf.data();
            write_size = (DWORD)(npx * 2);
        }

        DWORD written = 0;
        if (!WriteFile(pipe_write, write_ptr, write_size, &written, nullptr)) {
            // FFmpeg holds the only read end of this pipe, so a write can only fail because it is
            // gone — the read handle closes the instant the process exits, regardless of how much
            // is still buffered. That makes this the detector for a mid-take encoder death (NVENC
            // session limit, unsupported pix_fmt, disk full): it fires within a frame of the exit,
            // where before, the worker just broke out of the loop and the pass went quietly dead.
            const DWORD werr = GetLastError();
            DWORD code = STILL_ACTIVE;
            WaitForSingleObject(pi.hProcess, 500); // let it finish exiting so the code is real
            GetExitCodeProcess(pi.hProcess, &code);
            const uint32_t wrote = p->frame_count.load();
            if (code != STILL_ACTIVE)
                enc_fail(p, "FFmpeg exited (code %lu) after %u frame%s - see captures\\ffmpeg_log.txt.",
                         code, wrote, wrote == 1 ? "" : "s");
            else
                enc_fail(p, "write to FFmpeg failed (error %lu) after %u frame%s.",
                         werr, wrote, wrote == 1 ? "" : "s");
            p->release_buf(std::move(fd.buf)); // return buffer to pool even on error
            break;
        }
        p->frame_count++;
        p->release_buf(std::move(fd.buf)); // return buffer to pool for reuse
    }

    // A failed pass is taken out of the capture path (pass_is_live), but frames queued before the
    // failure became visible are still holding pooled buffers — several hundred MB at 4K. Hand
    // them back now rather than at the end of the take.
    if (p->enc_failed.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(p->q_mutex);
        while (!p->queue.empty()) { p->release_buf(std::move(p->queue.front().buf)); p->queue.pop(); }
        p->queue_depth = 0;
        p->queue_bytes = 0;
    }

    CloseHandle(pipe_write); // EOF on FFmpeg's stdin: it flushes, writes the trailer, exits 0
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) {
        // Clean-shutdown path, but the file can still be bad: a non-zero code here means FFmpeg
        // failed while finalising (out of space writing the moov atom, say), so the recording is
        // truncated or absent. Only meaningful when the wait actually succeeded — after a timeout
        // GetExitCodeProcess reports STILL_ACTIVE, which is not an error code.
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code) && code != 0)
            enc_fail(p, "FFmpeg exited (code %lu) - the file may be truncated; see captures\\ffmpeg_log.txt.", code);
    } else {
        enc_fail(p, "FFmpeg didn't exit within 30s - the file may be unfinished.");
    }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (h_log != INVALID_HANDLE_VALUE && h_log != h_nul) CloseHandle(h_log);
    CloseHandle(h_nul);
    p->worker_running = false; // last act: everything above is published before this flips
}

