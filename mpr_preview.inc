// === MultipassRecorder — mpr_preview.inc ===
// The preview/viewfinder subsystem — everything that turns a raw engine buffer into what the
// Viewfinder window, the Debug View, the OSD exposure meter and auto-exposure consume:
//   * a WYSIWYG display bitmap (exposure gain + Reinhard + gamma for HDR sources, passthrough
//     for LDR ones, with optional clip/crush false-color and channel isolation), and
//   * a 128-bin native-stops luminance histogram (feeds the exposure scope, Fit, AE, meter).
//
// Two interchangeable backends fill those outputs:
//   GPU (DX11 only, the tested API): a compute shader dispatched on the immediate context —
//     full-resolution display texture, histogram via groupshared atomics, and only a 528-byte
//     staging readback ever touches the CPU. The shader samples the source as float4, so it is
//     format-agnostic (r11g11b10 / rgba16f / rgba32f / r9e5 / any unorm). Compiled at runtime
//     with d3dcompiler_47 (present on every Win10+); if that fails we fall back to...
//   CPU (all other APIs / compile failure): throttled staging copy + CPU unpack loop, capped at
//     a 1024-wide bitmap. Understands the four HDR float formats; LDR sources keep the plain
//     raw-SRV preview.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.

// Defined with the scope UI (mpr_ui.inc); needed here for clip-tint thresholds and AE.
static void curve_window_stops(int hdr, float* floor_st, float* white_st, float* ceil_st);

// The RGB waveform/parade scope (mpr_waveform.inc, included right after this fragment) piggybacks
// on this file's GPU preview dispatch — it reuses the private source copy this file already makes.
// Forward-declared so preview_gpu_tick can invoke it without depending on include order.
static void waveform_capture(ID3D11Device* d11, ID3D11DeviceContext* ctx,
                             ID3D11ShaderResourceView* src_srv,
                             uint32_t sw, uint32_t sh, float gain, bool hdr_mode, bool mono);

// ---- Format classification -----------------------------------------------------------

// Float-RGB HDR formats: these get the exposure preview + stops histogram + scope.
static bool preview_fmt_hdr(format f) {
    return f == format::r11g11b10_float || f == format::r16g16b16a16_float
        || f == format::r32g32b32a32_float || f == format::r9g9b9e5;
}
// Formats the CPU fallback knows how to unpack (HDR only; LDR falls back to the raw SRV).
static bool preview_fmt_cpu(format f) { return preview_fmt_hdr(f); }
// SINGLE-CHANNEL formats. A Load() of an R-only texture returns float4(v, 0, 0, 1), so showing it
// as RGB paints it pure RED and calls a mid-grey value "clipped" — which is exactly what the
// native depth pass (r32_float) looked like, and what every r8/r16 engine mask buffer looked like
// before it. These are luminance data, so the shader replicates R across RGB (flag bit2).
static bool preview_fmt_mono(format f) { return format_role(f) == RTR_MASK; }
// Formats the GPU path can Load() as float4 — every color-like role. Depth/stencil and
// exotic formats keep the raw-SRV fallback.
static bool preview_fmt_gpu(format f) {
    switch (format_role(f)) {
        case RTR_HDR: case RTR_HIPREC: case RTR_SDR10: case RTR_SDR8:
        case RTR_DATA: case RTR_MASK: return true;
        default: return false;
    }
}

// ---- CPU unpack helpers ---------------------------------------------------------------

// IEEE half -> float. Denormal halves flush to 0: they top out at ~6e-5 linear (~ -14 st),
// far below any capture floor, and this is display/statistics-only.
static inline float pv_half_to_float(uint16_t h) {
    const uint32_t s = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
    uint32_t u;
    if (e == 0)       u = s;                                  // +-0 / denormal -> +-0
    else if (e == 31) u = s | 0x7F800000u | (m << 13);        // inf/nan
    else              u = s | ((e + 112u) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4); return f;
}

// r9g9b9e5 shared-exponent -> linear RGB.
static inline void pv_unpack_rgb9e5(uint32_t u, float* r, float* g, float* b) {
    const float sc = exp2f((float)((int)((u >> 27) & 0x1F) - 24));
    *r = (float)( u        & 0x1FF) * sc;
    *g = (float)((u >> 9)  & 0x1FF) * sc;
    *b = (float)((u >> 18) & 0x1FF) * sc;
}

// ---- Shared histogram finalize --------------------------------------------------------
// Both backends land their 128 native-stop bins + under/over/total counters in g_pv, then call
// this to derive the data-extent percentiles and publish the frame.
static void pv_finalize_hist() {
    double inrange = 0;
    for (int i = 0; i < PreviewCap::HIST_BINS; ++i) inrange += g_pv.hist[i];
    const float span = PreviewCap::HIST_HI - PreviewCap::HIST_LO;
    auto pctile = [&](double frac) -> float {
        double t = inrange * frac, run = 0;
        for (int i = 0; i < PreviewCap::HIST_BINS; ++i) {
            run += g_pv.hist[i];
            if (run >= t) return PreviewCap::HIST_LO + span * (i + 0.5f) / PreviewCap::HIST_BINS;
        }
        return PreviewCap::HIST_HI;
    };
    g_pv.span_lo       = inrange > 0 ? pctile(0.005) : 0;
    g_pv.median_native = inrange > 0 ? pctile(0.5)   : 0;
    g_pv.span_hi       = inrange > 0 ? pctile(0.995) : 0;
    g_pv.have_frame = true;
}

// ---- GPU backend (native D3D11) -------------------------------------------------------
// Uses the native device/context directly (only DX11 is a tested target anyway): reshade::api
// has no pipeline-state save/restore, while native CSGet*/CSSet* lets us leave the game's
// compute state exactly as we found it. All reshade handles are the native COM pointers on
// the D3D11 backend, so interop is a cast.

static constexpr uint32_t PV_HIST_UINTS = 132; // 128 bins + [128] under + [129] over + [130] total + spare

struct PvGpu {
    bool tried = false, ok = false;            // shader compile attempted / whole path usable
    ID3D11ComputeShader*       cs        = nullptr;
    ID3D11Buffer*              cb        = nullptr;
    ID3D11Buffer*              hist      = nullptr;
    ID3D11UnorderedAccessView* hist_uav  = nullptr;
    ID3D11Buffer*              hist_rb[2] = {};   // staging ping-pong for the 528-byte readback
    bool                       rb_pending[2] = {};
    int                        rb_next   = 0;
    ID3D11Texture2D*           priv      = nullptr; // private GPU copy of the source: always legal to
    ID3D11ShaderResourceView*  priv_srv  = nullptr; // SRV-sample (source may be RTV-bound right now)
    ID3D11Texture2D*           disp      = nullptr; // processed RGBA8 the UI samples (full source res)
    ID3D11UnorderedAccessView* disp_uav  = nullptr;
    ID3D11ShaderResourceView*  disp_srv  = nullptr;
    uint32_t    src_w = 0, src_h = 0;
    DXGI_FORMAT src_fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t    disp_w = 0, disp_h = 0;
    bool        disp_valid = false;            // disp holds a processed frame of the CURRENT source
    bool        last_hdr   = false;            // mode of the last dispatch (hist only valid in HDR mode)
};
static PvGpu g_pvg;

// Mirror of the HLSL cbuffer below (three 16-byte rows).
struct PvGpuParams {
    float    gain, lo_lin, hi_lin; uint32_t flags; // flags: bit0 clip tint, bit1 HDR mode, bit2 mono source
    uint32_t sw, sh, dw, dh;
    uint32_t channel, pad0, pad1, pad2;            // 0 rgb, 1 r, 2 g, 3 b, 4 a, 5 luma
};

// One dispatch does both jobs: nearest-sample the source, write the processed display pixel,
// and accumulate the native-stops histogram through groupshared bins (one global atomic per
// bin per group instead of per pixel).
static const char PV_CS_HLSL[] = R"(
cbuffer PvParams : register(b0) {
    float g_gain, g_lo, g_hi; uint g_flags;
    uint g_sw, g_sh, g_dw, g_dh;
    uint g_channel, g_pad0, g_pad1, g_pad2;
};
Texture2D<float4>         S : register(t0);
RWTexture2D<unorm float4> D : register(u0);
RWStructuredBuffer<uint>  H : register(u1);
groupshared uint LB[132];

[numthreads(8, 8, 1)]
void csmain(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    uint i;
    for (i = gi; i < 132; i += 64) LB[i] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < g_dw && id.y < g_dh) {
        uint sx = id.x * g_sw / g_dw, sy = id.y * g_sh / g_dh;
        float4 c = S.Load(int3(sx, sy, 0));
        // Single-channel source (depth, masks): a Load gives float4(v,0,0,1), so without this the
        // image is pure red and the luma below reads 0.2126*v instead of v.
        if (g_flags & 4u) c.rgb = c.rrr;
        float3 outc;
        if (g_flags & 2u) { // HDR: histogram in NATIVE (pre-gain) stops; display is exposed WYSIWYG
            float Y = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
            InterlockedAdd(LB[130], 1u);
            if (Y <= 1e-6) InterlockedAdd(LB[128], 1u);
            else {
                float st = log2(Y);
                if (st < -20.0)     InterlockedAdd(LB[128], 1u);
                else if (st >= 8.0) InterlockedAdd(LB[129], 1u);
                else { uint b = (uint)((st + 20.0) * (128.0 / 28.0)); InterlockedAdd(LB[min(b, 127u)], 1u); }
            }
            if ((g_flags & 1u) && Y > g_hi)      outc = float3(0.922, 0.176, 0.176); // clipped -> red
            else if ((g_flags & 1u) && Y < g_lo) outc = float3(0.196, 0.333, 0.922); // crushed -> blue
            else {
                float3 v = max(c.rgb * g_gain, 0.0);
                v = v / (1.0 + v);                     // Reinhard
                outc = pow(v, 1.0 / 2.2);
            }
        } else {
            outc = saturate(c.rgb);                    // LDR passthrough
        }
        if      (g_channel == 1) outc = outc.rrr;
        else if (g_channel == 2) outc = outc.ggg;
        else if (g_channel == 3) outc = outc.bbb;
        else if (g_channel == 4) outc = saturate(c.aaa);
        else if (g_channel == 5) outc = dot(outc, float3(0.2126, 0.7152, 0.0722)).xxx;
        D[id.xy] = float4(outc, 1.0);
    }
    GroupMemoryBarrierWithGroupSync();
    for (i = gi; i < 132; i += 64) { uint v = LB[i]; if (v) InterlockedAdd(H[i], v); }
}
)";

template <typename T> static void pv_com_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Release the per-source-size objects (private copy + display set). Shader/CB/hist survive.
static void pv_gpu_release_sized() {
    pv_com_release(g_pvg.priv_srv); pv_com_release(g_pvg.priv);
    pv_com_release(g_pvg.disp_uav); pv_com_release(g_pvg.disp_srv); pv_com_release(g_pvg.disp);
    g_pvg.src_w = g_pvg.src_h = g_pvg.disp_w = g_pvg.disp_h = 0;
    g_pvg.src_fmt = DXGI_FORMAT_UNKNOWN;
    g_pvg.disp_valid = false;
    g_pvg.rb_pending[0] = g_pvg.rb_pending[1] = false; // stale histograms die with the source
}

static void pv_gpu_release_all() {
    pv_gpu_release_sized();
    pv_com_release(g_pvg.hist_uav); pv_com_release(g_pvg.hist);
    pv_com_release(g_pvg.hist_rb[0]); pv_com_release(g_pvg.hist_rb[1]);
    pv_com_release(g_pvg.cb); pv_com_release(g_pvg.cs);
    g_pvg.ok = false; g_pvg.tried = false; // re-init cleanly on the next device
}

// Compile the shader + create the size-independent objects, once per device lifetime.
// Any failure marks the path dead and the CPU fallback takes over silently.
static bool pv_gpu_init(ID3D11Device* d11) {
    if (g_pvg.tried) return g_pvg.ok;
    g_pvg.tried = true;

    static HMODULE dcomp = LoadLibraryA("d3dcompiler_47.dll");
    typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    PFN_Compile compile = dcomp ? (PFN_Compile)GetProcAddress(dcomp, "D3DCompile") : nullptr;
    if (!compile) {
        reshade::log::message(reshade::log::level::warning, "MultipassRecorder: d3dcompiler_47 unavailable - preview uses the CPU path");
        return false;
    }
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = compile(PV_CS_HLSL, sizeof(PV_CS_HLSL) - 1, "mpr_preview_cs", nullptr, nullptr,
                         "csmain", "cs_5_0", (1u << 15) /*D3DCOMPILE_OPTIMIZATION_LEVEL3*/, 0, &blob, &err);
    if (FAILED(hr) || !blob) {
        char msg[256];
        sprintf_s(msg, "MultipassRecorder: preview CS compile failed (0x%08lx)%s%.128s", (unsigned long)hr,
                  err ? ": " : "", err ? (const char*)err->GetBufferPointer() : "");
        reshade::log::message(reshade::log::level::warning, msg);
        if (err) err->Release(); if (blob) blob->Release();
        return false;
    }
    if (err) err->Release();
    hr = d11->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_pvg.cs);
    blob->Release();
    if (FAILED(hr)) return false;

    bool ok = true;
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(PvGpuParams); cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(d11->CreateBuffer(&cbd, nullptr, &g_pvg.cb));

    D3D11_BUFFER_DESC hbd = {};
    hbd.ByteWidth = PV_HIST_UINTS * 4; hbd.Usage = D3D11_USAGE_DEFAULT;
    hbd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; hbd.StructureByteStride = 4;
    ok = ok && SUCCEEDED(d11->CreateBuffer(&hbd, nullptr, &g_pvg.hist));
    if (ok) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = PV_HIST_UINTS;
        ok = SUCCEEDED(d11->CreateUnorderedAccessView(g_pvg.hist, &ud, &g_pvg.hist_uav));
    }
    D3D11_BUFFER_DESC rbd = {};
    rbd.ByteWidth = PV_HIST_UINTS * 4; rbd.Usage = D3D11_USAGE_STAGING;
    rbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; ok && i < 2; ++i) ok = SUCCEEDED(d11->CreateBuffer(&rbd, nullptr, &g_pvg.hist_rb[i]));

    if (!ok) { pv_gpu_release_all(); g_pvg.tried = true; return false; }
    g_pvg.ok = true;
    return true;
}

// Recreate the source-sized objects when the previewed buffer changes size/format.
static bool pv_gpu_ensure_sized(ID3D11Device* d11, uint32_t sw, uint32_t sh, DXGI_FORMAT tfmt) {
    if (g_pvg.priv && g_pvg.src_w == sw && g_pvg.src_h == sh && g_pvg.src_fmt == tfmt) return true;
    pv_gpu_release_sized();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = sw; td.Height = sh; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = tfmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d11->CreateTexture2D(&td, nullptr, &g_pvg.priv)) ||
        FAILED(d11->CreateShaderResourceView(g_pvg.priv, nullptr, &g_pvg.priv_srv))) {
        pv_gpu_release_sized(); return false;
    }

    // Display at full source resolution (this is the whole point of the GPU path); a 4096 cap
    // bounds pathological sources, the shader's integer scale handles any dw/dh <= sw/sh.
    uint32_t dw = sw, dh = sh;
    const uint32_t CAP = 4096;
    if (dw >= dh) { if (dw > CAP) { dh = dh * CAP / dw; dw = CAP; } }
    else          { if (dh > CAP) { dw = dw * CAP / dh; dh = CAP; } }
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = dw; dd.Height = dh; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(d11->CreateTexture2D(&dd, nullptr, &g_pvg.disp)) ||
        FAILED(d11->CreateUnorderedAccessView(g_pvg.disp, nullptr, &g_pvg.disp_uav)) ||
        FAILED(d11->CreateShaderResourceView(g_pvg.disp, nullptr, &g_pvg.disp_srv))) {
        pv_gpu_release_sized(); return false;
    }
    g_pvg.src_w = sw; g_pvg.src_h = sh; g_pvg.src_fmt = tfmt;
    g_pvg.disp_w = dw; g_pvg.disp_h = dh;
    return true;
}

// One GPU preview tick. Returns false if this device/format can't take the GPU path (caller
// falls back to CPU). Polls histogram readbacks every call; copies+dispatches on throttle ticks.
static bool preview_gpu_tick(effect_runtime* rt, device* dev, resource src, format fmt,
                             uint32_t sw, uint32_t sh, float gain, float lo_lin, float hi_lin,
                             bool hdr_mode, bool want_wf) {
    if (dev->get_api() != device_api::d3d11 || !preview_fmt_gpu(fmt)) return false;
    ID3D11Device* d11 = reinterpret_cast<ID3D11Device*>(dev->get_native());
    if (!pv_gpu_init(d11)) return false;
    ID3D11DeviceContext* ctx = reinterpret_cast<ID3D11DeviceContext*>(
        rt->get_command_queue()->get_immediate_command_list()->get_native());

    if (!pv_gpu_ensure_sized(d11, sw, sh, (DXGI_FORMAT)format_to_default_typed(fmt))) return false;

    // Drain finished histogram readbacks (non-blocking; ping-pong so an in-flight copy is
    // never remapped or overwritten).
    for (int i = 0; i < 2; ++i) {
        if (!g_pvg.rb_pending[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m = {};
        HRESULT hr = ctx->Map(g_pvg.hist_rb[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr)) {
            const uint32_t* hb = static_cast<const uint32_t*>(m.pData);
            if (g_pvg.last_hdr) {
                for (int b = 0; b < PreviewCap::HIST_BINS; ++b) g_pv.hist[b] = (float)hb[b];
                g_pv.n_under = (float)hb[128]; g_pv.n_over = (float)hb[129]; g_pv.n_total = (float)hb[130];
                pv_finalize_hist();
            }
            ctx->Unmap(g_pvg.hist_rb[i], 0);
            g_pvg.rb_pending[i] = false;
        } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            g_pvg.rb_pending[i] = false;
        }
    }

    if (--g_pv.throttle > 0) return true;
    g_pv.throttle = 4; // ~ every 4th present, same cadence as the CPU path

    // Private copy first: the live source may be bound as an RTV right now, which would make a
    // direct SRV bind silently read nothing. A GPU->GPU copy is legal regardless of binding.
    ctx->CopySubresourceRegion(g_pvg.priv, 0, 0, 0, 0,
                               reinterpret_cast<ID3D11Resource*>((uintptr_t)src.handle), 0, nullptr);

    // Skip the whole tick if the parameters didn't make it across: dispatching anyway would run
    // the shader against the PREVIOUS tick's gain/dimensions/flags — one visibly wrong frame, and
    // silently wrong stats to go with it. Returning true (not false) keeps the GPU path owning
    // this source: a transient Map failure is no reason to tear over to the CPU backend.
    D3D11_MAPPED_SUBRESOURCE cm = {};
    if (FAILED(ctx->Map(g_pvg.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) return true;
    {
        PvGpuParams p = {};
        p.gain = gain; p.lo_lin = lo_lin; p.hi_lin = hi_lin;
        p.flags = (g_pv_show_clip ? 1u : 0u) | (hdr_mode ? 2u : 0u) | (preview_fmt_mono(fmt) ? 4u : 0u);
        p.sw = sw; p.sh = sh; p.dw = g_pvg.disp_w; p.dh = g_pvg.disp_h;
        p.channel = (uint32_t)g_pv_channel;
        memcpy(cm.pData, &p, sizeof(p));
        ctx->Unmap(g_pvg.cb, 0);
    }

    const UINT zero4[4] = {};
    ctx->ClearUnorderedAccessViewUint(g_pvg.hist_uav, zero4);

    // Save the game's CS state, run ours, restore exactly. Present never overlaps a draw, but
    // DX11 games legitimately rely on state persisting across Present, so we must not clobber.
    ID3D11ComputeShader* oldCS = nullptr; ID3D11ClassInstance* oldInst[16] = {}; UINT nInst = 16;
    ctx->CSGetShader(&oldCS, oldInst, &nInst);
    ID3D11UnorderedAccessView* oldUAV[2] = {};
    ctx->CSGetUnorderedAccessViews(0, 2, oldUAV);
    ID3D11ShaderResourceView* oldSRV = nullptr;
    ctx->CSGetShaderResources(0, 1, &oldSRV);
    ID3D11Buffer* oldCB = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCB);

    ID3D11UnorderedAccessView* uavs[2] = { g_pvg.disp_uav, g_pvg.hist_uav };
    ctx->CSSetShader(g_pvg.cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_pvg.cb);
    ctx->CSSetShaderResources(0, 1, &g_pvg.priv_srv);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ctx->Dispatch((g_pvg.disp_w + 7) / 8, (g_pvg.disp_h + 7) / 8, 1);

    ctx->CSSetShader(oldCS, nInst ? oldInst : nullptr, nInst);
    ctx->CSSetUnorderedAccessViews(0, 2, oldUAV, nullptr);
    ctx->CSSetShaderResources(0, 1, &oldSRV);
    ctx->CSSetConstantBuffers(0, 1, &oldCB);
    pv_com_release(oldCS);
    for (UINT i = 0; i < nInst; ++i) pv_com_release(oldInst[i]);
    pv_com_release(oldUAV[0]); pv_com_release(oldUAV[1]);
    pv_com_release(oldSRV); pv_com_release(oldCB);

    // Waveform / parade scope shares this dispatch's fresh private source copy (g_pvg.priv) and
    // throttle — it does its own CS state save/restore. Only when the panel actually wants it.
    if (want_wf) waveform_capture(d11, ctx, g_pvg.priv_srv, sw, sh, gain, hdr_mode, preview_fmt_mono(fmt));

    g_pvg.disp_valid = true;
    g_pvg.last_hdr = hdr_mode;
    if (!hdr_mode) g_pv.have_frame = false; // LDR dispatch produces no histogram; kill stale HDR stats

    if (hdr_mode && !g_pvg.rb_pending[g_pvg.rb_next]) {
        ctx->CopyResource(g_pvg.hist_rb[g_pvg.rb_next], g_pvg.hist);
        g_pvg.rb_pending[g_pvg.rb_next] = true;
        g_pvg.rb_next ^= 1;
    }
    return true;
}

// ---- Shared teardown / accessors ------------------------------------------------------

// Just the CPU backend's objects. Separate from pv_free because a CPU-side failure must NOT drag
// the GPU backend down with it: pv_gpu_release_all() destroys the compiled compute shader and
// resets `tried`, so the next tick that could have used the GPU path pays a full runtime
// recompile — for a staging allocation that has nothing to do with it.
static void pv_free_cpu(device* dev) {
    for (auto& s : g_pv.staging) if (s.handle) { dev->destroy_resource(s); s = { 0 }; }
    if (g_pv.done.handle)     { dev->destroy_fence(g_pv.done); g_pv.done = { 0 }; }
    if (g_pv.disp_srv.handle) { dev->destroy_resource_view(g_pv.disp_srv); g_pv.disp_srv = { 0 }; }
    if (g_pv.disp.handle)     { dev->destroy_resource(g_pv.disp); g_pv.disp = { 0 }; }
    g_pv.copy_index = g_pv.read_index = 0;
    g_pv.w = g_pv.h = g_pv.disp_w = g_pv.disp_h = 0;
    g_pv.src_fmt = format::unknown;
    g_pv.have_frame = false;
}

static void pv_free(device* dev) {
    pv_free_cpu(dev);
    pv_gpu_release_all();
}

// Nothing previewable this frame: make sure no consumer shows a stale processed frame.
static void pv_invalidate() {
    g_pv.have_frame = false;
    g_pvg.disp_valid = false;
}

// The processed display texture, whichever backend produced it ({0} = show the raw SRV).
static resource_view pv_display_srv(uint32_t* w = nullptr, uint32_t* h = nullptr) {
    if (g_pvg.disp_valid && g_pvg.disp_srv) {
        if (w) *w = g_pvg.disp_w; if (h) *h = g_pvg.disp_h;
        return { (uint64_t)(uintptr_t)g_pvg.disp_srv };
    }
    if (g_pv.have_frame && g_pv.disp_srv.handle) {
        if (w) *w = g_pv.disp_w; if (h) *h = g_pv.disp_h;
        return g_pv.disp_srv;
    }
    return { 0 };
}
// True while the GPU backend is producing the display (gates GPU-only UI like channel isolate).
static bool pv_gpu_active() { return g_pvg.disp_valid && g_pvg.disp_srv != nullptr; }

// ---- CPU backend ------------------------------------------------------------------------

// CPU-process one mapped HDR staging frame into the display bitmap + stops histogram.
// gain = the previewed pass's exposure (identity for raw buffers / non-encode-HDR passes).
// lo_lin/hi_lin = the capture window's floor/ceiling in NATIVE linear; pixels outside get a
// live false-color tint (blue = crushed black, red = clipped white) when g_pv_show_clip is on.
static void preview_process(device* dev, const uint8_t* src, uint32_t pitch,
                            uint32_t sw, uint32_t sh, format fmt,
                            float gain, float lo_lin, float hi_lin, uint32_t CAP) {
    // Downsample target: cap the long side. The caller sizes this for whoever's actually going
    // to display the bitmap -- a modest fixed cap for the small in-panel preview image, or
    // (Debug View / "Full screen") the real swapchain resolution, since either one stretches
    // this bitmap across the whole screen and a small cap there looks soft/blocky. The
    // histogram is derived from the same scan; every-4th-present throttling keeps it affordable.
    // (The GPU backend ignores the cap entirely -- it always renders at full source res.)
    uint32_t tw = sw, th = sh;
    if (tw >= th) { if (tw > CAP) { th = th * CAP / tw; tw = CAP; } }
    else          { if (th > CAP) { tw = tw * CAP / th; th = CAP; } }
    if (tw < 1) tw = 1; if (th < 1) th = 1;

    if (g_pv.disp_w != tw || g_pv.disp_h != th || !g_pv.disp.handle) {
        if (g_pv.disp_srv.handle) { dev->destroy_resource_view(g_pv.disp_srv); g_pv.disp_srv = { 0 }; }
        if (g_pv.disp.handle)     { dev->destroy_resource(g_pv.disp); g_pv.disp = { 0 }; }
        resource_desc dd(tw, th, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::cpu_to_gpu, resource_usage::shader_resource);
        if (!dev->create_resource(dd, nullptr, resource_usage::shader_resource, &g_pv.disp)) return;
        if (!dev->create_resource_view(g_pv.disp, resource_usage::shader_resource,
                                       resource_view_desc(format::r8g8b8a8_unorm), &g_pv.disp_srv)) return;
        g_pv.disp_w = tw; g_pv.disp_h = th;
    }

    subresource_data dst = {};
    if (!dev->map_texture_region(g_pv.disp, 0, nullptr, map_access::write_discard, &dst)) return;
    uint8_t* dp = static_cast<uint8_t*>(dst.data);

    // Shared tonemap (Reinhard + gamma 2.2) — the same reference view the GPU shader renders.
    auto tone  = [](float v) { if (v < 0) v = 0; v = v / (1 + v); v = powf(v, 1.0f / 2.2f); return v > 1 ? 1.0f : v; };
    auto tone8 = [&](float v) { return (uint8_t)(tone(v * gain) * 255.0f + 0.5f); };

    // Per-pixel tail shared by every format loop: clip/crush tint or the toned color, plus the
    // NATIVE-stops histogram (pre-gain, so the bars represent what's actually in the buffer and
    // DON'T move when exposure changes; the display bitmap still uses gain = WYSIWYG).
    int    bins[PreviewCap::HIST_BINS] = {};
    double n_tot = 0, n_und = 0, n_ovr = 0;
    const float span = PreviewCap::HIST_HI - PreviewCap::HIST_LO;
    auto put = [&](uint8_t* d, float Y, uint8_t R, uint8_t G, uint8_t B) {
        if (g_pv_show_clip && Y > hi_lin)      { d[0] = 235; d[1] = 45;  d[2] = 45;  } // clipped -> red
        else if (g_pv_show_clip && Y < lo_lin) { d[0] = 50;  d[1] = 85;  d[2] = 235; } // crushed -> blue
        else                                   { d[0] = R;   d[1] = G;   d[2] = B;   }
        d[3] = 255;
        n_tot += 1;
        if (Y <= 1e-6f) { n_und += 1; return; }
        float st = log2f(Y);
        if (st < PreviewCap::HIST_LO)  { n_und += 1; return; }
        if (st >= PreviewCap::HIST_HI) { n_ovr += 1; return; }
        int bi = (int)((st - PreviewCap::HIST_LO) / span * PreviewCap::HIST_BINS);
        if (bi < 0) bi = 0; else if (bi >= PreviewCap::HIST_BINS) bi = PreviewCap::HIST_BINS - 1;
        bins[bi]++;
    };

    switch (fmt) {
    case format::r11g11b10_float: {
        // Tone LUTs over the packed code space, same trick as the worker's HDR encode LUTs: R and
        // G are both 11-bit floats so one table covers both channels, B is 10-bit. Rebuilt once
        // per call (gain can change frame to frame in AE mode).
        uint8_t lut_rg[2048], lut_b[1024];
        for (uint32_t c = 0; c < 2048; ++c) lut_rg[c] = tone8(hdr_unpack_f11(c));
        for (uint32_t c = 0; c < 1024; ++c) lut_b[c]  = tone8(hdr_unpack_f10(c));
        for (uint32_t y = 0; y < th; ++y) {
            const uint32_t* srow = reinterpret_cast<const uint32_t*>(src + (size_t)(y * sh / th) * pitch);
            uint8_t* drow = dp + (size_t)y * dst.row_pitch;
            for (uint32_t x = 0; x < tw; ++x) {
                uint32_t u = srow[x * sw / tw];
                uint32_t rc = u & 0x7FF, gc = (u >> 11) & 0x7FF, bc = (u >> 22) & 0x3FF;
                float Y = 0.2126f * hdr_unpack_f11(rc) + 0.7152f * hdr_unpack_f11(gc) + 0.0722f * hdr_unpack_f10(bc);
                put(drow + (size_t)x * 4, Y, lut_rg[rc], lut_rg[gc], lut_b[bc]);
            }
        }
    } break;
    case format::r16g16b16a16_float: {
        // Full 16-bit code -> toned-byte LUT: 64K tone() evals per tick beat ~3 per displayed
        // pixel. Static (render-thread only), rebuilt each call because gain changes.
        static uint8_t lut16[65536];
        for (uint32_t c = 0; c < 65536; ++c) lut16[c] = tone8(pv_half_to_float((uint16_t)c));
        for (uint32_t y = 0; y < th; ++y) {
            const uint16_t* srow = reinterpret_cast<const uint16_t*>(src + (size_t)(y * sh / th) * pitch);
            uint8_t* drow = dp + (size_t)y * dst.row_pitch;
            for (uint32_t x = 0; x < tw; ++x) {
                const uint16_t* px = srow + (size_t)(x * sw / tw) * 4;
                float Y = 0.2126f * pv_half_to_float(px[0]) + 0.7152f * pv_half_to_float(px[1])
                        + 0.0722f * pv_half_to_float(px[2]);
                put(drow + (size_t)x * 4, Y, lut16[px[0]], lut16[px[1]], lut16[px[2]]);
            }
        }
    } break;
    case format::r32g32b32a32_float: {
        for (uint32_t y = 0; y < th; ++y) {
            const float* srow = reinterpret_cast<const float*>(src + (size_t)(y * sh / th) * pitch);
            uint8_t* drow = dp + (size_t)y * dst.row_pitch;
            for (uint32_t x = 0; x < tw; ++x) {
                const float* px = srow + (size_t)(x * sw / tw) * 4;
                float Y = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
                put(drow + (size_t)x * 4, Y, tone8(px[0]), tone8(px[1]), tone8(px[2]));
            }
        }
    } break;
    case format::r9g9b9e5: {
        for (uint32_t y = 0; y < th; ++y) {
            const uint32_t* srow = reinterpret_cast<const uint32_t*>(src + (size_t)(y * sh / th) * pitch);
            uint8_t* drow = dp + (size_t)y * dst.row_pitch;
            for (uint32_t x = 0; x < tw; ++x) {
                float r0, g0, b0; pv_unpack_rgb9e5(srow[x * sw / tw], &r0, &g0, &b0);
                float Y = 0.2126f * r0 + 0.7152f * g0 + 0.0722f * b0;
                put(drow + (size_t)x * 4, Y, tone8(r0), tone8(g0), tone8(b0));
            }
        }
    } break;
    default:
        dev->unmap_texture_region(g_pv.disp, 0);
        g_pv.have_frame = false;
        return;
    }
    dev->unmap_texture_region(g_pv.disp, 0);

    for (int i = 0; i < PreviewCap::HIST_BINS; ++i) g_pv.hist[i] = (float)bins[i]; // raw counts
    g_pv.n_total = (float)n_tot; g_pv.n_under = (float)n_und; g_pv.n_over = (float)n_ovr;
    pv_finalize_hist();
}

// ---- Capture tick (backend dispatch) --------------------------------------------------

// Throttled processing of the previewed buffer. Runs on the present thread (same immediate
// context as the recorder). GPU path on DX11 for every color-like format; CPU staging path
// (HDR float formats only) elsewhere; anything else keeps the plain raw-SRV preview.
// `cap` bounds the CPU bitmap's long side (see preview_process); the GPU path ignores it.
static void preview_capture_tick(effect_runtime* rt, RecordPass* meter_pass, uint32_t cap) {
    device* dev = rt->get_device();
    resource src = { 0 };
    RecordPass* wpass = nullptr;
    if (g_preview_pass) {
        wpass = g_preview_pass;
        resource_view srv = pass_preview_srv(rt, *wpass);
        if (srv.handle) src = dev->get_resource_from_view(srv);
    } else if (g_preview_handle) {
        // Resolve through the registry — NEVER trust the bare handle. This is a raw engine buffer
        // the game owns and can free at any moment; eviction clears g_preview_handle when it sees
        // the free, and this check covers the case where it doesn't (or hasn't yet). Without it,
        // get_resource_desc/copy_texture_region below run on freed memory: a 0xC0000005 the game's
        // own crash handler swallows, so it never even reaches the Windows event log.
        if (!live_handle_is_live(g_preview_handle)) { g_preview_handle = 0; pv_invalidate(); return; }
        src = { g_preview_handle };
    } else if (meter_pass) {
        // Live OSD exposure meter: sample the actively-recording HDR pass (panel closed).
        wpass = meter_pass;
        resource_view srv = pass_preview_srv(rt, *meter_pass);
        if (srv.handle) src = dev->get_resource_from_view(srv);
    }
    if (!src.handle) { pv_invalidate(); return; }
    resource_desc sd = dev->get_resource_desc(src);
    const format   fmt = sd.texture.format;
    const uint32_t sw = sd.texture.width, sh = sd.texture.height;

    // Multisampled sources have no preview path here. The GPU backend's private copy is
    // single-sample and CopySubresourceRegion between mismatched sample counts is invalid — a
    // SILENT D3D11 error, so the display texture keeps whatever it last held while disp_valid goes
    // on claiming the frame is current, i.e. a stale image presented as a live one. (The CPU path
    // happens to fail cleanly.) Bail and name it instead, once per source.
    if (sd.texture.samples > 1) {
        pv_invalidate();
        static uint64_t msaa_reported = 0;
        if (msaa_reported != src.handle) {
            msaa_reported = src.handle;
            char msg[256]; // _TRUNCATE: sprintf_s would ABORT on overflow and pass labels are user-supplied
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        "MultipassRecorder: %s is multisampled (%ux MSAA) - it can't be previewed; pick the resolved buffer instead.",
                        wpass ? pass_display_name(*wpass) : "the previewed buffer",
                        (unsigned)sd.texture.samples);
            reshade::log::message(reshade::log::level::warning, msg);
            g_scan_report = msg + sizeof("MultipassRecorder: ") - 1;
        }
        return;
    }

    const bool hdr_fmt = preview_fmt_hdr(fmt);

    // Exposure gain only when it's real: an encode-HDR pass's enc_exposure is what actually
    // bakes into the file. Raw engine buffers and non-r11 passes preview at identity.
    const bool enc_hdr_pass = wpass && pass_is_hdr_src(*wpass);
    const float gain = (hdr_fmt && enc_hdr_pass) ? powf(2.0f, wpass->enc_exposure) : 1.0f;

    // Capture-window edges in NATIVE linear for the clip/crush tint. Non-encode-HDR sources
    // tint against the PQ default window at identity exposure (same default the scope shows);
    // PNG has no curve window (16-bit linear, effectively never clips) -> tint disabled.
    float lo_lin = 0.0f, hi_lin = 3.4e38f;
    if (hdr_fmt && !(enc_hdr_pass && wpass->enc_codec == ENC_PNG)) {
        float fp, wp, cp;
        curve_window_stops(enc_hdr_pass ? wpass->enc_hdr : HDR_PQ, &fp, &wp, &cp);
        const float e = enc_hdr_pass ? wpass->enc_exposure : 0.0f;
        lo_lin = powf(2.0f, fp - e); hi_lin = powf(2.0f, cp - e);
    }

    // Build the waveform only for the on-screen Viewfinder (meter_pass == nullptr is the
    // panel/debug path; the OSD-meter path passes a pass and never shows the waveform).
    const bool want_wf = g_wf_show && g_preview_open && (meter_pass == nullptr);
    if (preview_gpu_tick(rt, dev, src, fmt, sw, sh, gain, lo_lin, hi_lin, hdr_fmt, want_wf))
        return;

    // ---- CPU fallback: staging copy + readback (HDR float formats only) ----
    g_pvg.disp_valid = false; // GPU path is off for this source; never show its stale frame
    if (!preview_fmt_cpu(fmt)) { g_pv.have_frame = false; return; }

    if (g_pv.w != sw || g_pv.h != sh || g_pv.src_fmt != fmt || !g_pv.staging[0].handle) {
        for (auto& s : g_pv.staging) if (s.handle) { dev->destroy_resource(s); s = { 0 }; }
        if (g_pv.done.handle) { dev->destroy_fence(g_pv.done); g_pv.done = { 0 }; }
        g_pv.copy_index = g_pv.read_index = 0;
        resource_desc st = sd;
        st.type = resource_type::texture_2d; st.heap = memory_heap::gpu_to_cpu;
        st.usage = resource_usage::copy_dest; st.flags = resource_flags::none; st.texture.levels = 1;
        bool ok = dev->create_fence(0, fence_flags::none, &g_pv.done);
        for (int i = 0; ok && i < 2; ++i) ok = dev->create_resource(st, nullptr, resource_usage::copy_dest, &g_pv.staging[i]);
        if (!ok) { pv_free_cpu(dev); return; }
        g_pv.w = sw; g_pv.h = sh; g_pv.src_fmt = fmt;
    }

    command_queue* q = rt->get_command_queue();
    command_list* cmd = q->get_immediate_command_list();
    if (--g_pv.throttle <= 0 && (g_pv.copy_index - g_pv.read_index) < 2) {
        cmd->copy_texture_region(src, 0, nullptr, g_pv.staging[g_pv.copy_index % 2], 0, nullptr, filter_mode::min_mag_mip_point);
        g_pv.copy_index++;
        q->signal(g_pv.done, g_pv.copy_index);
        g_pv.throttle = 4; // ~ capture every 4th present
    }
    if (g_pv.read_index < g_pv.copy_index) {
        uint64_t tv = g_pv.read_index + 1;
        if (dev->wait(g_pv.done, tv, 0)) {
            subresource_data md = {};
            if (dev->map_texture_region(g_pv.staging[(tv - 1) % 2], 0, nullptr, map_access::read_only, &md)) {
                const uint32_t bpp = format_bpp(fmt);
                uint32_t pitch = md.row_pitch < sw * bpp ? sw * bpp : md.row_pitch;
                preview_process(dev, static_cast<const uint8_t*>(md.data), pitch, sw, sh, fmt, gain, lo_lin, hi_lin, cap);
                dev->unmap_texture_region(g_pv.staging[(tv - 1) % 2], 0);
            }
            g_pv.read_index++;
        }
    }
}

// Camera-style auto-exposure: place the scene's highlights just under the pass's capture-window
// ceiling (protect-highlights — the same target as the panel's Fit button), then add the EV
// compensation dial. Runs ONLY while not recording; the value it writes to enc_exposure is what
// bakes into the encoder LUT at worker spawn, so a take is exposure-locked (AE-lock) with no
// mid-shot pumping. Native-stop stats (span_hi) are pre-exposure, so there's no feedback loop.
static void ae_update(RecordPass* p) {
    if (!p || !p->ae_auto) return;
    if (!(g_pv.have_frame && g_pv.n_total > 0)) return;
    float fp, wp, cp; curve_window_stops(p->enc_hdr, &fp, &wp, &cp);
    float target = (cp - 0.5f) - g_pv.span_hi + p->ev_comp; // protect-highlights base + EV comp
    if (target < -6.0f) target = -6.0f; else if (target > 12.0f) target = 12.0f;
    // Viewfinder damping so the value glides as you pan instead of snapping frame-to-frame.
    p->enc_exposure += (target - p->enc_exposure) * 0.15f;
}
