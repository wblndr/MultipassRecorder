// === MultipassRecorder — mpr_native.inc ===
// Native passes — depth, normals and world computed by the ADDON, into targets the addon owns,
// with no .fx doing the work.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.
//
// WHY this exists. These three passes used to be rendered by MultipassRecorder.fx, which wrote
// them into RGBA8 textures the addon then read by name. That cost a 32-bit depth buffer all but
// 256 of its levels BEFORE the addon ever saw a pixel — every band in every depth capture was
// created there. It also put the tuning (far plane, reversed Z, FOV, flips) in ReShade's effect
// UI rather than the addon's, where it could not be persisted per pass, and where five of the
// knobs had no addon control at all.
//
// WHAT IS LEFT OF THE .fx. Exactly one thing the addon cannot do for itself: ReShade resolves the
// game's depth buffer internally (the "Generic Depth" logic compiled into reshade64.dll — see
// deps/reshade/examples/09-depth/generic_depth_addon.cpp, built by ReShade.vcxproj) but publishes
// it ONLY to a texture with the DEPTH semantic, which only an EFFECT can declare. So the shrunken
// MultipassRecorder.fx declares that texture, copies the depth buffer into R32F, and stops. This
// file reads it by name and does everything else.
//
// HOW THE FRAME FITS TOGETHER — deliberately TWO different hooks, for two different reasons:
//
//   on_reshade_begin_effects   (mpr_fxlayer.inc's hook, before the visible chain)
//     WORLD copies the back buffer here, because here it is the clean game image: no ReShade
//     effects, no overlay, no addon OSD. That keeps `world` usable as a neutral plate. (It is a
//     deliberate change from the old .fx, whose PS_world ran wherever MultipassRecorder_Setup
//     happened to sit in the user's technique order.)
//
//   on_reshade_finish_effects  (mpr_shot.inc's hook, after the visible chain)
//     DEPTH and NORMALS dispatch here, because texDepthRaw is written BY the visible chain — at
//     begin_effects it still holds the previous frame.
//
// on_reshade_present then copies each native_rt into staging exactly like any other source; the
// whole worker/FFmpeg half of the addon is untouched.
//
// PRECISION, HONESTLY. The depth target is r32_float, and that full precision reaches the
// Viewfinder (format_role -> RTR_MASK, so preview_fmt_gpu accepts it) and stills (shot_kind ->
// SK_FLOAT, written as float EXR) with no conversion. The VIDEO path is the narrow one: NVENC
// tops out at p010le (10-bit), so a recorded depth .mp4 carries 1024 levels, not 65536 — four
// times the old 256, not 256 times. For a genuinely high-precision depth SEQUENCE, use the
// existing 16-bit PNG codec. mpr_capture.inc's worker converts r32_float -> gray16le on the way
// to FFmpeg (there is no LUT for it, unlike the r11g11b10 path: a float32 domain can't be
// tabulated, so it is a scale+clamp per pixel — still on the worker thread, never the render one).

// Reactive report for this subsystem, same contract as g_fx_report: written after something has
// already gone wrong, naming the pass, never predictively.
static std::string g_native_report;

static void native_report(reshade::log::level lvl, const char* fmt, ...) {
    char msg[320]; // _TRUNCATE, never sprintf_s: pass names are user-supplied and it ABORTS on overflow
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    reshade::log::message(lvl, msg);
    g_native_report = msg + sizeof("MultipassRecorder: ") - 1;
}

// ---- The owned target ---------------------------------------------------------------------
// Per-pass latch for a target that could not be BUILT, for the same reason mpr_fxlayer.inc has
// one: without it a failed allocation is retried — and logged — every single frame for the rest
// of the session, under exactly the VRAM pressure that made it fail. Re-arms on native_destroy,
// so a transient failure isn't permanent.
static bool g_native_rt_failed[N_FIXED + N_CUSTOM] = {};
static bool& native_rt_failed(const RecordPass& p) { return g_native_rt_failed[&p - g_passes]; }

static void native_destroy(device* dev, RecordPass& p) {
    if (p.native_srv.handle) { dev->destroy_resource_view(p.native_srv); p.native_srv = { 0 }; }
    if (p.native_uav.handle) { dev->destroy_resource_view(p.native_uav); p.native_uav = { 0 }; }
    if (p.native_rt.handle)  { dev->destroy_resource(p.native_rt);       p.native_rt  = { 0 }; }
    p.native_w = p.native_h = 0;
    p.native_fmt = format::unknown;
    native_rt_failed(p) = false;
}
// Forward-declared so native_free_all can drop the compiled shaders too — they belong to the same
// device as the targets, so anything that frees one must free the other.
static void native_gpu_free();
static void native_free_all(device* dev) {
    for (auto& p : g_passes) native_destroy(dev, p);
    native_gpu_free();
}

// (Re)create the pass's own target at w*h*f. `need_uav` distinguishes a compute output (depth,
// normals) from a plain copy destination (world). Returns false when it could not be built, and
// latches that so it isn't re-attempted every frame.
static bool native_ensure_rt(device* dev, RecordPass& p, uint32_t w, uint32_t h, format f, bool need_uav) {
    // Only trust the cached answer when the target is COMPLETE — the same trap fx_ensure_rt
    // documents: a half-built target (resource created, view creation failed) otherwise reports
    // success from the second frame on, and hands the runtime a null view in release builds.
    const bool views_ok = p.native_rt.handle != 0 && p.native_srv.handle != 0 &&
                          (!need_uav || p.native_uav.handle != 0);
    if (p.native_w == w && p.native_h == h && p.native_fmt == f && (views_ok || native_rt_failed(p)))
        return !native_rt_failed(p);

    native_destroy(dev, p);
    p.native_w = w; p.native_h = h; p.native_fmt = f;

    resource_usage usage = resource_usage::shader_resource | resource_usage::copy_source |
                           resource_usage::copy_dest;
    if (need_uav) usage |= resource_usage::unordered_access;

    const resource_desc d(w, h, 1, 1, f, 1, memory_heap::gpu_only, usage);
    if (!dev->create_resource(d, nullptr, resource_usage::copy_dest, &p.native_rt)) {
        p.native_rt = { 0 };
        native_rt_failed(p) = true;
        native_report(reshade::log::level::error,
                      "MultipassRecorder: pass '%s' can't capture - couldn't allocate its %ux%u %s target (out of video memory?).",
                      p.name, w, h, probe_format_name(f));
        return false;
    }
    const resource_view_desc vd(f);
    if (!dev->create_resource_view(p.native_rt, resource_usage::shader_resource, vd, &p.native_srv)) {
        // Fatal here, unlike the FX layer's optional preview SRV: every native pass is READ back
        // through this view by the Viewfinder and the glance strip, and depth/normals are computed
        // from a source SRV anyway — a target we can't sample is not half-usable.
        native_destroy(dev, p);
        p.native_w = w; p.native_h = h; p.native_fmt = f;
        native_rt_failed(p) = true;
        native_report(reshade::log::level::error,
                      "MultipassRecorder: pass '%s' can't capture - the driver wouldn't give its %s target a shader-resource view.",
                      p.name, probe_format_name(f));
        return false;
    }
    if (need_uav && !dev->create_resource_view(p.native_rt, resource_usage::unordered_access, vd, &p.native_uav)) {
        native_destroy(dev, p);
        p.native_w = w; p.native_h = h; p.native_fmt = f;
        native_rt_failed(p) = true;
        native_report(reshade::log::level::error,
                      "MultipassRecorder: pass '%s' can't capture - the driver wouldn't give its %s target an unordered-access view.",
                      p.name, probe_format_name(f));
        return false;
    }
    return true;
}

// ---- When a native pass computes ----------------------------------------------------------
// Mirrors fx_should_render (mpr_fxlayer.inc), including its beat PREDICTION while recording: both
// of these hooks run earlier in the frame than on_reshade_present, where the capture cadence is
// actually decided, so erring a fraction of a millisecond early is what keeps a native pass in
// register with the FX layers and the engine buffers. A late one corrupts the take.
static bool native_should_render(const RecordPass& p) {
    if (!src_is_native(p.source)) return false;
    // Being LOOKED at is reason enough, enabled or not — you frame a depth pass in the Viewfinder
    // to decide whether it's worth recording. Affordable here in a way it isn't for an FX layer:
    // this is one dispatch or one copy, not a whole technique.
    if (g_preview_pass == &p && (g_preview_open || g_debug_view)) return true;
    if (!p.enabled) return false;
    if (g_live_frame == g_shot_capture_frame) return true;   // a still is being grabbed THIS frame
    if (g_recording) return fx_beat_due();
    return g_settings_ttl > 0 && (g_live_frame % 4) == 0;    // gallery thumbnails, panel open
}

// ---- WORLD: the clean plate ----------------------------------------------------------------
// Called from on_reshade_begin_effects (mpr_fxlayer.inc) with the back buffer already resolved and
// resolution-checked, so this does not repeat that work. The copy is the whole pass: `world` is
// definitionally "the frame as the game handed it over".
static void native_world_tick(device* dev, command_list* cmd, resource back, const resource_desc& bd)
{
    for (auto& p : g_passes) {
        if (p.source != SRC_NATIVE_WORLD || !native_should_render(p)) continue;

        const format bf = format_to_default_typed(bd.texture.format);
        // The encode path understands 8-bit 4-channel and packed r11g11b10; reuse the FX layer's
        // classification rather than restating it, since a world plate IS a back-buffer copy and
        // inherits exactly the same format question.
        if (!fx_format_supported(bf)) {
            native_report(reshade::log::level::warning,
                          "MultipassRecorder: pass '%s' can't record - the back buffer is %s, which the encode path can't write.",
                          p.name, probe_format_name(bf));
            continue;
        }
        p.ffmpeg_fmt = fx_ffmpeg_fmt(bf);
        if (!native_ensure_rt(dev, p, bd.texture.width, bd.texture.height, bf, /*need_uav=*/false))
            continue;

        const resource       res[2]    = { back, p.native_rt };
        const resource_usage old_st[2] = { resource_usage::render_target, resource_usage::copy_dest };
        const resource_usage new_st[2] = { resource_usage::copy_source,   resource_usage::copy_dest };
        cmd->barrier(2, res, old_st, new_st);
        cmd->copy_texture_region(back, 0, nullptr, p.native_rt, 0, nullptr, filter_mode::min_mag_mip_point);
        cmd->barrier(2, res, new_st, old_st);

        p.native_last_render_frame = g_live_frame;
    }
}

// ---- DEPTH + NORMALS: the compute path -----------------------------------------------------
// Native D3D11, for the same reasons mpr_preview.inc gives: reshade::api has no compute pipeline
// or state save/restore, and every reshade handle IS the native COM pointer on the D3D11 backend,
// so interop is a cast. DX11 is the only tested target; other APIs simply don't get these passes.

// Layout of the probe's structured buffer. THE HLSL ABOVE INDEXES THESE AS BARE LITERALS because
// it cannot see this enum — the index is written beside each line there so the two can be checked
// against each other by eye, same discipline as NativeFlag below. Append only.
enum NativeProbeSlot : uint32_t {
    NPB_BINS  = 256,   // [0..255] histogram of saturate(raw) over 0..1
    NPB_LO    = 256,   // count of samples < 0.001
    NPB_HI    = 257,   // count of samples > 0.999
    NPB_OOB   = 258,   // count of samples > 1.0
    NPB_MINC  = 259,   // complement of the minimum's float bit pattern
    NPB_MAX   = 260,   // maximum's float bit pattern
    NPB_SUM   = 261,   // sum of saturate(raw) * 4096
    NPB_UINTS = 262,
};
static const uint32_t NP_SAMPLES = 256 * 256; // the probe's fixed grid — the shader's thread count

struct NativeGpu {
    bool tried = false, ok = false;
    ID3D11ComputeShader* cs_depth   = nullptr;
    ID3D11ComputeShader* cs_normals = nullptr;
    ID3D11Buffer*        cb         = nullptr;
    // Probe (optional): its failure must never take depth/normals down with it, so `ok` above does
    // not cover these — every use site tests cs_probe itself.
    ID3D11ComputeShader*       cs_probe    = nullptr;
    ID3D11Buffer*              probe_buf   = nullptr;
    ID3D11UnorderedAccessView* probe_uav   = nullptr;
    ID3D11Buffer*              probe_rb[2] = {};   // staging ping-pong, same shape as the preview's
    bool                       rb_pending[2] = {};
    int                        rb_next     = 0;
};
static NativeGpu g_ng;

// Mirror of the HLSL cbuffer below (two 16-byte rows).
struct NativeParams {
    float    far_plane, depth_mul, fov, normal_mul;
    uint32_t w, h, flags, pad;
};
// Flag bits. THE HLSL BELOW TESTS THESE AS BARE LITERALS (`g_flags & 32u`) because it cannot see
// this enum — the decimal value is written beside each one so the two can be checked against each
// other by eye. Renumbering here without editing the shader silently swaps which control does what,
// which looks like a maths bug rather than a wiring one. Add new flags at the END.
enum NativeFlag : uint32_t {
    NF_REVERSED    = 1u << 0, // 1
    NF_UPSIDE_DOWN = 1u << 1, // 2
    NF_LOG_CURVE   = 1u << 2, // 4
    NF_REVERSE_LUMA= 1u << 3, // 8
    NF_LINEAR_OUT  = 1u << 4, // 16
    NF_USE_FOV     = 1u << 5, // 32
    NF_FLIP_X      = 1u << 6, // 64
    NF_FLIP_Y      = 1u << 7, // 128
    NF_FLIP_Z      = 1u << 8, // 256
};

// Both entry points share linearize(), which is a direct port of the old MultipassRecorder.fx
// Getdepth(). They declare their outputs on DIFFERENT UAV slots so one source file can hold both
// (u0 float for depth, u1 unorm float4 for normals); each compile strips the one it doesn't use,
// and the dispatch binds only its own slot.
static const char NATIVE_CS_HLSL[] = R"(
cbuffer NpParams : register(b0) {
    float g_far, g_depth_mul, g_fov, g_normal_mul;
    uint  g_w, g_h, g_flags, g_pad;
};
Texture2D<float>           S  : register(t0);
RWTexture2D<float>         DD : register(u0);
RWTexture2D<unorm float4>  DN : register(u1);
RWStructuredBuffer<uint>   PB : register(u2);   // probe_cs only — see NativeProbeSlot in the C++

// Raw depth at a pixel, honouring the upside-down flip on the READ (as the original did on uv.y).
float raw_at(int2 p) {
    p = clamp(p, int2(0, 0), int2(g_w - 1, g_h - 1));
    if (g_flags & 2u) p.y = (int)(g_h - 1) - p.y;
    return S.Load(int3(p, 0)).x;
}

// Port of Getdepth(): multiplier -> optional log curve -> optional reversed-Z -> reverse
// projection into 0..1 -> optional luma inversion. With NF_LINEAR_OUT the 0..1 remap is scaled
// back out to world units and the cosmetic steps (luma inversion, saturate) are skipped, so the
// pass carries true scene-linear distance with nothing baked in.
float linearize(int2 p) {
    float d = raw_at(p) * g_depth_mul;
    if (g_flags & 4u) { const float C = 0.01; d = (exp(d * log(C + 1.0)) - 1.0) / C; }
    if (g_flags & 1u) d = 1.0 - d;
    const float N = 1.0;
    d /= g_far - d * (g_far - N);
    if (g_flags & 16u) return d * g_far;          // scene-linear, unclamped
    if (g_flags & 8u)  d = 1.0 - d;
    return saturate(d);
}

[numthreads(8, 8, 1)]
void depth_cs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_w || id.y >= g_h) return;
    DD[id.xy] = linearize(int2(id.xy));
}

// Port of Getnormal(): three taps (centre / north / east) re-linearized independently — NOT read
// back from the depth pass's output, exactly as the original did, so the normals pass is
// self-contained and its own depth settings apply.
[numthreads(8, 8, 1)]
void normals_cs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_w || id.y >= g_h) return;
    int2 p = int2(id.xy);
    float2 inv = float2(1.0 / g_w, 1.0 / g_h);
    float2 uvC = (float2(p) + 0.5) * inv;
    float2 uvN = uvC - float2(0.0, inv.y);
    float2 uvE = uvC + float2(inv.x, 0.0);

    float3 vC = float3(uvC - 0.5, 1.0) * linearize(p);
    float3 vN = float3(uvN - 0.5, 1.0) * linearize(p + int2(0, -1));
    float3 vE = float3(uvE - 0.5, 1.0) * linearize(p + int2(1,  0));

    // tan(fov/2)*2 because (pos - 0.5) spans 1.0, not 2.0.
    float m = (g_flags & 32u) ? tan(radians(g_fov) * 0.5) * 2.0 : g_normal_mul;
    vC.xy *= m; vN.xy *= m; vE.xy *= m;

    float3 n = normalize(cross(vC - vN, vC - vE));
    if (g_flags & 64u)  n.x = -n.x;
    if (g_flags & 128u) n.y = -n.y;
    if (g_flags & 256u) n.z = -n.z;
    DN[id.xy] = float4(n * 0.5 + 0.5, 1.0);
}

// Statistics of the RAW source, before a single one of the controls above touches it. This is the
// only instrument that can tell "no depth is reaching the addon" apart from "the settings are
// wrong": on screen both are a flat frame, and the whole trap this subsystem walked into was that
// a mis-set multiplier is indistinguishable from a dead pipe when all you have is the picture.
//
// Deliberately a FIXED 256x256 sample grid rather than the full frame. It bounds the atomic
// traffic (this runs behind a panel-open request, not on the capture path), and it keeps the
// fixed-point sum below 2^32 (65536 samples * 4096) so the mean needs no float atomic — cs_5_0
// has none. The readout says "sampled" for exactly this reason; don't quietly call it exact.
[numthreads(8, 8, 1)]
void probe_cs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 256u || id.y >= 256u) return;
    int2 p = int2((int)((id.x * g_w) / 256u), (int)((id.y * g_h) / 256u));
    p = clamp(p, int2(0, 0), int2(g_w - 1, g_h - 1));
    float d  = S.Load(int3(p, 0)).x;
    float ds = saturate(d);

    InterlockedAdd(PB[min((uint)(ds * 256.0), 255u)], 1u);   //   0..255  histogram over 0..1
    if (d < 0.001) InterlockedAdd(PB[256], 1u);              // 256  at/near the low extreme
    if (d > 0.999) InterlockedAdd(PB[257], 1u);              // 257  at/near the high extreme
    if (d > 1.0)   InterlockedAdd(PB[258], 1u);              // 258  outside 0..1 (not a hardware depth range)
    // The buffer is cleared to ZERO before every dispatch, so a plain InterlockedMin would stick at
    // 0 forever. Store the COMPLEMENT of the minimum and take a max instead; the CPU undoes it.
    // asuint() is monotonic for non-negative floats, which is what makes an integer atomic a valid
    // float min/max in the first place — hence the max(d, 0) guard.
    uint bits = asuint(max(d, 0.0));
    InterlockedMax(PB[259], ~bits);                          // 259  ~min
    InterlockedMax(PB[260], bits);                           // 260  max
    InterlockedAdd(PB[261], (uint)(ds * 4096.0));            // 261  sum, 12-bit fixed point
}
)";

template <typename T> static void ng_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void native_gpu_free() {
    ng_release(g_ng.cs_depth); ng_release(g_ng.cs_normals); ng_release(g_ng.cb);
    ng_release(g_ng.cs_probe); ng_release(g_ng.probe_uav); ng_release(g_ng.probe_buf);
    ng_release(g_ng.probe_rb[0]); ng_release(g_ng.probe_rb[1]);
    g_ng.rb_pending[0] = g_ng.rb_pending[1] = false; g_ng.rb_next = 0;
    g_ng.ok = false; g_ng.tried = false; // re-init cleanly on the next device
}

// Compile both entry points once per device lifetime, behind a tried/ok latch — the same contract
// pv_gpu_init and waveform_gpu_init use. A failure is reported once and the passes stay dark;
// there is no CPU fallback here (unlike the preview, which must always show something).
static ID3D11ComputeShader* native_compile(ID3D11Device* d11, const char* entry) {
    static HMODULE dcomp = LoadLibraryA("d3dcompiler_47.dll");
    typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    static PFN_Compile compile = dcomp ? (PFN_Compile)GetProcAddress(dcomp, "D3DCompile") : nullptr;
    if (!compile) return nullptr;

    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = compile(NATIVE_CS_HLSL, sizeof(NATIVE_CS_HLSL) - 1, "mpr_native_cs", nullptr, nullptr,
                         entry, "cs_5_0", (1u << 15) /*D3DCOMPILE_OPTIMIZATION_LEVEL3*/, 0, &blob, &err);
    if (FAILED(hr) || !blob) {
        native_report(reshade::log::level::error,
                      "MultipassRecorder: native '%s' shader failed to compile (0x%08lx)%s%.128s",
                      entry, (unsigned long)hr, err ? ": " : "",
                      err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release(); if (blob) blob->Release();
        return nullptr;
    }
    if (err) err->Release();
    ID3D11ComputeShader* cs = nullptr;
    hr = d11->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
    blob->Release();
    return SUCCEEDED(hr) ? cs : nullptr;
}

static bool native_gpu_init(ID3D11Device* d11) {
    if (g_ng.tried) return g_ng.ok;
    g_ng.tried = true;

    g_ng.cs_depth   = native_compile(d11, "depth_cs");
    g_ng.cs_normals = native_compile(d11, "normals_cs");
    if (!g_ng.cs_depth || !g_ng.cs_normals) { native_gpu_free(); g_ng.tried = true; return false; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(NativeParams); cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(d11->CreateBuffer(&cbd, nullptr, &g_ng.cb))) { native_gpu_free(); g_ng.tried = true; return false; }

    // The probe is built on the same latch but is deliberately NOT part of `ok`: it is an
    // instrument, and losing the instrument must not also lose the passes it was there to explain.
    // Every use site tests g_ng.cs_probe, and the readout says so rather than showing stale numbers.
    g_ng.cs_probe = native_compile(d11, "probe_cs");
    if (g_ng.cs_probe) {
        bool pok = true;
        D3D11_BUFFER_DESC pbd = {};
        pbd.ByteWidth = NPB_UINTS * 4; pbd.Usage = D3D11_USAGE_DEFAULT;
        pbd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        pbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; pbd.StructureByteStride = 4;
        pok = pok && SUCCEEDED(d11->CreateBuffer(&pbd, nullptr, &g_ng.probe_buf));
        if (pok) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = NPB_UINTS;
            pok = SUCCEEDED(d11->CreateUnorderedAccessView(g_ng.probe_buf, &ud, &g_ng.probe_uav));
        }
        D3D11_BUFFER_DESC rbd = {};
        rbd.ByteWidth = NPB_UINTS * 4; rbd.Usage = D3D11_USAGE_STAGING;
        rbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; pok && i < 2; ++i) pok = SUCCEEDED(d11->CreateBuffer(&rbd, nullptr, &g_ng.probe_rb[i]));
        if (!pok) { // partial probe is no probe — drop it whole, keep depth/normals
            ng_release(g_ng.cs_probe); ng_release(g_ng.probe_uav); ng_release(g_ng.probe_buf);
            ng_release(g_ng.probe_rb[0]); ng_release(g_ng.probe_rb[1]);
        }
    }

    g_ng.ok = true;
    return true;
}

static uint32_t native_flags(const RecordPass& p) {
    uint32_t f = 0;
    if (p.native_reversed)      f |= NF_REVERSED;
    if (p.native_upside_down)   f |= NF_UPSIDE_DOWN;
    if (p.native_log_curve)     f |= NF_LOG_CURVE;
    if (p.native_reverse_luma)  f |= NF_REVERSE_LUMA;
    if (p.native_linear_output) f |= NF_LINEAR_OUT;
    if (p.native_use_fov)       f |= NF_USE_FOV;
    if (p.native_flip_x)        f |= NF_FLIP_X;
    if (p.native_flip_y)        f |= NF_FLIP_Y;
    if (p.native_flip_z)        f |= NF_FLIP_Z;
    return f;
}

// The raw depth texture the shrunken MultipassRecorder.fx publishes. Resolved by NAME every frame
// (never cached): effect texture handles die on every effect reload, exactly like technique handles.
static resource_view native_depth_source(effect_runtime* rt) {
    effect_texture_variable var = rt->find_texture_variable(nullptr, "texDepthRaw");
    if (var == 0) return { 0 };
    resource_view srv = { 0 };
    rt->get_texture_binding(var, &srv, nullptr);
    return srv;
}

// Latch so a missing texDepthRaw is reported once per outage, not once per frame. Re-arms the
// moment the texture comes back, so the next genuine outage is reported again.
static bool g_native_src_missing = false;

// Called from on_reshade_finish_effects (mpr_shot.inc), AFTER the visible chain has run — which is
// what makes texDepthRaw this frame's depth rather than last frame's.
static void native_depth_normals_tick(effect_runtime* rt, device* dev)
{
    bool want = false;
    for (const auto& p : g_passes)
        if ((p.source == SRC_NATIVE_DEPTH || p.source == SRC_NATIVE_NORMALS) && native_should_render(p))
            { want = true; break; }
    if (!want) return;

    if (dev->get_api() != device_api::d3d11) return; // compute path is DX11-only, like the preview

    const resource_view src_srv = native_depth_source(rt);
    if (src_srv.handle == 0) {
        if (!g_native_src_missing) {
            g_native_src_missing = true;
            native_report(reshade::log::level::warning,
                          "MultipassRecorder: depth and normals have no source - MultipassRecorder.fx isn't loaded, "
                          "or its 'Multipass Recorder Setup' technique is switched off in the ReShade list.");
        }
        return;
    }
    g_native_src_missing = false;

    ID3D11Device* d11 = reinterpret_cast<ID3D11Device*>(dev->get_native());
    if (!native_gpu_init(d11)) return;
    ID3D11DeviceContext* ctx = reinterpret_cast<ID3D11DeviceContext*>(
        rt->get_command_queue()->get_immediate_command_list()->get_native());

    uint32_t w = 0, h = 0;
    rt->get_screenshot_width_and_height(&w, &h);
    if (w == 0 || h == 0) return;

    // Save the game's CS state once for the whole group, restore once. Same contract as
    // mpr_preview.inc: DX11 games legitimately rely on state persisting across Present.
    ID3D11ComputeShader* oldCS = nullptr; ID3D11ClassInstance* oldInst[16] = {}; UINT nInst = 16;
    ctx->CSGetShader(&oldCS, oldInst, &nInst);
    ID3D11UnorderedAccessView* oldUAV[2] = {};
    ctx->CSGetUnorderedAccessViews(0, 2, oldUAV);
    ID3D11ShaderResourceView* oldSRV = nullptr;
    ctx->CSGetShaderResources(0, 1, &oldSRV);
    ID3D11Buffer* oldCB = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCB);

    ID3D11ShaderResourceView* s = reinterpret_cast<ID3D11ShaderResourceView*>((uintptr_t)src_srv.handle);

    for (auto& p : g_passes) {
        const bool is_depth = (p.source == SRC_NATIVE_DEPTH);
        if ((!is_depth && p.source != SRC_NATIVE_NORMALS) || !native_should_render(p)) continue;

        // Depth keeps every bit the engine rendered (r32_float); normals are a direction vector,
        // for which 8 bits per axis is ample and keeps the pass on the plain 8-bit encode path.
        const format f = is_depth ? format::r32_float : format::r8g8b8a8_unorm;
        p.ffmpeg_fmt = "rgba"; // ignored for depth: the worker keys on cap_format == r32_float
        if (!native_ensure_rt(dev, p, w, h, f, /*need_uav=*/true)) continue;

        D3D11_MAPPED_SUBRESOURCE cm = {};
        if (FAILED(ctx->Map(g_ng.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) continue;
        {
            NativeParams np = {};
            np.far_plane  = p.native_far_plane;
            np.depth_mul  = p.native_depth_mul;
            np.fov        = p.native_fov;
            np.normal_mul = p.native_normal_mul;
            np.w = w; np.h = h;
            np.flags = native_flags(p);
            memcpy(cm.pData, &np, sizeof(np));
            ctx->Unmap(g_ng.cb, 0);
        }

        ID3D11UnorderedAccessView* u = reinterpret_cast<ID3D11UnorderedAccessView*>((uintptr_t)p.native_uav.handle);
        ID3D11UnorderedAccessView* uavs[2] = { is_depth ? u : nullptr, is_depth ? nullptr : u };
        ctx->CSSetShader(is_depth ? g_ng.cs_depth : g_ng.cs_normals, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &g_ng.cb);
        ctx->CSSetShaderResources(0, 1, &s);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

        p.native_last_render_frame = g_live_frame;
    }

    ctx->CSSetShader(oldCS, nInst ? oldInst : nullptr, nInst);
    ctx->CSSetUnorderedAccessViews(0, 2, oldUAV, nullptr);
    ctx->CSSetShaderResources(0, 1, &oldSRV);
    ctx->CSSetConstantBuffers(0, 1, &oldCB);
    ng_release(oldCS);
    for (UINT i = 0; i < nInst; ++i) ng_release(oldInst[i]);
    ng_release(oldUAV[0]); ng_release(oldUAV[1]);
    ng_release(oldSRV); ng_release(oldCB);
}

// ---- THE DEPTH PROBE: what is actually in texDepthRaw ---------------------------------------
// The instrument this subsystem shipped without, and the reason a working depth pipe was
// indistinguishable from a dead one. Everything above turns raw depth into a picture; a wrong
// multiplier and an empty source produce the SAME picture (a flat frame), so no amount of staring
// at the Viewfinder separates them. This reads the source before any of the tuning and states what
// is in it — which also gives Auto-calibrate something to calibrate FROM.
//
// It runs behind an explicit request (the depth controls being on screen), throttled, never on the
// capture path: a diagnostic that costs a recording frames is a diagnostic you turn off.

enum NativeProbeState {
    NPS_IDLE,       // nobody has asked yet this session
    NPS_NO_API,     // not D3D11 — the compute path doesn't exist here, same as depth/normals
    NPS_NO_SHADER,  // probe shader or its buffers unavailable (depth/normals may still be fine)
    NPS_NO_SOURCE,  // texDepthRaw didn't resolve: .fx not loaded, or its technique is switched off
    NPS_WAITING,    // asked, dispatched, first readback hasn't landed yet
    NPS_OK,         // g_np holds real numbers
};
static NativeProbeState g_np_state = NPS_IDLE;

struct NativeProbeStats {
    bool     have  = false;   // a readback has landed at least once this session
    uint64_t frame = 0;       // g_live_frame the landed stats were dispatched on
    uint32_t w = 0, h = 0;    // source dimensions at that dispatch
    float    min_v = 0.0f, max_v = 0.0f, mean = 0.0f;
    float    frac_lo = 0.0f, frac_hi = 0.0f, frac_oob = 0.0f;
    uint32_t hist[NPB_BINS] = {};
};
static NativeProbeStats g_np;

// Frame the UI last asked for a probe. Zero = never; the tick treats it as a sentinel rather than
// a frame number so the probe can't run for the first eight frames of every session unasked.
static uint64_t g_np_request_frame = 0;
static void native_probe_request() { g_np_request_frame = g_live_frame; }

static void native_probe_drain(ID3D11DeviceContext* ctx) {
    for (int i = 0; i < 2; ++i) {
        if (!g_ng.rb_pending[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m = {};
        HRESULT hr = ctx->Map(g_ng.probe_rb[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr)) {
            const uint32_t* b = static_cast<const uint32_t*>(m.pData);
            for (uint32_t k = 0; k < NPB_BINS; ++k) g_np.hist[k] = b[k];
            const float n = (float)NP_SAMPLES;
            g_np.frac_lo  = b[NPB_LO]  / n;
            g_np.frac_hi  = b[NPB_HI]  / n;
            g_np.frac_oob = b[NPB_OOB] / n;
            // Undo the complement trick the shader used to get a minimum out of a zero-cleared
            // buffer. A dispatch that touched nothing leaves ~min == 0 -> min == that bit pattern
            // read back as a huge float, so guard on the max having moved at all.
            const uint32_t maxb = b[NPB_MAX], minb = ~b[NPB_MINC];
            float mn = 0.0f, mx = 0.0f;
            memcpy(&mx, &maxb, sizeof(mx));
            memcpy(&mn, &minb, sizeof(mn));
            g_np.max_v = mx;
            g_np.min_v = (mn <= mx) ? mn : mx;
            g_np.mean  = (float)b[NPB_SUM] / (n * 4096.0f);
            g_np.have  = true;
            g_np.frame = g_live_frame;
            g_np_state = NPS_OK;
            ctx->Unmap(g_ng.probe_rb[i], 0);
            g_ng.rb_pending[i] = false;
        } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            g_ng.rb_pending[i] = false;
        }
    }
}

// Called from on_reshade_finish_effects (mpr_shot.inc), beside native_depth_normals_tick and for
// the same reason: this is the only point in the frame where texDepthRaw holds THIS frame's depth.
static void native_probe_tick(effect_runtime* rt, device* dev)
{
    if (dev->get_api() != device_api::d3d11) {
        if (g_np_request_frame != 0) g_np_state = NPS_NO_API;
        return;
    }
    ID3D11Device* d11 = reinterpret_cast<ID3D11Device*>(dev->get_native());
    ID3D11DeviceContext* ctx = reinterpret_cast<ID3D11DeviceContext*>(
        rt->get_command_queue()->get_immediate_command_list()->get_native());

    // Drain unconditionally, ahead of the request test: a readback that was still in flight when
    // the panel closed has to be collected, or its slot stays pending for the rest of the session
    // and the ping-pong degrades to a single buffer (then to none).
    native_probe_drain(ctx);

    if (g_np_request_frame == 0 || (g_live_frame - g_np_request_frame) > 8) return;
    if (!native_gpu_init(d11) || !g_ng.cs_probe) { g_np_state = NPS_NO_SHADER; return; }

    const resource_view src_srv = native_depth_source(rt);
    if (src_srv.handle == 0) { g_np_state = NPS_NO_SOURCE; return; }
    if (!g_np.have) g_np_state = NPS_WAITING;

    if ((g_live_frame % 4) != 0) return;                       // throttle: this is a readout, not a feed
    if (g_ng.rb_pending[g_ng.rb_next]) return;                 // both slots busy — skip this beat

    uint32_t w = 0, h = 0;
    rt->get_screenshot_width_and_height(&w, &h);
    if (w == 0 || h == 0) return;

    D3D11_MAPPED_SUBRESOURCE cm = {};
    if (FAILED(ctx->Map(g_ng.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) return;
    {
        NativeParams np = {};
        np.w = w; np.h = h;   // the probe reads nothing else out of the cbuffer
        memcpy(cm.pData, &np, sizeof(np));
        ctx->Unmap(g_ng.cb, 0);
    }

    // Same save/restore contract as the dispatch above and as mpr_preview.inc: DX11 games
    // legitimately rely on compute state persisting across Present, so leave it exactly as found.
    ID3D11ComputeShader* oldCS = nullptr; ID3D11ClassInstance* oldInst[16] = {}; UINT nInst = 16;
    ctx->CSGetShader(&oldCS, oldInst, &nInst);
    ID3D11UnorderedAccessView* oldUAV = nullptr;
    ctx->CSGetUnorderedAccessViews(2, 1, &oldUAV);
    ID3D11ShaderResourceView* oldSRV = nullptr;
    ctx->CSGetShaderResources(0, 1, &oldSRV);
    ID3D11Buffer* oldCB = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCB);

    ID3D11ShaderResourceView* s = reinterpret_cast<ID3D11ShaderResourceView*>((uintptr_t)src_srv.handle);
    const UINT zero4[4] = { 0, 0, 0, 0 };
    ctx->ClearUnorderedAccessViewUint(g_ng.probe_uav, zero4);
    ctx->CSSetShader(g_ng.cs_probe, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_ng.cb);
    ctx->CSSetShaderResources(0, 1, &s);
    ctx->CSSetUnorderedAccessViews(2, 1, &g_ng.probe_uav, nullptr);
    ctx->Dispatch(256 / 8, 256 / 8, 1);

    ctx->CopyResource(g_ng.probe_rb[g_ng.rb_next], g_ng.probe_buf);
    g_ng.rb_pending[g_ng.rb_next] = true;
    g_ng.rb_next ^= 1;
    g_np.w = w; g_np.h = h;

    ctx->CSSetShader(oldCS, nInst ? oldInst : nullptr, nInst);
    ctx->CSSetUnorderedAccessViews(2, 1, &oldUAV, nullptr);
    ctx->CSSetShaderResources(0, 1, &oldSRV);
    ctx->CSSetConstantBuffers(0, 1, &oldCB);
    ng_release(oldCS);
    for (UINT i = 0; i < nInst; ++i) ng_release(oldInst[i]);
    ng_release(oldUAV); ng_release(oldSRV); ng_release(oldCB);
}

// Value below which fraction `q` of the sampled pixels fall, read off the 0..1 histogram. Used
// instead of the raw maximum when calibrating: a handful of stray pixels (a skybox seam, a decal,
// a cleared tile) at the extreme would otherwise set the multiplier for the entire pass.
static float native_probe_percentile(float q) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < NPB_BINS; ++i) total += g_np.hist[i];
    if (total == 0) return 0.0f;
    const uint32_t target = (uint32_t)(q * (float)total);
    uint32_t run = 0;
    for (uint32_t i = 0; i < NPB_BINS; ++i) {
        run += g_np.hist[i];
        if (run >= target) return ((float)i + 0.5f) / (float)NPB_BINS;
    }
    return 1.0f;
}

// Which end of the range the FAR plane appears to sit at. ONE implementation, shared by
// Auto-calibrate and the readout that reports it — same rule as hist_population being shared by the
// glance strip and the scope: two instruments that disagree about the same measurement are worse
// than one instrument, because the disagreement reads as a bug in whichever you trust less.
//
// An INFERENCE, and every caller labels it as one. Most of a scene sits near the far plane (the
// reciprocal distribution) and sky is exactly the far value, so a conventional buffer crowds 1.0
// and a reverse-Z one crowds 0.0. The mean is the tiebreak for a frame with no distance in it.
static bool native_probe_looks_reversed() {
    if (g_np.frac_hi > g_np.frac_lo * 1.5f) return false;
    if (g_np.frac_lo > g_np.frac_hi * 1.5f) return true;
    return g_np.mean < 0.5f;
}

enum NativeAutoResult { NA_OK, NA_NO_DATA, NA_FLAT };

// Derive `reversed` and the multiplier from what the probe actually measured. This is a STARTING
// POINT, not a truth: it describes the frame that was on screen when you pressed it, so it wants a
// representative view (some near geometry, some distance). It deliberately does NOT touch the far
// plane — that is a creative choice about which distance maps to white, not a property of the
// buffer, and overwriting it would undo framing work every time you re-ran this.
static NativeAutoResult native_auto_calibrate(RecordPass& p) {
    if (!g_np.have) return NA_NO_DATA;
    if (g_np.max_v - g_np.min_v < 1e-6f) return NA_FLAT;

    p.native_reversed = native_probe_looks_reversed();

    // Put the NEAR end of the buffer's real range at the near end of the 0..1 remap. That end is
    // the maximum under BOTH conventions — reversed reads d = 1 - raw*m, plain reads d = raw*m, and
    // either way m * (top of range) == 1 is the condition — so one formula covers both.
    const float top = (g_np.frac_oob > 0.001f) ? g_np.max_v : native_probe_percentile(0.995f);
    if (top > 1e-6f) {
        float m = 1.0f / top;
        if (m < NATIVE_DMUL_MIN) m = NATIVE_DMUL_MIN;
        if (m > NATIVE_DMUL_MAX) m = NATIVE_DMUL_MAX;
        p.native_depth_mul = m;
    }
    p.native_log_curve = false; // a log source is rare and the probe can't tell it apart; don't guess
    return NA_OK;
}
