// === MultipassRecorder — mpr_waveform.inc ===
// The RGB waveform / parade scope. A companion to the exposure scope (mpr_ui.inc's
// draw_exposure_scope): where that one is a 1-D luminance-over-stops histogram, this is a 2-D
// *spatial* scope — for each screen COLUMN (x), the distribution of pixel VALUES (y). It's the
// same instrument a video colorist reads: R/G/B traces stacked (overlay), side-by-side (parade),
// or a single luma trace. Pointed at the depth pass it reads as the scene's near->far layout
// across the frame.
//
// Architecture mirrors ReShade's ReGrade addon and reuses this addon's own preview plumbing:
//   * A per-column compute pass (wf_build) walks each output column's slice of the source and
//     accumulates a 256-bin value histogram per channel in GROUPSHARED memory (one group owns
//     one output column, so there is zero cross-group atomic contention), then writes its column
//     out to a structured count buffer.
//   * A resolve pass (wf_resolve) turns those raw counts into a colored RGBA8 display texture
//     (log-ish exponential intensity so faint tails still read), which the UI shows with a plain
//     ImGui::Image — no CPU readback ever touches the waveform.
// Both run on the immediate context right after the preview processor's own dispatch (it already
// made a private, SRV-legal copy of the source in g_pvg.priv), on the same every-4th-frame
// throttle. DX11 only, like the rest of the GPU preview path; there is no CPU fallback (the scope
// just isn't offered when the GPU path is unavailable).
//
// Part of the main.cpp unity build: #included after mpr_preview.inc (it is invoked from
// preview_gpu_tick via a forward declaration there) and before mpr_control.inc (which calls
// waveform_free on runtime teardown). Not a standalone TU — see CLAUDE.md > 'Source layout'.

static constexpr uint32_t WF_COLS = 384;   // horizontal resolution (screen-column buckets)
static constexpr uint32_t WF_BINS = 256;   // vertical resolution (value buckets, 0..1 => 0..255)

// ---- Compute shaders ------------------------------------------------------------------
// Build: one thread group per output column. 256 threads stride the column's pixels, atomic-add
// into a groupshared [256 bins * 4 channels] table (R,G,B,Luma), then thread `gi` writes bin gi.
static const char WF_BUILD_HLSL[] = R"(
cbuffer WfCB : register(b0) {
    uint g_sw, g_sh, g_cols, g_flags;   // flags bit1 = HDR source (tonemap value like the preview)
    float g_gain, g_kexp; uint g_mode, g_bins;
};
Texture2D<float4>        S : register(t0);
RWStructuredBuffer<uint> W : register(u0);   // (col*256 + bin)*4 + channel
groupshared uint gb[256 * 4];

float3 wf_map(float3 c) {
    if (g_flags & 2u) { float3 v = max(c * g_gain, 0.0); v = v / (1.0 + v); return pow(v, 1.0 / 2.2); }
    return saturate(c);                       // LDR / data (e.g. depth) : raw 0..1
}

[numthreads(256, 1, 1)]
void wf_build(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    uint i;
    for (i = gi; i < 256u * 4u; i += 256u) gb[i] = 0u;
    GroupMemoryBarrierWithGroupSync();

    uint col = gid.x;
    if (col < g_cols) {
        uint span_lo = col * g_sw / g_cols;
        uint span_hi = (col + 1u) * g_sw / g_cols;
        uint span_w  = max(span_hi - span_lo, 1u);
        uint total   = span_w * g_sh;
        for (i = gi; i < total; i += 256u) {
            uint sx = span_lo + (i % span_w);
            uint sy = i / span_w;
            if (sx >= g_sw || sy >= g_sh) continue;
            float3 src = S.Load(int3(sx, sy, 0)).rgb;
            if (g_flags & 4u) src = src.rrr;  // single-channel source: R-only would trace one line
            float3 v = wf_map(src);
            float luma = dot(v, float3(0.2126, 0.7152, 0.0722));
            InterlockedAdd(gb[min((uint)(v.r * 255.0 + 0.5), 255u) * 4u + 0u], 1u);
            InterlockedAdd(gb[min((uint)(v.g * 255.0 + 0.5), 255u) * 4u + 1u], 1u);
            InterlockedAdd(gb[min((uint)(v.b * 255.0 + 0.5), 255u) * 4u + 2u], 1u);
            InterlockedAdd(gb[min((uint)(luma * 255.0 + 0.5), 255u) * 4u + 3u], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (col < g_cols) {                          // gi in [0,256): one thread per value-bin
        uint base = (col * 256u + gi) * 4u;
        W[base + 0u] = gb[gi * 4u + 0u];
        W[base + 1u] = gb[gi * 4u + 1u];
        W[base + 2u] = gb[gi * 4u + 2u];
        W[base + 3u] = gb[gi * 4u + 3u];
    }
}
)";

// Resolve: color the counts into the display texture. Row 0 = top = white (high value).
static const char WF_RESOLVE_HLSL[] = R"(
cbuffer WfCB : register(b0) {
    uint g_sw, g_sh, g_cols, g_flags;
    float g_gain, g_kexp; uint g_mode, g_bins;
};
StructuredBuffer<uint>    W : register(t0);
RWTexture2D<unorm float4> D : register(u0);

float wf_int(uint count) { return 1.0 - exp(-(float)count * g_kexp); }  // faint tails still show

[numthreads(8, 8, 1)]
void wf_resolve(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_cols || id.y >= g_bins) return;
    uint bin = (g_bins - 1u) - id.y;             // flip so bright values sit at the top
    float3 outc = 0.02;                          // near-black background
    if (bin == 0u || bin == 64u || bin == 128u || bin == 192u || bin == g_bins - 1u)
        outc += 0.05;                            // value graticule at 0 / .25 / .5 / .75 / 1

    if (g_mode == 1u) {                          // parade: R | G | B panels
        uint panel = max(g_cols / 3u, 1u);
        uint third = min(id.x / panel, 2u);
        uint local = id.x - third * panel;
        uint scol  = min(local * g_cols / panel, g_cols - 1u);
        float I = wf_int(W[(scol * 256u + bin) * 4u + third]);
        float3 tint = third == 0u ? float3(1.0, 0.28, 0.28)
                    : third == 1u ? float3(0.35, 1.0, 0.35)
                                  : float3(0.45, 0.60, 1.0);
        outc += tint * I;
        if (local == 0u && third > 0u) outc += 0.10; // panel separators
    } else if (g_mode == 2u) {                   // luma trace
        outc += wf_int(W[(id.x * 256u + bin) * 4u + 3u]).xxx;
    } else {                                     // overlay R,G,B
        outc += float3(wf_int(W[(id.x * 256u + bin) * 4u + 0u]),
                       wf_int(W[(id.x * 256u + bin) * 4u + 1u]),
                       wf_int(W[(id.x * 256u + bin) * 4u + 2u]));
    }
    D[id.xy] = float4(saturate(outc), 1.0);
}
)";

// ---- GPU state ------------------------------------------------------------------------
// All objects are source-size-independent (the scope always bins into WF_COLS x WF_BINS regardless
// of the source resolution), so everything is created once per device lifetime.
struct WfGpu {
    bool tried = false, ok = false, valid = false; // compiled / usable / disp holds a fresh frame
    ID3D11ComputeShader*       cs_build   = nullptr;
    ID3D11ComputeShader*       cs_resolve = nullptr;
    ID3D11Buffer*              cb         = nullptr;
    ID3D11Buffer*              counts     = nullptr;   // WF_COLS*WF_BINS*4 uints
    ID3D11UnorderedAccessView* counts_uav = nullptr;
    ID3D11ShaderResourceView*  counts_srv = nullptr;
    ID3D11Texture2D*           disp       = nullptr;   // WF_COLS x WF_BINS RGBA8 shown by ImGui
    ID3D11UnorderedAccessView* disp_uav   = nullptr;
    ID3D11ShaderResourceView*  disp_srv   = nullptr;
};
static WfGpu g_wf;

// Mirror of the HLSL cbuffer (two 16-byte rows).
struct WfCB { uint32_t sw, sh, cols, flags; float gain, kexp; uint32_t mode, bins; };

template <typename T> static void wf_com_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void waveform_free() {
    wf_com_release(g_wf.counts_srv); wf_com_release(g_wf.counts_uav); wf_com_release(g_wf.counts);
    wf_com_release(g_wf.disp_srv);   wf_com_release(g_wf.disp_uav);   wf_com_release(g_wf.disp);
    wf_com_release(g_wf.cb);
    wf_com_release(g_wf.cs_resolve); wf_com_release(g_wf.cs_build);
    g_wf.ok = false; g_wf.tried = false; g_wf.valid = false; // re-init cleanly on the next device
}

static ID3D11ComputeShader* wf_compile(ID3D11Device* d11, const char* src, size_t len, const char* entry) {
    static HMODULE dcomp = LoadLibraryA("d3dcompiler_47.dll");
    typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    static PFN_Compile compile = dcomp ? (PFN_Compile)GetProcAddress(dcomp, "D3DCompile") : nullptr;
    if (!compile) return nullptr;
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = compile(src, len, "mpr_waveform_cs", nullptr, nullptr, entry, "cs_5_0",
                         (1u << 15) /*OPTIMIZATION_LEVEL3*/, 0, &blob, &err);
    if (FAILED(hr) || !blob) {
        char msg[256];
        sprintf_s(msg, "MultipassRecorder: waveform CS '%s' compile failed (0x%08lx)%s%.120s", entry,
                  (unsigned long)hr, err ? ": " : "", err ? (const char*)err->GetBufferPointer() : "");
        reshade::log::message(reshade::log::level::warning, msg);
        if (err) err->Release(); if (blob) blob->Release();
        return nullptr;
    }
    if (err) err->Release();
    ID3D11ComputeShader* cs = nullptr;
    hr = d11->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
    blob->Release();
    return SUCCEEDED(hr) ? cs : nullptr;
}

static bool waveform_gpu_init(ID3D11Device* d11) {
    if (g_wf.tried) return g_wf.ok;
    g_wf.tried = true;

    g_wf.cs_build   = wf_compile(d11, WF_BUILD_HLSL,   sizeof(WF_BUILD_HLSL)   - 1, "wf_build");
    g_wf.cs_resolve = wf_compile(d11, WF_RESOLVE_HLSL, sizeof(WF_RESOLVE_HLSL) - 1, "wf_resolve");
    if (!g_wf.cs_build || !g_wf.cs_resolve) { waveform_free(); g_wf.tried = true; return false; }

    bool ok = true;
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(WfCB); cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(d11->CreateBuffer(&cbd, nullptr, &g_wf.cb));

    const UINT nelem = WF_COLS * WF_BINS * 4;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = nelem * 4; bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    ok = ok && SUCCEEDED(d11->CreateBuffer(&bd, nullptr, &g_wf.counts));
    if (ok) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = nelem;
        ok = SUCCEEDED(d11->CreateUnorderedAccessView(g_wf.counts, &ud, &g_wf.counts_uav));
    }
    if (ok) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = nelem;
        ok = SUCCEEDED(d11->CreateShaderResourceView(g_wf.counts, &sd, &g_wf.counts_srv));
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = WF_COLS; td.Height = WF_BINS; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ok = ok && SUCCEEDED(d11->CreateTexture2D(&td, nullptr, &g_wf.disp));
    ok = ok && SUCCEEDED(d11->CreateUnorderedAccessView(g_wf.disp, nullptr, &g_wf.disp_uav));
    ok = ok && SUCCEEDED(d11->CreateShaderResourceView(g_wf.disp, nullptr, &g_wf.disp_srv));

    if (!ok) { waveform_free(); g_wf.tried = true; return false; }
    g_wf.ok = true;
    return true;
}

// Build + resolve one waveform frame from `src_srv` (the preview processor's private source copy).
// gain/hdr_mode match the preview: HDR sources are tonemapped to the same WYSIWYG value the image
// shows, so 0..1 bins are meaningful; LDR/data sources bin their raw value. Runs on the immediate
// context; saves and restores the game's compute state exactly, like preview_gpu_tick.
static void waveform_capture(ID3D11Device* d11, ID3D11DeviceContext* ctx,
                             ID3D11ShaderResourceView* src_srv,
                             uint32_t sw, uint32_t sh, float gain, bool hdr_mode, bool mono) {
    if (!src_srv || sw == 0 || sh == 0) return;
    if (!waveform_gpu_init(d11)) return;

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(ctx->Map(g_wf.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        WfCB p = {};
        p.sw = sw; p.sh = sh; p.cols = WF_COLS;
        p.flags = (hdr_mode ? 2u : 0u) | (mono ? 4u : 0u);
        p.gain = gain; p.mode = (uint32_t)g_wf_mode; p.bins = WF_BINS;
        // Exponential intensity auto-scaled by the pixels-per-column so brightness reads the same
        // regardless of source resolution; g_wf_bright is the user's trace-brightness dial.
        float rows_per_col = ((float)sw / WF_COLS) * (float)sh;
        p.kexp = g_wf_bright / (rows_per_col < 1.0f ? 1.0f : rows_per_col);
        memcpy(m.pData, &p, sizeof(p));
        ctx->Unmap(g_wf.cb, 0);
    }

    // Save the game's CS state (u0/u1 span both our and any bound game UAVs; t0 + b0 too).
    ID3D11ComputeShader* oldCS = nullptr; ID3D11ClassInstance* oldInst[16] = {}; UINT nInst = 16;
    ctx->CSGetShader(&oldCS, oldInst, &nInst);
    ID3D11UnorderedAccessView* oldUAV[1] = {}; ctx->CSGetUnorderedAccessViews(0, 1, oldUAV);
    ID3D11ShaderResourceView*  oldSRV[1] = {}; ctx->CSGetShaderResources(0, 1, oldSRV);
    ID3D11Buffer*              oldCB[1]  = {}; ctx->CSGetConstantBuffers(0, 1, oldCB);

    ID3D11UnorderedAccessView* nullu = nullptr;

    // Pass 1 — build the per-column counts.
    ctx->CSSetConstantBuffers(0, 1, &g_wf.cb);
    ctx->CSSetShader(g_wf.cs_build, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &src_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_wf.counts_uav, nullptr);
    ctx->Dispatch(WF_COLS, 1, 1);

    // Pass 2 — resolve to the display texture. Unbind the count UAV first, then read it as an SRV
    // (DX11 tracks the UAV->SRV hazard between dispatches automatically).
    ctx->CSSetUnorderedAccessViews(0, 1, &nullu, nullptr);
    ctx->CSSetShader(g_wf.cs_resolve, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &g_wf.counts_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_wf.disp_uav, nullptr);
    ctx->Dispatch((WF_COLS + 7) / 8, (WF_BINS + 7) / 8, 1);

    // Restore exactly what we found.
    ctx->CSSetShader(oldCS, nInst ? oldInst : nullptr, nInst);
    ctx->CSSetUnorderedAccessViews(0, 1, oldUAV, nullptr);
    ctx->CSSetShaderResources(0, 1, oldSRV);
    ctx->CSSetConstantBuffers(0, 1, oldCB);
    wf_com_release(oldCS);
    for (UINT i = 0; i < nInst; ++i) wf_com_release(oldInst[i]);
    wf_com_release(oldUAV[0]); wf_com_release(oldSRV[0]); wf_com_release(oldCB[0]);

    g_wf.valid = true;
}

// The waveform display texture as a resource_view for ImGui ({0} = nothing built yet).
static resource_view waveform_display_srv() {
    if (g_wf.valid && g_wf.disp_srv) return { (uint64_t)(uintptr_t)g_wf.disp_srv };
    return { 0 };
}
