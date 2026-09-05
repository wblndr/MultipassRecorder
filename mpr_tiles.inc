// === MultipassRecorder — mpr_tiles.inc ===
// The Record tab's "At a glance" strip: one small, correctly-exposed thumbnail per pass, all
// live at once, plus the per-pass range split that drives each tile's crush/keep/clip bar.
//
// Why this exists as its own subsystem rather than more thumbnails in mpr_ui.inc: the raw SRV
// the pass table already draws is the buffer's LITERAL contents, and for the passes you would
// actually worry about that is a lie. An r11g11b10 scene buffer previewed raw reads as blown-out
// white; a depth buffer reads as near-black. Both look exactly like "this pass is broken", so a
// wall of raw thumbnails would generate more false alarms than it catches real ones. These tiles
// run the same tonemap + exposure the Viewfinder does, so a black tile means a black buffer.
//
// Relationship to mpr_preview.inc: this reuses that file's compiled compute shader (one shader,
// one meaning of "exposed view") but owns everything else. In particular it has its OWN constant
// buffer, histogram buffer and readback pair, because the Viewfinder dispatches from the same
// present and would otherwise overwrite this pass's statistics with the previewed pass's.
//
// Cost model — this is on the render thread, and it keeps running during a take:
//   * ONE pass is processed per tick, round-robin. With the panel open on the Record tab and a
//     typical 4-8 visible passes, every tile refreshes ~8-15x/second, which is far more than a
//     "has anything gone wrong" check needs.
//   * The dispatch is into a ~192px tile (the shader's integer downsample does the work), so the
//     real per-tick cost is the source copy, not the shader.
//   * The whole thing is gated at the call site on the panel being open on the Record tab, so a
//     closed panel — the normal state while shooting — costs nothing at all.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.

// Long side of a tile's processed texture. Small on purpose: it is displayed at ~110-165px in a
// narrow panel column, and every pixel here is per-pass GPU memory that exists all session.
static constexpr uint32_t TILE_LONG = 192;

// Which passes the Record tab (list AND strip) shows. Single source of truth so the strip can
// never show a different set than the table under it.
// Index 3 (normals_native) is the ENGINE section, deliberately hidden from the Record tab — the
// pass still exists in the backend and the Buffers tab still exposes every engine buffer.
static constexpr int GLANCE_HIDDEN_FIXED = 3;
static bool glance_shows(int i) {
    if (i == GLANCE_HIDDEN_FIXED) return false;
    const RecordPass& p = g_passes[i];
    return p.is_custom ? p.promoted : true;
}

struct TileSlot {
    ID3D11Texture2D*           disp     = nullptr; // processed RGBA8 the strip samples
    ID3D11UnorderedAccessView* disp_uav = nullptr;
    ID3D11ShaderResourceView*  disp_srv = nullptr;
    uint32_t disp_w = 0, disp_h = 0;
    uint32_t src_w  = 0, src_h  = 0;      // source dims disp was sized for
    bool     valid  = false;              // disp holds a processed frame of the CURRENT source
    bool     hdr    = false;              // last dispatch ran the HDR branch -> a histogram exists
    uint32_t gen    = 0;                  // bumped on every resize/reset; tags in-flight readbacks

    // Raw native-stop histogram of the last completed readback. Stored raw (not already split
    // into zones) so the crush/keep/clip bar re-derives against the pass's CURRENT exposure and
    // codec window every frame — dragging the Exposure slider moves the bar immediately instead
    // of waiting for the round-robin to come back around to this pass.
    bool     have_stats = false;
    float    hist[PreviewCap::HIST_BINS] = {};
    float    n_under = 0, n_over = 0, n_total = 0;
    uint32_t stats_frame = 0;             // g_live_frame the stats landed on (staleness)
};
static TileSlot g_tiles[N_FIXED + N_CUSTOM];

// Shared source scratch. The live source may be RTV-bound at present time, which makes a direct
// SRV bind silently read nothing (same reason mpr_preview.inc keeps a private copy), so every
// dispatch goes through a GPU->GPU copy. ONE scratch for all tiles, resized only on a genuine
// mismatch: a game's passes are overwhelmingly the same resolution, so round-robining through
// them almost never reallocates.
static ID3D11Texture2D*          g_tile_scr     = nullptr;
static ID3D11ShaderResourceView* g_tile_scr_srv = nullptr;
static uint32_t                  g_tile_scr_w = 0, g_tile_scr_h = 0;
static DXGI_FORMAT               g_tile_scr_fmt = DXGI_FORMAT_UNKNOWN;

// Our own CB + histogram + readback pair (see the header comment: sharing the Viewfinder's would
// cross-contaminate statistics between two dispatches in the same present).
static ID3D11Buffer*              g_tile_cb       = nullptr;
static ID3D11Buffer*              g_tile_hist     = nullptr;
static ID3D11UnorderedAccessView* g_tile_hist_uav = nullptr;
static ID3D11Buffer*              g_tile_hist_rb[2] = {};
static bool                       g_tile_rb_pending[2] = {};
// Which slot issued each in-flight readback, and that slot's generation at issue time. A readback
// completes ~2 frames after its dispatch, by which time the round-robin has moved on — without
// this tag the result would be applied to whichever pass happens to be current, so pass A's
// clipping would surface as an amber warning on pass B's tile. Silently, and plausibly.
static int                        g_tile_rb_slot[2] = { -1, -1 };
static uint32_t                   g_tile_rb_gen[2]  = {};
static int                        g_tile_rb_next = 0;
static int                        g_tile_rr = -1;   // round-robin cursor
static bool                       g_tile_tried = false, g_tile_ok = false;

// Releasing a slot destroys an SRV the overlay may have drawn with THIS frame. That is safe here,
// and deliberately does NOT need the retire-a-few-frames dance the live-RT thumbnails use
// (g_live_retired, mpr_livert.inc) -- do not "fix" this by copying that pattern:
//   * ReShade finishes draw_gui() (build AND render of the overlay draw list) before it invokes
//     reshade_present, which is the only place tile_tick runs. No in-flight draw list can still
//     name a view we destroy here.
//   * These views only ever exist on D3D11, where the runtime keeps a submitted draw's resources
//     alive until the GPU is done with them. The retirement queue exists because D3D12 does not.
static void tile_release_slot(TileSlot& s) {
    pv_com_release(s.disp_uav); pv_com_release(s.disp_srv); pv_com_release(s.disp);
    s.disp_w = s.disp_h = s.src_w = s.src_h = 0;
    s.valid = s.hdr = s.have_stats = false;
    s.gen++;   // orphan any readback still in flight for this slot
}

static void tile_free() {
    for (auto& s : g_tiles) tile_release_slot(s);
    pv_com_release(g_tile_scr_srv); pv_com_release(g_tile_scr);
    g_tile_scr_w = g_tile_scr_h = 0; g_tile_scr_fmt = DXGI_FORMAT_UNKNOWN;
    pv_com_release(g_tile_hist_uav); pv_com_release(g_tile_hist);
    pv_com_release(g_tile_hist_rb[0]); pv_com_release(g_tile_hist_rb[1]);
    pv_com_release(g_tile_cb);
    g_tile_rb_pending[0] = g_tile_rb_pending[1] = false;
    g_tile_rb_slot[0] = g_tile_rb_slot[1] = -1;
    g_tile_rb_next = 0; g_tile_rr = -1;
    g_tile_tried = false; g_tile_ok = false;   // re-init cleanly on the next device
}

// Device-lifetime objects. The compute shader itself comes from the preview subsystem — one
// shader means the tiles and the Viewfinder can't drift apart on what "exposed" looks like.
static bool tile_init(ID3D11Device* d11) {
    if (g_tile_tried) return g_tile_ok;
    g_tile_tried = true;
    if (!pv_gpu_init(d11)) return false;   // no shader -> tiles fall back to the raw SRV

    bool ok = true;
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(PvGpuParams); cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(d11->CreateBuffer(&cbd, nullptr, &g_tile_cb));

    D3D11_BUFFER_DESC hbd = {};
    hbd.ByteWidth = PV_HIST_UINTS * 4; hbd.Usage = D3D11_USAGE_DEFAULT;
    hbd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; hbd.StructureByteStride = 4;
    ok = ok && SUCCEEDED(d11->CreateBuffer(&hbd, nullptr, &g_tile_hist));
    if (ok) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = PV_HIST_UINTS;
        ok = SUCCEEDED(d11->CreateUnorderedAccessView(g_tile_hist, &ud, &g_tile_hist_uav));
    }
    D3D11_BUFFER_DESC rbd = {};
    rbd.ByteWidth = PV_HIST_UINTS * 4; rbd.Usage = D3D11_USAGE_STAGING;
    rbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; ok && i < 2; ++i) ok = SUCCEEDED(d11->CreateBuffer(&rbd, nullptr, &g_tile_hist_rb[i]));

    if (!ok) { tile_free(); g_tile_tried = true; return false; }
    g_tile_ok = true;
    return true;
}

// Per-slot display texture, sized to the source's aspect with the long side capped at TILE_LONG.
static bool tile_ensure_slot(ID3D11Device* d11, TileSlot& s, uint32_t sw, uint32_t sh) {
    if (s.disp && s.src_w == sw && s.src_h == sh) return true;
    tile_release_slot(s);
    if (sw == 0 || sh == 0) return false;

    uint32_t dw = sw, dh = sh;
    if (dw >= dh) { if (dw > TILE_LONG) { dh = dh * TILE_LONG / dw; dw = TILE_LONG; } }
    else          { if (dh > TILE_LONG) { dw = dw * TILE_LONG / dh; dh = TILE_LONG; } }
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = dw; dd.Height = dh; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(d11->CreateTexture2D(&dd, nullptr, &s.disp)) ||
        FAILED(d11->CreateUnorderedAccessView(s.disp, nullptr, &s.disp_uav)) ||
        FAILED(d11->CreateShaderResourceView(s.disp, nullptr, &s.disp_srv))) {
        tile_release_slot(s); return false;
    }
    s.disp_w = dw; s.disp_h = dh; s.src_w = sw; s.src_h = sh;
    return true;
}

static bool tile_ensure_scratch(ID3D11Device* d11, uint32_t sw, uint32_t sh, DXGI_FORMAT fmt) {
    if (g_tile_scr && g_tile_scr_w == sw && g_tile_scr_h == sh && g_tile_scr_fmt == fmt) return true;
    pv_com_release(g_tile_scr_srv); pv_com_release(g_tile_scr);
    g_tile_scr_w = g_tile_scr_h = 0; g_tile_scr_fmt = DXGI_FORMAT_UNKNOWN;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = sw; td.Height = sh; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d11->CreateTexture2D(&td, nullptr, &g_tile_scr)) ||
        FAILED(d11->CreateShaderResourceView(g_tile_scr, nullptr, &g_tile_scr_srv))) {
        pv_com_release(g_tile_scr_srv); pv_com_release(g_tile_scr);
        return false;
    }
    g_tile_scr_w = sw; g_tile_scr_h = sh; g_tile_scr_fmt = fmt;
    return true;
}

// Drain finished histogram readbacks onto the slot that ISSUED them (see g_tile_rb_slot).
static void tile_drain_readbacks(ID3D11DeviceContext* ctx) {
    for (int i = 0; i < 2; ++i) {
        if (!g_tile_rb_pending[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m = {};
        HRESULT hr = ctx->Map(g_tile_hist_rb[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr)) {
            const int slot = g_tile_rb_slot[i];
            // Drop the result if the slot was resized, blanked or un-promoted while this readback
            // was in flight -- those stats describe a buffer that is no longer what the tile shows.
            if (slot >= 0 && slot < N_FIXED + N_CUSTOM && g_tiles[slot].gen == g_tile_rb_gen[i]) {
                TileSlot& s = g_tiles[slot];
                const uint32_t* hb = static_cast<const uint32_t*>(m.pData);
                for (int b = 0; b < PreviewCap::HIST_BINS; ++b) s.hist[b] = (float)hb[b];
                s.n_under = (float)hb[128]; s.n_over = (float)hb[129]; s.n_total = (float)hb[130];
                s.have_stats = s.n_total > 0;
                s.stats_frame = g_live_frame;
            }
            ctx->Unmap(g_tile_hist_rb[i], 0);
            g_tile_rb_pending[i] = false; g_tile_rb_slot[i] = -1;
        } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            g_tile_rb_pending[i] = false; g_tile_rb_slot[i] = -1;
        }
    }
}

// One tick: drain readbacks, then process exactly ONE pass (round-robin). Call only while the
// strip is actually visible -- see the gate in on_reshade_present.
static void tile_tick(effect_runtime* rt) {
    device* dev = rt->get_device();
    if (dev->get_api() != device_api::d3d11) return;   // tiles fall back to the raw SRV elsewhere
    ID3D11Device* d11 = reinterpret_cast<ID3D11Device*>(dev->get_native());
    if (!tile_init(d11)) return;
    ID3D11DeviceContext* ctx = reinterpret_cast<ID3D11DeviceContext*>(
        rt->get_command_queue()->get_immediate_command_list()->get_native());

    tile_drain_readbacks(ctx);

    // Advance to the next visible pass. Bounded by the slot count, so "nothing visible" just
    // leaves the cursor where it was rather than spinning.
    const int N = N_FIXED + N_CUSTOM;
    int idx = -1;
    for (int n = 0; n < N; ++n) {
        const int cand = (g_tile_rr + 1 + n) % N;
        if (glance_shows(cand)) { idx = cand; break; }
    }
    if (idx < 0) return;
    g_tile_rr = idx;

    RecordPass& p = g_passes[idx];
    TileSlot&   s = g_tiles[idx];

    resource_view srv = pass_preview_srv(rt, p);
    if (!srv.handle) { s.valid = false; s.have_stats = false; return; } // not bound this frame
    resource src = dev->get_resource_from_view(srv);
    if (!src.handle) { s.valid = false; s.have_stats = false; return; }
    const resource_desc sd = dev->get_resource_desc(src);
    const format   fmt = sd.texture.format;
    const uint32_t sw = sd.texture.width, sh = sd.texture.height;

    // Multisampled sources have no tile: CopySubresourceRegion between mismatched sample counts
    // is a SILENT D3D11 error, so the copy would no-op and the tile would keep presenting a stale
    // image as a live one. The Viewfinder reports this per source already; here we just go blank.
    if (sd.texture.samples > 1 || !preview_fmt_gpu(fmt)) { s.valid = false; s.have_stats = false; return; }
    if (!tile_ensure_slot(d11, s, sw, sh)) return;
    if (!tile_ensure_scratch(d11, sw, sh, (DXGI_FORMAT)format_to_default_typed(fmt))) return;

    // Exposure/window derivation is deliberately identical to preview_capture_tick's: gain applies
    // only where it is real (an encode-HDR pass's enc_exposure is what actually bakes into the
    // file), so a tile shows what the recording will hold, not a prettier version of it.
    const bool hdr_fmt = preview_fmt_hdr(fmt);
    const bool enc_hdr_pass = pass_is_hdr_src(p);
    const float gain = (hdr_fmt && enc_hdr_pass) ? powf(2.0f, p.enc_exposure) : 1.0f;

    ctx->CopySubresourceRegion(g_tile_scr, 0, 0, 0, 0,
                               reinterpret_cast<ID3D11Resource*>((uintptr_t)src.handle), 0, nullptr);

    // Skip the dispatch outright if the parameters didn't land: running the shader against the
    // PREVIOUS tile's gain/dimensions would silently mis-expose this tile and mis-attribute its
    // histogram -- exactly the "plausible but wrong" failure the strip exists to prevent.
    D3D11_MAPPED_SUBRESOURCE cm = {};
    if (FAILED(ctx->Map(g_tile_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) return;
    {
        PvGpuParams prm = {};
        prm.gain = gain; prm.lo_lin = 0.0f; prm.hi_lin = 3.4e38f;
        // No clip/crush false-colour (bit0 clear): at tile size the red/blue splatter obscures the
        // image, and the range bar under the tile carries that information properly.
        prm.flags = (hdr_fmt ? 2u : 0u) | (preview_fmt_mono(fmt) ? 4u : 0u); // bit2: R-only -> grey, not red
        prm.sw = sw; prm.sh = sh; prm.dw = s.disp_w; prm.dh = s.disp_h;
        prm.channel = 0;   // always full RGB; channel isolation is a Viewfinder tool
        memcpy(cm.pData, &prm, sizeof(prm));
        ctx->Unmap(g_tile_cb, 0);
    }

    const UINT zero4[4] = {};
    ctx->ClearUnorderedAccessViewUint(g_tile_hist_uav, zero4);

    // Save the game's CS state, run ours, restore exactly -- DX11 games legitimately rely on
    // compute state persisting across Present, so we must not clobber it.
    ID3D11ComputeShader* oldCS = nullptr; ID3D11ClassInstance* oldInst[16] = {}; UINT nInst = 16;
    ctx->CSGetShader(&oldCS, oldInst, &nInst);
    ID3D11UnorderedAccessView* oldUAV[2] = {};
    ctx->CSGetUnorderedAccessViews(0, 2, oldUAV);
    ID3D11ShaderResourceView* oldSRV = nullptr;
    ctx->CSGetShaderResources(0, 1, &oldSRV);
    ID3D11Buffer* oldCB = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCB);

    ID3D11UnorderedAccessView* uavs[2] = { s.disp_uav, g_tile_hist_uav };
    ctx->CSSetShader(g_pvg.cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_tile_cb);
    ctx->CSSetShaderResources(0, 1, &g_tile_scr_srv);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ctx->Dispatch((s.disp_w + 7) / 8, (s.disp_h + 7) / 8, 1);

    ctx->CSSetShader(oldCS, nInst ? oldInst : nullptr, nInst);
    ctx->CSSetUnorderedAccessViews(0, 2, oldUAV, nullptr);
    ctx->CSSetShaderResources(0, 1, &oldSRV);
    ctx->CSSetConstantBuffers(0, 1, &oldCB);
    pv_com_release(oldCS);
    for (UINT i = 0; i < nInst; ++i) pv_com_release(oldInst[i]);
    pv_com_release(oldUAV[0]); pv_com_release(oldUAV[1]);
    pv_com_release(oldSRV); pv_com_release(oldCB);

    s.valid = true;
    s.hdr   = hdr_fmt;
    if (!hdr_fmt) s.have_stats = false;   // LDR dispatch writes no histogram; kill stale HDR stats

    if (hdr_fmt && !g_tile_rb_pending[g_tile_rb_next]) {
        ctx->CopyResource(g_tile_hist_rb[g_tile_rb_next], g_tile_hist);
        g_tile_rb_pending[g_tile_rb_next] = true;
        g_tile_rb_slot[g_tile_rb_next]    = idx;
        g_tile_rb_gen[g_tile_rb_next]     = s.gen;
        g_tile_rb_next ^= 1;
    }
}

// ---- Accessors for the strip (mpr_ui.inc) ----

// The processed tile, or {0} when there is none (no GPU path, unsupported format, not bound yet)
// -- the caller then falls back to the pass's raw SRV so a tile is never simply missing.
static resource_view tile_display_srv(int idx, uint32_t* w = nullptr, uint32_t* h = nullptr) {
    if (idx < 0 || idx >= N_FIXED + N_CUSTOM) return { 0 };
    const TileSlot& s = g_tiles[idx];
    if (!s.valid || !s.disp_srv) return { 0 };
    if (w) *w = s.disp_w; if (h) *h = s.disp_h;
    return { (uint64_t)(uintptr_t)s.disp_srv };
}

// Range split of this tile against the pass's CURRENT capture window, as fractions of the frame.
// False when this pass has no meaningful answer (LDR source, no histogram yet, PNG's linear
// 16-bit path which effectively never clips) -- the strip then draws no bar rather than a
// confident-looking wrong one.
static bool tile_range_split(int idx, const RecordPass& p, float* crush, float* keep, float* clip) {
    if (idx < 0 || idx >= N_FIXED + N_CUSTOM) return false;
    const TileSlot& s = g_tiles[idx];
    if (!s.have_stats || !s.hdr || s.n_total <= 0) return false;
    if (!pass_is_hdr_src(p)) return false;              // window belongs to the encode-HDR path only
    if (p.enc_codec == ENC_PNG) return false;           // 16-bit linear: no curve window to fall out of

    float fp, wp, cp;
    curve_window_stops(p.enc_hdr, &fp, &wp, &cp);
    double cr, kp, cl, tot;
    hist_population(s.hist, s.n_under, s.n_over, s.n_total, p.enc_exposure, fp, cp, &cr, &kp, &cl, &tot);
    if (tot <= 0) return false;
    *crush = (float)(cr / tot); *keep = (float)(kp / tot); *clip = (float)(cl / tot);
    return true;
}
