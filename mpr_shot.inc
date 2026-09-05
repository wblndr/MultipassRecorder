// === MultipassRecorder — mpr_shot.inc ===
// Still capture — one keypress writes a frame-coherent set of stills: the game view after
// ReShade's effect chain, plus a still of every enabled capture pass, each in the best format
// its pixel format can actually carry.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.
//
// WHY this isn't just "record one frame". The video path is built around a sustained cadence:
// a persistent FFmpeg child per pass, triple-buffered staging, a global frame lock, NVENC
// sessions. A still wants none of that and needs three things the video path deliberately can't
// give it — a format that isn't a video codec (EXR/PNG16), sources the encode path refuses
// (rgba16f, r10g10b10a2, 1- and 2-channel buffers), and no cadence at all. So stills get their
// own one-shot staging + their own short-lived FFmpeg, and touch none of the recorder's state.
// A still can therefore be taken mid-take without costing the recording a frame.
//
// THE FOUR PHASES, and why the grab is not on the frame you pressed the key:
//   WAIT  - self-timer / burst spacing. The live-RT registry is kept enumerating the whole time
//           (g_shot_pending), because an SRC_ENGINE_RT pass can only resolve its fingerprint
//           against a registry something is actually feeding.
//   GRAB  - one designated frame (g_shot_capture_frame). mpr_fxlayer.inc forces every enabled
//           effect layer to render on exactly that frame, on_reshade_finish_effects copies the
//           screen, and on_reshade_present copies every pass source. All of it is the SAME game
//           frame, which is the whole point: the AO still registers with the beauty still.
//   DRAIN - poll the fence non-blocking (never stall the render thread for a screenshot), then
//           map, copy out, and hand the pixels to the writer thread.
//   write - off the render thread entirely: decode, encode, spawn FFmpeg, write the sidecar.
//
// WHAT GETS BAKED IN, AND WHAT DOESN'T. An EXR is written scene-linear with NO exposure and NO
// transfer curve — it is the archival file, and exposure is a grading decision that belongs in
// the NLE, not burnt into the pixels. The PNG preview beside it is the opposite: it bakes the
// pass's live exposure + the same Reinhard/gamma view the Viewfinder shows, so it matches what
// you framed and opens in any image viewer. 8-bit and unorm sources have no such choice to make
// and pass through untouched.

// The settings themselves (ShotFmt, ShotConfig, g_shotcfg and the combo tables) live in
// mpr_state.inc, because mpr_config.inc — which persists them — is included before this file.
// Same split the preview subsystem uses: PreviewCap is state, the pipeline is its own fragment.
// The shutter is HK_STILL in the shared action table (mpr_hotkeys.inc): no second hotkey
// mechanism lives here.

// ---- Source classification --------------------------------------------------------------
// What KIND of pixels a buffer holds, which is all the still path needs to decide how to write
// it. Deliberately wider than the encode path (pass_is_hdr_src / fx_format_supported): a still
// has no NVENC and no fixed pix_fmt to satisfy, so rgba16f, r10g10b10a2 and the 1-/2-channel
// data buffers are all perfectly writable here even though they can't be recorded as video.
enum ShotKind : int { SK_UNSUPPORTED = 0, SK_LDR8, SK_UNORM, SK_FLOAT };
static ShotKind shot_kind(format f) {
    switch (f) {
        case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb: case format::r8g8b8a8_typeless:
        case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb:
            return SK_LDR8;
        case format::r10g10b10a2_unorm: case format::r16g16b16a16_unorm:
        case format::r16g16_unorm: case format::r8g8_unorm:
        case format::r8_unorm: case format::r16_unorm:
            return SK_UNORM;
        case format::r11g11b10_float: case format::r9g9b9e5:
        case format::r16g16b16a16_float: case format::r32g32b32a32_float:
        case format::r16g16_float: case format::r16_float: case format::r32_float:
            return SK_FLOAT;
        default:
            return SK_UNSUPPORTED;
    }
}
// Is this an 8-bit BGRA layout? (The passthrough path hands the bytes to FFmpeg untouched, so
// the channel order becomes the input pix_fmt rather than a CPU swizzle.)
static bool shot_is_bgra(format f) {
    return f == format::b8g8r8a8_unorm || f == format::b8g8r8a8_unorm_srgb;
}

// Decode one source row to float RGB. unorm formats come out 0..1 exactly as stored (an _srgb
// buffer stays gamma-encoded — we report what the buffer holds, we don't reinterpret it);
// float formats come out true scene-linear. Missing channels are filled the way the buffer's
// own meaning suggests: a 2-channel buffer keeps B = 0 (it IS two channels — normals/motion),
// a 1-channel buffer replicates to grey so it reads as an image rather than a red plate.
static void shot_decode_row(format f, const uint8_t* src, uint32_t w, float* dst)
{
    switch (f) {
        case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb: case format::r8g8b8a8_typeless:
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*3+0] = src[x*4+0] / 255.0f; dst[x*3+1] = src[x*4+1] / 255.0f; dst[x*3+2] = src[x*4+2] / 255.0f;
            } break;
        case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb:
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*3+0] = src[x*4+2] / 255.0f; dst[x*3+1] = src[x*4+1] / 255.0f; dst[x*3+2] = src[x*4+0] / 255.0f;
            } break;
        case format::r10g10b10a2_unorm: {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(src);
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t u = s[x];
                dst[x*3+0] = ( u        & 0x3FF) / 1023.0f;
                dst[x*3+1] = ((u >> 10) & 0x3FF) / 1023.0f;
                dst[x*3+2] = ((u >> 20) & 0x3FF) / 1023.0f;
            } break; }
        case format::r16g16b16a16_unorm: {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*3+0] = s[x*4+0] / 65535.0f; dst[x*3+1] = s[x*4+1] / 65535.0f; dst[x*3+2] = s[x*4+2] / 65535.0f;
            } break; }
        case format::r16g16b16a16_float: {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*3+0] = pv_half_to_float(s[x*4+0]);
                dst[x*3+1] = pv_half_to_float(s[x*4+1]);
                dst[x*3+2] = pv_half_to_float(s[x*4+2]);
            } break; }
        case format::r32g32b32a32_float: {
            const float* s = reinterpret_cast<const float*>(src);
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*3+0] = s[x*4+0]; dst[x*3+1] = s[x*4+1]; dst[x*3+2] = s[x*4+2];
            } break; }
        case format::r11g11b10_float: {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(src);
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t u = s[x];
                dst[x*3+0] = hdr_unpack_f11( u        & 0x7FF);
                dst[x*3+1] = hdr_unpack_f11((u >> 11) & 0x7FF);
                dst[x*3+2] = hdr_unpack_f10((u >> 22) & 0x3FF);
            } break; }
        case format::r9g9b9e5: {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(src);
            for (uint32_t x = 0; x < w; ++x) pv_unpack_rgb9e5(s[x], &dst[x*3+0], &dst[x*3+1], &dst[x*3+2]);
            break; }
        case format::r16g16_float: {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*3+0] = pv_half_to_float(s[x*2+0]); dst[x*3+1] = pv_half_to_float(s[x*2+1]); dst[x*3+2] = 0.0f;
            } break; }
        case format::r16g16_unorm: {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*3+0] = s[x*2+0] / 65535.0f; dst[x*3+1] = s[x*2+1] / 65535.0f; dst[x*3+2] = 0.0f;
            } break; }
        case format::r8g8_unorm:
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*3+0] = src[x*2+0] / 255.0f; dst[x*3+1] = src[x*2+1] / 255.0f; dst[x*3+2] = 0.0f;
            } break;
        case format::r8_unorm:
            for (uint32_t x = 0; x < w; ++x) { const float v = src[x] / 255.0f; dst[x*3+0] = dst[x*3+1] = dst[x*3+2] = v; }
            break;
        case format::r16_unorm: {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
            for (uint32_t x = 0; x < w; ++x) { const float v = s[x] / 65535.0f; dst[x*3+0] = dst[x*3+1] = dst[x*3+2] = v; }
            break; }
        case format::r16_float: {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
            for (uint32_t x = 0; x < w; ++x) { const float v = pv_half_to_float(s[x]); dst[x*3+0] = dst[x*3+1] = dst[x*3+2] = v; }
            break; }
        case format::r32_float: {
            const float* s = reinterpret_cast<const float*>(src);
            for (uint32_t x = 0; x < w; ++x) { const float v = s[x]; dst[x*3+0] = dst[x*3+1] = dst[x*3+2] = v; }
            break; }
        default:
            for (uint32_t x = 0; x < w; ++x) dst[x*3+0] = dst[x*3+1] = dst[x*3+2] = 0.0f;
            break;
    }
}

// ---- One captured source, and one press' worth of them ----------------------------------
struct ShotItem {
    char     tag[96]  = "";        // filename tag ("screen", "depth", "MXAO", ...)
    char     what[160] = "";       // human description for the sidecar
    resource staging  = {};        // gpu_to_cpu copy of the source (released at drain)
    bool     use_texture = true;   // texture staging (DX) vs buffer staging (GL/VK), as in the recorder
    uint32_t w = 0, h = 0, pitch = 0, bpp = 0;
    format   fmt = format::unknown;
    float    exposure = 0.0f;      // the pass's applied exposure, baked into the PNG preview only
    bool     is_screen = false;
    std::vector<uint8_t> px;       // raw source pixels, still GPU-padded (pitch), filled at drain
};

struct ShotBatch {
    std::vector<ShotItem> items;
    ShotConfig  cfg;
    std::string stem;       // "<game>_<YYYY-MM-DD HH-MM-SS>"
    std::string suffix;     // "" or "_f02" for burst frames
    bool        burst = false;
    // Sources that couldn't be grabbed (unsupported format, MSAA, nothing bound this frame). Each
    // one logs its own reason as it happens, but only the count survives to the final verdict —
    // without it a shot silently writes 4 files when you were expecting 5.
    int         skipped = 0;
};

// ---- Capture state (render thread only) --------------------------------------------------
enum ShotStage : int { SHOT_IDLE = 0, SHOT_WAIT, SHOT_GRAB, SHOT_DRAIN };
static int        g_shot_stage = SHOT_IDLE;
static ShotBatch  g_shot_batch;
static fence      g_shot_fence = {};
static uint64_t   g_shot_fence_val = 0;
static std::chrono::high_resolution_clock::time_point g_shot_due;  // when the next frame of the sequence fires
static int        g_shot_left  = 0;   // frames still owed by this burst
static int        g_shot_index = 0;   // 1-based index of the frame being taken
static std::string g_shot_stem;       // session stem, fixed for the whole burst
// When the shutter fired, for the screen flash. Set on the grab frame (not when the file lands),
// so the flash marks the moment that is actually in the picture.
static std::chrono::high_resolution_clock::time_point g_shot_flash_t;

// ---- Result reporting (writer thread -> UI) ---------------------------------------------
// Same discipline as the encoder report: never predictive, always names what actually happened.
static std::mutex  g_shot_msg_mx;
static std::string g_shot_msg;
static bool        g_shot_msg_err = false;
static std::atomic<int> g_shot_busy{ 0 };  // batches queued or being written ("Saving..." in the UI)
static std::chrono::high_resolution_clock::time_point g_shot_msg_t;

static void shot_report(bool err, const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    // _TRUNCATE, never the plain secure CRT call: these interpolate user paths and technique names,
    // and sprintf_s ABORTS THE GAME on overflow.
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lk(g_shot_msg_mx);
        g_shot_msg = buf;
        g_shot_msg_err = err;
        g_shot_msg_t = std::chrono::high_resolution_clock::now();
    }
    char line[600];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "MultipassRecorder still: %s", buf);
    reshade::log::message(err ? reshade::log::level::error : reshade::log::level::info, line);
}

// ---- Writer thread -----------------------------------------------------------------------
// ONE background thread drains a queue of batches, so a 10-frame burst can never have ten
// FFmpeg children alive at once, and the render thread never waits on file I/O.
static std::mutex              g_shotq_mx;
static std::condition_variable g_shotq_cv;
static std::queue<ShotBatch>   g_shotq;
static std::thread             g_shot_thread;
static bool                    g_shot_thread_stop = false;

static std::string shot_dir(bool create) {
    std::string d = captures_dir(create) + "\\stills";
    if (create) CreateDirectoryA(d.c_str(), nullptr);
    return d;
}

static const char* shot_ext(int fmt) {
    switch (fmt) {
        case SHOTF_JPEG:      return "jpg";
        case SHOTF_TIFF16:    return "tif";
        case SHOTF_EXR_HALF:
        case SHOTF_EXR_FLOAT: return "exr";
        default:              return "png";
    }
}
static bool shot_fmt_is_exr(int fmt) { return fmt == SHOTF_EXR_HALF || fmt == SHOTF_EXR_FLOAT; }

// Which format this item is actually written in: float buffers follow the HDR policy, everything
// else follows the screen/pass unorm policy. A screen grab on an HDR swapchain (rgba16f /
// r10g10b10a2 with float content) therefore upgrades itself rather than being crushed to 8-bit.
static int shot_target_fmt(const ShotConfig& c, const ShotItem& it) {
    if (shot_kind(it.fmt) == SK_FLOAT) return c.hdr_fmt;
    return it.is_screen ? c.screen_fmt : c.ldr_fmt;
}

// Build the raw frame FFmpeg is fed, and say which pix_fmt it is.
// Fast path: 8-bit 4-channel source into an 8-bit target is a pitch strip and nothing else — no
// decode, no rounding, the bytes the GPU produced are the bytes that reach the encoder.
static void shot_build_raw(const ShotItem& it, int target, std::vector<uint8_t>& out, const char** in_pf)
{
    const uint32_t w = it.w, h = it.h;
    const size_t   n = (size_t)w * h;
    const ShotKind k = shot_kind(it.fmt);
    const bool     want8   = (target == SHOTF_PNG8 || target == SHOTF_JPEG);
    const bool     want_exr = shot_fmt_is_exr(target);

    if (k == SK_LDR8 && want8) {                       // passthrough
        *in_pf = shot_is_bgra(it.fmt) ? "bgra" : "rgba";
        out.resize(n * 4);
        for (uint32_t y = 0; y < h; ++y)
            memcpy(out.data() + (size_t)y * w * 4, it.px.data() + (size_t)y * it.pitch, (size_t)w * 4);
        return;
    }

    std::vector<float> row((size_t)w * 3);
    if (want_exr) {
        // gbrpf32le: three planes, G then B then R (verified against a round-trip through the
        // exr encoder). Linear, untouched — no exposure, no curve.
        *in_pf = "gbrpf32le";
        out.resize(n * 3 * sizeof(float));
        float* G = reinterpret_cast<float*>(out.data());
        float* B = G + n;
        float* R = G + n * 2;
        for (uint32_t y = 0; y < h; ++y) {
            shot_decode_row(it.fmt, it.px.data() + (size_t)y * it.pitch, w, row.data());
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                R[i] = row[x*3+0]; G[i] = row[x*3+1]; B[i] = row[x*3+2];
            }
        }
        return;
    }

    if (want8) {
        // Viewable 8-bit. A float source gets the pass's exposure + the Viewfinder's Reinhard/gamma
        // view, so the file matches what you framed; a unorm source is already display-referred.
        *in_pf = "rgb24";
        out.resize(n * 3);
        const float gain = powf(2.0f, it.exposure);
        for (uint32_t y = 0; y < h; ++y) {
            shot_decode_row(it.fmt, it.px.data() + (size_t)y * it.pitch, w, row.data());
            uint8_t* d = out.data() + (size_t)y * w * 3;
            for (uint32_t x = 0; x < w * 3; ++x) {
                float v = row[x];
                const uint16_t u = (k == SK_FLOAT) ? hdr_tonemap_u16(v * gain)
                                                   : (uint16_t)((v < 0 ? 0 : v > 1 ? 1 : v) * 65535.0f + 0.5f);
                d[x] = (uint8_t)(u >> 8);
            }
        }
        return;
    }

    // 16-bit interleaved (PNG16 / TIFF16). A unorm source lands exact; a float source uses the
    // same HDR_PNG_SCALE linear fixed point the PNG-sequence codec already writes, so the same
    // "divide by 4096" recovers linear from either.
    *in_pf = "rgb48le";
    out.resize(n * 3 * sizeof(uint16_t));
    uint16_t* d16 = reinterpret_cast<uint16_t*>(out.data());
    for (uint32_t y = 0; y < h; ++y) {
        shot_decode_row(it.fmt, it.px.data() + (size_t)y * it.pitch, w, row.data());
        uint16_t* d = d16 + (size_t)y * w * 3;
        for (uint32_t x = 0; x < w * 3; ++x) {
            const float v = row[x];
            d[x] = (k == SK_FLOAT) ? hdr_to_u16(v)
                                   : (uint16_t)((v < 0 ? 0 : v > 1 ? 1 : v) * 65535.0f + 0.5f);
        }
    }
}

// Spawn FFmpeg, push one frame down its stdin, wait for it to finish. Returns false with a
// reason in `err` — a still that didn't get written has to say so, same as a dead encoder does.
static bool shot_run_ffmpeg(const char* args, const uint8_t* data, size_t bytes, char* err, size_t errsz)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE pipe_read = nullptr, pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 1024 * 1024)) {
        _snprintf_s(err, errsz, _TRUNCATE, "couldn't create the FFmpeg pipe (error %lu)", GetLastError());
        return false;
    }
    SetHandleInformation(pipe_write, HANDLE_FLAG_INHERIT, 0);

    HANDLE h_log = CreateFileA((captures_dir(true) + "\\ffmpeg_log.txt").c_str(),
                               FILE_APPEND_DATA, FILE_SHARE_READ, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE h_nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdInput  = pipe_read;
    si.hStdError  = (h_log != INVALID_HANDLE_VALUE) ? h_log : h_nul;
    si.hStdOutput = h_nul;
    si.dwFlags    = STARTF_USESTDHANDLES;

    // CreateProcessA writes to its command line, so hand it a mutable copy.
    std::vector<char> cmdline(args, args + strlen(args) + 1);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            _snprintf_s(err, errsz, _TRUNCATE, "FFmpeg not found at '%s' - set the path in the Setup tab", g_ffmpeg_path);
        else
            _snprintf_s(err, errsz, _TRUNCATE, "FFmpeg didn't start (error %lu)", e);
        CloseHandle(pipe_read); CloseHandle(pipe_write);
        if (h_log != h_nul && h_log != INVALID_HANDLE_VALUE) CloseHandle(h_log);
        CloseHandle(h_nul);
        return false;
    }
    tether_ffmpeg_to_game(pi.hProcess);
    CloseHandle(pipe_read);

    bool ok = true;
    size_t off = 0;
    while (off < bytes) {
        const DWORD chunk = (DWORD)((bytes - off > (size_t)8 * 1024 * 1024) ? 8 * 1024 * 1024 : bytes - off);
        DWORD written = 0;
        if (!WriteFile(pipe_write, data + off, chunk, &written, nullptr) || written == 0) {
            _snprintf_s(err, errsz, _TRUNCATE, "FFmpeg closed its input early (error %lu) - see captures\\ffmpeg_log.txt",
                        GetLastError());
            ok = false;
            break;
        }
        off += written;
    }
    CloseHandle(pipe_write);   // EOF: FFmpeg flushes and exits

    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code) && code != 0) {
            _snprintf_s(err, errsz, _TRUNCATE, "FFmpeg exited (code %lu) - see captures\\ffmpeg_log.txt", code);
            ok = false;
        }
    } else {
        TerminateProcess(pi.hProcess, 1);
        _snprintf_s(err, errsz, _TRUNCATE, "FFmpeg didn't finish within 60s");
        ok = false;
    }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (h_log != h_nul && h_log != INVALID_HANDLE_VALUE) CloseHandle(h_log);
    CloseHandle(h_nul);
    return ok;
}

// Encode one buffer to one file.
static bool shot_write_one(const ShotBatch& b, const ShotItem& it, int target, bool preview,
                           const std::string& dir, std::string* out_name, char* err, size_t errsz)
{
    std::vector<uint8_t> raw;
    const char* in_pf = "rgba";
    shot_build_raw(it, target, raw, &in_pf);
    if (raw.empty()) { _snprintf_s(err, errsz, _TRUNCATE, "nothing to encode"); return false; }

    char name[320];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "%s%s_%s%s.%s",
                b.stem.c_str(), b.suffix.c_str(), it.tag, preview ? "_preview" : "", shot_ext(target));
    const std::string path = dir + "\\" + name;

    char enc[256];
    switch (target) {
        case SHOTF_PNG16:
            strcpy_s(enc, "-c:v png -pix_fmt rgb48be");
            break;
        case SHOTF_TIFF16:
            strcpy_s(enc, "-c:v tiff -pix_fmt rgb48le -compression_algo lzw");
            break;
        case SHOTF_JPEG: {
            int q = b.cfg.jpeg_q; if (q < 1) q = 1; else if (q > 31) q = 31;
            _snprintf_s(enc, sizeof(enc), _TRUNCATE, "-c:v mjpeg -q:v %d -pix_fmt yuvj444p", q);
            break; }
        case SHOTF_EXR_HALF: case SHOTF_EXR_FLOAT: {
            int c = b.cfg.exr_comp; if (c < 0 || c > 3) c = 3;
            _snprintf_s(enc, sizeof(enc), _TRUNCATE, "-c:v exr -compression %s -format %s",
                        SHOT_EXR_COMP[c], target == SHOTF_EXR_HALF ? "half" : "float");
            break; }
        default:
            strcpy_s(enc, "-c:v png -pix_fmt rgb24");
            break;
    }

    // -update 1: tell the image2 muxer this filename is one image, not a numbered sequence.
    char args[2048];
    _snprintf_s(args, sizeof(args), _TRUNCATE,
                "\"%s\" -y -hide_banner -loglevel error -f rawvideo -pix_fmt %s -s %ux%u -i pipe:0 %s -frames:v 1 -update 1 \"%s\"",
                g_ffmpeg_path, in_pf, it.w, it.h, enc, path.c_str());

    if (!shot_run_ffmpeg(args, raw.data(), raw.size(), err, errsz)) return false;
    *out_name = name;
    return true;
}

// Human-readable note of what a written file actually contains — the difference between an
// archival linear plate and a baked view is exactly the thing you forget a week later.
static const char* shot_fmt_note(int target, ShotKind k, bool preview) {
    if (preview)              return "tonemapped view (exposure baked) - for viewing, not grading";
    if (shot_fmt_is_exr(target)) return "scene-linear, no exposure or curve applied";
    if (target == SHOTF_PNG16 || target == SHOTF_TIFF16)
        return k == SK_FLOAT ? "linear fixed point - divide by 4096 for linear" : "exact 16-bit, as stored";
    if (k == SK_FLOAT)        return "tonemapped view (exposure baked)";
    return "as stored in the buffer";
}

static void shot_write_batch(ShotBatch& b)
{
    // Count the files first: whether a shot earns its own folder depends on it.
    int planned = 0;
    for (const auto& it : b.items) {
        const int t = shot_target_fmt(b.cfg, it);
        planned++;
        if (shot_fmt_is_exr(t) && b.cfg.hdr_preview) planned++;
    }
    std::string dir = shot_dir(true);
    const bool own_dir = (b.cfg.folder == SHOTDIR_ALWAYS) ||
                         (b.cfg.folder == SHOTDIR_MULTI && (planned > 1 || b.burst));
    if (own_dir) {
        dir += "\\" + b.stem;
        CreateDirectoryA(dir.c_str(), nullptr);
    }

    std::string sidecar;
    int written = 0, failed = 0;
    std::string first_name, last_err;

    for (const auto& it : b.items) {
        const int      target = shot_target_fmt(b.cfg, it);
        const ShotKind k      = shot_kind(it.fmt);
        char err[256] = ""; std::string name;

        if (shot_write_one(b, it, target, false, dir, &name, err, sizeof(err))) {
            written++;
            if (first_name.empty()) first_name = name;
            char line[640];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "  %-28s %-9s %ux%u  %s\r\n      source: %s\r\n      note  : %s\r\n",
                        name.c_str(), probe_format_name(it.fmt), it.w, it.h,
                        (k == SK_FLOAT) ? "HDR float" : (k == SK_LDR8 ? "8-bit" : "unorm"),
                        it.what, shot_fmt_note(target, k, false));
            sidecar += line;
        } else {
            failed++; last_err = err;
            char line[640];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "  %-28s FAILED - %s\r\n", it.tag, err);
            sidecar += line;
        }

        // Viewable companion for a linear EXR.
        if (shot_fmt_is_exr(target) && b.cfg.hdr_preview) {
            char perr[256] = ""; std::string pname;
            if (shot_write_one(b, it, SHOTF_PNG8, true, dir, &pname, perr, sizeof(perr))) {
                written++;
                char line[640];
                _snprintf_s(line, sizeof(line), _TRUNCATE, "  %-28s preview   %+.2f st\r\n      note  : %s\r\n",
                            pname.c_str(), it.exposure, shot_fmt_note(SHOTF_PNG8, k, true));
                sidecar += line;
            } else { failed++; last_err = perr; }
        }
    }

    if (b.cfg.sidecar && written > 0) {
        char head[512];
        _snprintf_s(head, sizeof(head), _TRUNCATE,
                    "MultipassRecorder %s - still capture\r\ngame    : %s\r\nshot    : %s%s\r\nfiles   : %d\r\n\r\n",
                    g_version, g_game_tag, b.stem.c_str(), b.suffix.c_str(), written);
        const std::string txt = std::string(head) + sidecar;
        const std::string tpath = dir + "\\" + b.stem + b.suffix + "_info.txt";
        HANDLE h = CreateFileA(tpath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr = 0; WriteFile(h, txt.data(), (DWORD)txt.size(), &wr, nullptr);
            CloseHandle(h);
        }
    }

    // Final verdict. Skips are folded in here (each already logged its own reason) so a shot that
    // wrote fewer files than you expected says so, instead of reading as a clean success.
    char skip[64] = "";
    if (b.skipped > 0)
        _snprintf_s(skip, sizeof(skip), _TRUNCATE, ", %d source%s skipped",
                    b.skipped, b.skipped == 1 ? "" : "s");
    const std::string where = own_dir ? (b.stem + "\\") : std::string("stills\\");
    if (failed == 0 && b.skipped == 0)
        shot_report(false, "saved %d file%s to %s", written, written == 1 ? "" : "s", where.c_str());
    else if (failed == 0)
        shot_report(true, "saved %d file%s to %s%s - see the log for which", written,
                    written == 1 ? "" : "s", where.c_str(), skip);
    else if (written > 0)
        shot_report(true, "saved %d file%s, %d failed%s - %s", written, written == 1 ? "" : "s",
                    failed, skip, last_err.c_str());
    else
        shot_report(true, "nothing was saved%s - %s", skip,
                    last_err.empty() ? "no source could be written" : last_err.c_str());
}

static void shot_writer_func() {
    for (;;) {
        ShotBatch b;
        {
            std::unique_lock<std::mutex> lk(g_shotq_mx);
            g_shotq_cv.wait(lk, [] { return !g_shotq.empty() || g_shot_thread_stop; });
            if (g_shotq.empty()) return;         // stop requested and drained
            b = std::move(g_shotq.front());
            g_shotq.pop();
        }
        shot_write_batch(b);
        g_shot_busy.fetch_sub(1, std::memory_order_relaxed);
    }
}

static void shot_enqueue(ShotBatch&& b) {
    g_shot_busy.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_shotq_mx);
        g_shotq.push(std::move(b));
    }
    // Started lazily and joined in AddonUninit — a detached thread could still be writing when
    // the DLL unloads, which is exactly the class of bug the recorder's worker join avoids.
    if (!g_shot_thread.joinable()) g_shot_thread = std::thread(shot_writer_func);
    g_shotq_cv.notify_one();
}

static void shot_shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_shotq_mx);
        g_shot_thread_stop = true;
    }
    g_shotq_cv.notify_all();
    if (g_shot_thread.joinable()) g_shot_thread.join();
}

// ---- Source acquisition (render thread) --------------------------------------------------
// A pass's source resource for THIS frame. Extracted from on_reshade_present's Phase 1 so the
// still path and the recording path can never disagree about what a pass points at.
static resource pass_source_resource(effect_runtime* rt, device* dev, const RecordPass& p)
{
    if (p.source == SRC_NAMED) {
        effect_texture_variable var = rt->find_texture_variable(nullptr, p.tex_name);
        if (var == 0) return { 0 };
        resource_view srv = { 0 };
        rt->get_texture_binding(var, &srv, nullptr);
        if (srv.handle == 0) return { 0 };
        return dev->get_resource_from_view(srv);
    }
    if (p.source == SRC_FX_LAYER) {
        // Rendered into this pass's own target earlier in the frame (mpr_fxlayer.inc).
        return p.fx_rt;
    }
    if (src_is_native(p.source)) {
        // Computed into this pass's own target earlier in the frame (mpr_native.inc): world at
        // begin_effects, depth/normals at finish_effects. Either way it is ready by present.
        return p.native_rt;
    }
    // SRC_ENGINE_RT: fingerprint resolved against the live registry, this frame only.
    resource tex = { 0 };
    tex.handle = resolve_engine_rt(p);
    return tex;
}

// Copy one source into its own staging resource and remember what it was. `barriers` is for
// sources the runtime hands us mid-frame in render_target state (the back buffer at
// finish_effects); pass sources follow the recorder's convention and are copied as-is.
static bool shot_add_source(device* dev, command_list* cmd, resource tex, const char* tag,
                            const char* what, float exposure, bool is_screen, bool barriers)
{
    if (tex.handle == 0) return false;
    const resource_desc d = dev->get_resource_desc(tex);
    const format f = format_to_default_typed(d.texture.format);
    const uint32_t bpp = format_bpp(f);

    if (d.texture.samples > 1) {
        g_shot_batch.skipped++;
        shot_report(true, "'%s' skipped - multisampled buffers can't be copied to a still.", tag);
        return false;
    }
    if (shot_kind(f) == SK_UNSUPPORTED || bpp == 0) {
        g_shot_batch.skipped++;
        shot_report(true, "'%s' skipped - %s isn't a format this can write.", tag, probe_format_name(f));
        return false;
    }

    ShotItem it;
    strncpy_s(it.tag,  tag,  _TRUNCATE);
    strncpy_s(it.what, what, _TRUNCATE);
    it.w = d.texture.width; it.h = d.texture.height;
    it.bpp = bpp; it.fmt = f; it.exposure = exposure; it.is_screen = is_screen;
    it.pitch = (it.w * bpp + 3) & ~3u;
    it.use_texture = (dev->get_api() != device_api::opengl && dev->get_api() != device_api::vulkan);

    resource_desc sd;
    if (it.use_texture) {
        sd = d;
        sd.type  = resource_type::texture_2d;
        sd.heap  = memory_heap::gpu_to_cpu;
        sd.usage = resource_usage::copy_dest;
        sd.flags = resource_flags::none;
        sd.texture.levels = 1;
        sd.texture.depth_or_layers = 1;
    } else {
        sd = resource_desc((uint64_t)it.pitch * it.h, memory_heap::gpu_to_cpu, resource_usage::copy_dest);
    }
    if (!dev->create_resource(sd, nullptr, resource_usage::copy_dest, &it.staging)) {
        g_shot_batch.skipped++;
        shot_report(true, "'%s' skipped - couldn't create a staging buffer for it.", tag);
        return false;
    }

    if (barriers) {
        const resource_usage from = resource_usage::render_target, to = resource_usage::copy_source;
        cmd->barrier(1, &tex, &from, &to);
    }
    if (it.use_texture) cmd->copy_texture_region(tex, 0, nullptr, it.staging, 0, nullptr, filter_mode::min_mag_mip_point);
    else                cmd->copy_texture_to_buffer(tex, 0, nullptr, it.staging, 0, 0, 0);
    if (barriers) {
        const resource_usage from = resource_usage::copy_source, to = resource_usage::render_target;
        cmd->barrier(1, &tex, &from, &to);
    }

    g_shot_batch.items.push_back(std::move(it));
    return true;
}

// Release anything a pending shot is holding (device teardown / cancel).
static void shot_free(device* dev) {
    for (auto& it : g_shot_batch.items)
        if (it.staging.handle && dev) dev->destroy_resource(it.staging);
    g_shot_batch = ShotBatch{};
    if (g_shot_fence.handle && dev) { dev->destroy_fence(g_shot_fence); g_shot_fence = { 0 }; }
    g_shot_stage = SHOT_IDLE;
    g_shot_left = 0;
    g_shot_pending = false;
    g_shot_capture_frame = 0xffffffff;
}

// ---- The sequence ------------------------------------------------------------------------
static bool shot_is_busy() { return g_shot_stage != SHOT_IDLE || g_shot_busy.load(std::memory_order_relaxed) > 0; }

// Start a still sequence (hotkey or the panel button). One sequence at a time: a second press
// while frames are still owed would interleave two bursts into the same folder.
static void shot_trigger() {
    if (g_shot_stage != SHOT_IDLE) return;
    if (!g_shotcfg.src_screen && !g_shotcfg.src_passes) {
        shot_report(true, "nothing selected to capture - tick Screen or Passes in the Stills tab.");
        return;
    }
    time_t t = time(nullptr);
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &tm_info);
    char stem[224];
    _snprintf_s(stem, sizeof(stem), _TRUNCATE, "%s_%s", g_game_tag, stamp);
    g_shot_stem = stem;

    g_shot_left  = g_shotcfg.burst < 1 ? 1 : (g_shotcfg.burst > 60 ? 60 : g_shotcfg.burst);
    g_shot_index = 0;
    const int delay = g_shotcfg.timer_sec < 0 ? 0 : (g_shotcfg.timer_sec > 60 ? 60 : g_shotcfg.timer_sec);
    g_shot_due   = std::chrono::high_resolution_clock::now() + std::chrono::seconds(delay);
    g_shot_stage = SHOT_WAIT;
    g_shot_pending = true;   // keep the live-RT registry enumerating for engine-buffer passes
    if (delay > 0)
        shot_report(false, "self-timer: %d s...", delay);
    else if (g_shot_left > 1)
        shot_report(false, "burst of %d...", g_shot_left);
}

// Seconds left on the self-timer (UI countdown), or 0.
static float shot_timer_left() {
    if (g_shot_stage != SHOT_WAIT) return 0.0f;
    const auto now = std::chrono::high_resolution_clock::now();
    const double s = std::chrono::duration<double>(g_shot_due - now).count();
    return s > 0 ? (float)s : 0.0f;
}

// WAIT -> GRAB. Called early in on_reshade_present, before g_live_enum is recomputed, so the
// registry is already armed for the frame we're about to grab.
static void shot_arm_tick() {
    if (g_shot_stage != SHOT_WAIT) return;
    if (std::chrono::high_resolution_clock::now() < g_shot_due) return;
    // Two frames of lead: the frame after this present is the first one whose binds the live-RT
    // registry will have seen with the hooks on, and engine-RT passes resolve against exactly
    // that. One frame would work when the panel is already open; two always does.
    g_shot_capture_frame = g_live_frame + 2;
    g_shot_stage = SHOT_GRAB;
    g_shot_index++;
    g_shot_batch = ShotBatch{};
    g_shot_batch.cfg   = g_shotcfg;
    g_shot_batch.stem  = g_shot_stem;
    g_shot_batch.burst = (g_shotcfg.burst > 1);
    if (g_shot_batch.burst) {
        char sfx[16]; _snprintf_s(sfx, sizeof(sfx), _TRUNCATE, "_f%02d", g_shot_index);
        g_shot_batch.suffix = sfx;
    }
}

// The screen half of the grab: the game view AFTER ReShade's effect chain but BEFORE any UI, so
// the ReShade menu, our OSD, Debug View and the composition guides are all absent from the pixels
// (they are ImGui draw lists rendered later in the frame).
static void on_reshade_finish_effects(effect_runtime* rt, command_list* cmd, resource_view rtv, resource_view)
{
    if (g_fx_in_render || rtv.handle == 0) return;               // our own render_technique re-enters here

    device* dev = rt->get_device();
    const resource back = dev->get_resource_from_view(rtv);
    if (back.handle == 0) return;
    // Only the GAME's frame: this event also fires when another addon drives render_effects into
    // a target of its own (same guard as the FX-layer hook). Hoisted ABOVE the still-capture
    // guards below, because the native depth/normals dispatch needs it too.
    uint32_t rw = 0, rh = 0;
    rt->get_screenshot_width_and_height(&rw, &rh);
    const resource_desc bd = dev->get_resource_desc(back);
    if (rw == 0 || bd.texture.width != rw || bd.texture.height != rh) return;

    // Native depth/normals run EVERY frame the cadence asks for, not just on a still's grab frame.
    // This call must stay above the SHOT_GRAB guards: below them it would only ever fire while a
    // screenshot was being taken, so the depth and normals passes would record nothing at all.
    // Here (after the visible chain) is also the only place texDepthRaw holds THIS frame's depth.
    native_depth_normals_tick(rt, dev);
    // The raw-source readout behind the depth controls. Same hook for the same reason (texDepthRaw
    // is only this frame's depth here), and it self-gates on having been asked for — a closed panel
    // costs one comparison. It must NOT sit under the guards below either: the whole point is to
    // report on a pass that is currently producing nothing.
    native_probe_tick(rt, dev);

    if (g_shot_stage != SHOT_GRAB || g_live_frame != g_shot_capture_frame) return;
    if (!g_shot_batch.cfg.src_screen) return;
    for (const auto& it : g_shot_batch.items) if (it.is_screen) return; // already grabbed this frame

    shot_add_source(dev, cmd, back, "screen", "game view, after ReShade effects, before any overlay",
                    0.0f, /*is_screen=*/true, /*barriers=*/true);
}

// The passes half of the grab, then hand the whole frame off to the fence. Called from
// on_reshade_present AFTER the recorder's own phases, so a still never delays a capture beat.
static void shot_grab_tick(effect_runtime* rt) {
    if (g_shot_stage != SHOT_GRAB || g_live_frame != g_shot_capture_frame) return;

    device*        dev = rt->get_device();
    command_queue* q   = rt->get_command_queue();
    command_list*  cmd = q->get_immediate_command_list();

    if (g_shot_batch.cfg.src_passes) {
        for (auto& p : g_passes) {
            if (!p.enabled) continue;
            if (p.is_custom && !p.promoted) continue;
            char tag[96]; pass_file_tag(p, tag, sizeof(tag));
            char what[160];
            switch (p.source) {
                case SRC_NAMED:
                    _snprintf_s(what, sizeof(what), _TRUNCATE, "pass '%s' - ReShade texture %s", p.name, p.tex_name);
                    break;
                case SRC_FX_LAYER:
                    _snprintf_s(what, sizeof(what), _TRUNCATE, "pass '%s' - effect layer %s", p.name, p.fx_tech);
                    break;
                // Without these three the native passes fell through to the engine-buffer default
                // below and the sidecar .txt described a computed depth map as an "engine buffer".
                case SRC_NATIVE_DEPTH:
                    _snprintf_s(what, sizeof(what), _TRUNCATE,
                                "pass '%s' - linearized depth, computed by the addon from ReShade's depth buffer "
                                "(far plane %.1f%s)", p.name, p.native_far_plane,
                                p.native_linear_output ? ", scene-linear output" : "");
                    break;
                case SRC_NATIVE_NORMALS:
                    _snprintf_s(what, sizeof(what), _TRUNCATE,
                                "pass '%s' - screen-space normals, reconstructed by the addon from depth (%.1f deg vertical FOV)",
                                p.name, p.native_use_fov ? p.native_fov : 0.0f);
                    break;
                case SRC_NATIVE_WORLD:
                    _snprintf_s(what, sizeof(what), _TRUNCATE,
                                "pass '%s' - game view before ReShade's effect chain (clean plate)", p.name);
                    break;
                default:
                    _snprintf_s(what, sizeof(what), _TRUNCATE, "pass '%s' - engine buffer %s",
                                p.name, p.label[0] ? p.label : probe_format_name(p.rt_fmt));
                    break;
            }
            const resource tex = pass_source_resource(rt, dev, p);
            if (tex.handle == 0) {
                g_shot_batch.skipped++;
                shot_report(true, "'%s' skipped - its source wasn't available this frame.", tag);
                continue;
            }
            shot_add_source(dev, cmd, tex, tag, what, p.enc_exposure, false, false);
        }
    }

    g_shot_capture_frame = 0xffffffff;   // the grab frame is over either way

    if (g_shot_batch.items.empty()) {
        shot_report(true, "nothing was captured - no source was available this frame.");
        g_shot_stage = SHOT_IDLE;
        g_shot_left = 0;
        g_shot_pending = false;
        return;
    }

    if (g_shot_fence.handle == 0 && !dev->create_fence(0, fence_flags::none, &g_shot_fence)) {
        shot_report(true, "couldn't create the readback fence.");
        for (auto& it : g_shot_batch.items) dev->destroy_resource(it.staging);
        g_shot_batch = ShotBatch{};
        g_shot_stage = SHOT_IDLE;
        g_shot_left = 0;
        g_shot_pending = false;
        return;
    }
    // One fence value covers every copy issued this frame, the screen's included: they were all
    // recorded into the same immediate command list, on this queue, before this signal.
    q->signal(g_shot_fence, ++g_shot_fence_val);
    q->flush_immediate_command_list();
    g_shot_stage = SHOT_DRAIN;
    g_shot_flash_t = std::chrono::high_resolution_clock::now();
}

// DRAIN: poll (never block) and, once the GPU is done, copy the pixels out and hand them over.
static void shot_drain_tick(effect_runtime* rt) {
    if (g_shot_stage != SHOT_DRAIN) return;
    device* dev = rt->get_device();
    if (!dev->wait(g_shot_fence, g_shot_fence_val, 0)) return;   // still copying — try again next frame

    for (auto& it : g_shot_batch.items) {
        subresource_data md = {};
        void* ptr = nullptr;
        bool ok;
        if (it.use_texture) {
            ok = dev->map_texture_region(it.staging, 0, nullptr, map_access::read_only, &md);
            ptr = md.data;
            // Same guard as the recorder: an under-reported pitch would read across rows.
            if (md.row_pitch < it.w * it.bpp) md.row_pitch = it.w * it.bpp;
        } else {
            ok = dev->map_buffer_region(it.staging, 0, UINT64_MAX, map_access::read_only, &ptr);
            md.row_pitch = it.pitch;
        }
        if (ok && ptr) {
            it.px.resize((size_t)md.row_pitch * it.h);
            memcpy(it.px.data(), ptr, it.px.size());
            it.pitch = md.row_pitch;
            if (it.use_texture) dev->unmap_texture_region(it.staging, 0);
            else                dev->unmap_buffer_region(it.staging);
        } else {
            shot_report(true, "'%s' couldn't be read back from the GPU.", it.tag);
        }
        dev->destroy_resource(it.staging);
        it.staging = { 0 };
    }
    // Drop anything that failed to map rather than writing a black file for it.
    for (size_t i = g_shot_batch.items.size(); i-- > 0; )
        if (g_shot_batch.items[i].px.empty()) g_shot_batch.items.erase(g_shot_batch.items.begin() + i);

    if (!g_shot_batch.items.empty()) shot_enqueue(std::move(g_shot_batch));
    g_shot_batch = ShotBatch{};

    // Next frame of the burst, or done.
    if (--g_shot_left > 0) {
        int gap = g_shotcfg.burst_ms; if (gap < 0) gap = 0; else if (gap > 10000) gap = 10000;
        g_shot_due   = std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(gap);
        g_shot_stage = SHOT_WAIT;
    } else {
        g_shot_stage = SHOT_IDLE;
        g_shot_pending = false;
    }
}

// ---- On-screen feedback ------------------------------------------------------------------
// A shutter flash and a toast, because a still gives no other sign it happened — unlike a
// recording, there's no badge left on screen afterwards to tell you it worked.
static void shot_draw_feedback()
{
    if (imgui_function_table_instance() == nullptr) return;
    const auto now = std::chrono::high_resolution_clock::now();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;

    if (g_shotcfg.flash) {
        const double age = std::chrono::duration<double>(now - g_shot_flash_t).count();
        if (age >= 0.0 && age < 0.30) {
            const int a = (int)((1.0 - age / 0.30) * 140.0);
            ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(255, 255, 255, a));
        }
    }

    // Self-timer countdown: big, centered, and it has to be visible with the menu closed.
    const float left = shot_timer_left();
    if (left > 0.05f) {
        char cd[16]; _snprintf_s(cd, sizeof(cd), _TRUNCATE, "%d", (int)(left + 0.999f));
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(cd);
        const ImVec2 c(ds.x * 0.5f, ds.y * 0.5f);
        dl->AddCircleFilled(c, 34.0f, IM_COL32(0, 0, 0, 120), 32);
        dl->AddCircle(c, 34.0f, mpr_u32(MPR_WARN), 32, 2.0f);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), MPR_TEXT_HI, cd);
    }

    // Result toast, bottom-center, for a few seconds after the writer reports.
    std::string msg; bool err = false; double age = 1e9;
    {
        std::lock_guard<std::mutex> lk(g_shot_msg_mx);
        if (!g_shot_msg.empty()) {
            msg = g_shot_msg; err = g_shot_msg_err;
            age = std::chrono::duration<double>(now - g_shot_msg_t).count();
        }
    }
    const bool busy = g_shot_busy.load(std::memory_order_relaxed) > 0;
    if (msg.empty() || (age > 4.0 && !busy)) return;

    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y - 42.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    MprThemeScope theme(true); // hud: stays borderless -- see draw_buffer_preview for the Begin ordering
    ImGui::Begin("##MPR_SHOT_TOAST", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_AlwaysAutoResize);
    if (busy) {
        ImGui::TextColored(MPR_INFO, "[o]");
        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted("saving still...");
    } else {
        ImGui::TextColored(err ? MPR_ERR : MPR_OK, err ? "[!]" : "[o]");
        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted(msg.c_str());
    }
    ImGui::End();
}
