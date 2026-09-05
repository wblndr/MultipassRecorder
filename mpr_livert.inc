// === MultipassRecorder — mpr_livert.inc ===
// Engine buffer discovery — live-RT registry hooks, engine-RT fingerprint resolution, auto-profiler.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.

// Live registry: find-or-add a bound engine resource by handle, tracking per-frame bind counts
// and first-seen order for the Buffers gallery + engine fingerprint resolution. `how` records whether
// this bind was a color render target (LIVE_RTV) or a compute UAV (LIVE_UAV); a resource bound
// both ways accumulates both bits. Returns the registry index, or -1 if full/invalid. Cheap: a
// linear scan over <=64 slots, under g_live_mutex — D3D12 titles record binds on parallel worker
// threads, so this is NOT render-thread-only despite what the DX11-era design assumed.
static int live_touch(device* dev, uint64_t handle, uint8_t how) {
    if (handle == 0) return -1;
    std::lock_guard<std::mutex> lock(g_live_mutex); // D3D12 records binds on parallel threads
    // The registry is capped at 64 slots, so reserving once up front means the vector never
    // reallocates -- the overlay can hold LiveRT* across a frame's draw without them dangling.
    if (g_live_rts.capacity() < 64) g_live_rts.reserve(64);
    if (g_live_seen_frame != g_live_frame) { g_live_seen_frame = g_live_frame; g_live_order_ctr = 0; }
    int idx = -1, free_slot = -1;
    for (size_t k = 0; k < g_live_rts.size(); ++k) {
        if (g_live_rts[k].handle == handle) { idx = (int)k; break; }
        if (g_live_rts[k].handle == 0 && free_slot < 0) free_slot = (int)k;
    }
    if (idx < 0) {
        // Prefer a slot the game freed (eviction blanks rather than erases, so indices stay put).
        if (free_slot >= 0) idx = free_slot;
        else if (g_live_rts.size() < 64) { g_live_rts.push_back(LiveRT{}); idx = (int)g_live_rts.size() - 1; }
        else return -1;
        LiveRT& n = g_live_rts[idx];
        n = LiveRT{};                                   // fully reset (a reused slot carries stale counters)
        n.desc = dev->get_resource_desc({ handle });
        n.seq = g_live_seq_ctr++;                       // stable row identity for the session
        n.order = n.order_raw = g_live_order_ctr;       // seed, so a new row doesn't read "o00" for 8 frames
        n.handle.publish(handle);                       // published LAST: readers skip handle == 0
    }
    LiveRT& s = g_live_rts[idx];
    if (s.last_frame != g_live_frame) {
        s.binds = 0; s.draws = 0;
        // De-jitter the bind order: adopt a new value only once the game has agreed with itself for
        // LIVE_ORDER_HOLD frames. A buffer whose position genuinely moves still follows, but a
        // conditional pass toggling upstream no longer renumbers half the gallery every frame.
        const int o = g_live_order_ctr++;
        if (o == s.order_raw) { if (s.order_hold < LIVE_ORDER_HOLD) ++s.order_hold; }
        else                  { s.order_raw = o; s.order_hold = 0; }
        if (s.order_hold >= LIVE_ORDER_HOLD) s.order = o;
    }
    s.binds++;
    s.bound_as |= how;
    s.last_frame = g_live_frame; // last, and a release store: freshness is what the lockless readers
                                 // gate on, so publishing it after the fields it vouches for means a
                                 // slot never reads fresh with this frame's bind kind not yet in it
    return idx;
}

// Render targets bound this frame (drives draw attribution: the primary RT owns subsequent draws).
static void live_on_bind(command_list* cmd, uint32_t count, const resource_view* rtvs) {
    device* dev = cmd->get_device();
    g_live_current = -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (rtvs[i].handle == 0) continue;
        resource res = dev->get_resource_from_view(rtvs[i]);
        int idx = live_touch(dev, res.handle, LIVE_RTV);
        if (idx >= 0 && i == 0) g_live_current = idx; // attribute subsequent draws to the primary RT
    }
}

// Compute UAV binds (DX11 CS/OM unordered-access views surface here as push_descriptors updates).
// Registers texture UAVs so the Buffers gallery also lists compute-shader outputs — motion vectors,
// SSAO/SSR/GI, bloom chains, auto-exposure targets — that never appear as a color render target.
// Only texture UAVs are tracked (buffer UAVs aren't image-like); SRV/sampler/CBV updates are
// ignored by the type gate so this stays cheap despite firing on every descriptor push.
static void live_on_push_desc(command_list* cmd, shader_stage, pipeline_layout, uint32_t,
                              const descriptor_table_update& u) {
    if (u.type != descriptor_type::unordered_access_view || u.descriptors == nullptr) return;
    device* dev = cmd->get_device();
    const resource_view* views = static_cast<const resource_view*>(u.descriptors);
    for (uint32_t i = 0; i < u.count; ++i) {
        if (views[i].handle == 0) continue;
        resource res = dev->get_resource_from_view(views[i]);
        if (res.handle == 0) continue;
        // The DX11 event doesn't split buffer vs texture UAVs; drop buffer UAVs (not image-like,
        // would register as a junk 0x0 row) so only 2D compute-output images reach the browser.
        if (dev->get_resource_desc(res).type != resource_type::texture_2d) continue;
        live_touch(dev, res.handle, LIVE_UAV);
    }
}

// Lazily create (and cache) a shader-resource view for a live RT so ImGui can sample it.
// Fails gracefully (returns {0}) for targets the driver won't let us view as an SRV.
//
// Takes g_live_mutex, and that is load-bearing on a multi-threaded recorder (D3D12/BO6). Unlocked,
// this raced eviction two ways: it could call create_resource_view on a handle the game had just
// freed (an access violation inside the driver), and it could write s.srv into a slot eviction had
// already blanked — where the evict loop, which matches on `handle`, could never find it again and
// live_touch's `n = LiveRT{}` would silently zero it on slot reuse, leaking the descriptor.
// A caller that matched a specific handle in an unlocked scan passes it as `expect`: "not blank"
// is not enough, because the game can free that buffer and a worker thread can REUSE the slot for
// a different one between the scan and this call — and a view of the wrong buffer is a perfectly
// valid view, so it fails silently as a Viewfinder/thumbnail showing something else entirely.
static resource_view live_get_srv(device* dev, LiveRT& s, uint64_t expect = 0) {
    std::lock_guard<std::mutex> lock(g_live_mutex);
    // Re-check under the lock: the caller found this slot in an earlier (unlocked) pass, and the
    // game may have freed the buffer in between. A blanked slot has no resource to view.
    if (s.handle == 0 || (expect != 0 && s.handle != expect)) return { 0 };
    if (s.srv.handle) return s.srv;
    if (s.srv_tried)  return { 0 };
    s.srv_tried = true;
    resource_view_desc vd(format_to_default_typed(s.desc.texture.format)); // typeless -> typed, 2D, 1 mip
    dev->create_resource_view({ s.handle }, resource_usage::shader_resource, vd, &s.srv);
    return s.srv;
}

// Park an evicted view for deferred destruction. CALLER MUST HOLD g_live_mutex.
// If the queue is somehow saturated (pathological eviction churn), destroy immediately rather than
// leaking — a stale-descriptor risk for one frame beats unbounded VRAM growth over a session.
static void live_retire_view(device* dev, resource_view v) {
    if (v.handle == 0) return;
    if (g_live_retired.size() >= 256) { dev->destroy_resource_view(v); return; }
    g_live_retired.push_back(RetiredView{ v, g_live_frame });
}

// Destroy retired views once no in-flight overlay draw list can still reference them.
// Called once per present, before the frame counter advances.
static void live_retire_tick(device* dev) {
    std::lock_guard<std::mutex> lock(g_live_mutex);
    if (g_live_retired.empty()) return;
    size_t keep = 0;
    for (size_t i = 0; i < g_live_retired.size(); ++i) {
        const RetiredView& r = g_live_retired[i];
        if (g_live_frame - r.frame >= LIVE_RETIRE_FRAMES) dev->destroy_resource_view(r.view);
        else g_live_retired[keep++] = r;
    }
    g_live_retired.resize(keep);
}

// Is this handle still a registered, recently-bound engine buffer?
// The preview path holds a RAW handle across frames (g_preview_handle) and the game can free that
// buffer at any time, so every use has to be resolved through the registry rather than trusting the
// handle. Uses the display window so the Viewfinder matches what the gallery still lists.
//
// This is the same contract resolve_engine_rt gives the recording path, and it carries the same
// residual race: the buffer can in principle be freed between this check and the copy that follows.
// What it eliminates is the far worse failure it replaced — a handle that stays dead FOREVER and is
// dereferenced on every subsequent frame.
static bool live_handle_is_live(uint64_t handle) {
    if (handle == 0) return false;
    std::lock_guard<std::mutex> lock(g_live_mutex);
    for (auto& s : g_live_rts)
        if (s.handle == handle && g_live_frame - s.last_frame <= LIVE_LIST_FRAMES) return true;
    return false;
}

// Sorted candidate set for an engine-RT fingerprint: every recently-bound buffer matching
// (format, min-width, bind-kind), in first-seen bind order. This is THE definition of both the
// liveness window and the ordering — promote_rt saves the ordinal as an index into this exact
// list and resolve_engine_rt reads it back, so the two can never disagree. (They used to:
// promote counted candidates over a wider window with its own tie-breaks, which could shift the
// ordinal by one and silently re-point the new pass at a neighboring buffer.)
static int engine_rt_candidates(format fmt, uint32_t min_w, uint8_t bind, const LiveRT** cand) {
    // Read WITHOUT g_live_mutex — this runs per pass per frame on the render thread and must not
    // contend with the worker threads registering binds. What makes that safe is the field order
    // below: `handle` and `last_frame` are acquire loads (AtomicField), and they are published in
    // the reverse of the order tested here, so a slot that passes both tests had its plain fields
    // written before either became visible.
    int n = 0;
    for (auto& s : g_live_rts) {
        if (s.handle == 0)                   continue;          // freed slot (retracted FIRST on eviction)
        // Deliberately the strictest liveness window in the addon: a pass must never resolve to a
        // buffer the game has stopped drawing into. The gallery's wider LIVE_LIST_FRAMES window is
        // a display convenience only and must not leak in here.
        if (g_live_frame - s.last_frame > 1) continue;          // not bound this frame
        if ((s.bound_as & bind) == 0)        continue;          // wrong bind kind (RTV vs UAV)
        if (s.desc.texture.format != fmt)    continue;
        if (s.desc.texture.width  <  min_w)  continue;
        cand[n++] = &s; if (n >= 64) break;
    }
    // Insertion sort by de-jittered bind order, ties broken on the stable creation sequence — two
    // buffers can briefly report the same held order while one of them is still settling, and an
    // ambiguous compare here would swap a promoted pass onto its neighbour for a frame.
    for (int i = 1; i < n; ++i) { const LiveRT* x = cand[i]; int j = i - 1;
        while (j >= 0 && (cand[j]->order > x->order ||
                         (cand[j]->order == x->order && cand[j]->seq > x->seq))) { cand[j + 1] = cand[j]; --j; }
        cand[j + 1] = x; }
    return n;
}

// Resolve a pass's SRC_ENGINE_RT fingerprint to a live handle for THIS frame (0 if no match).
static uint64_t resolve_engine_rt(const RecordPass& p) {
    const LiveRT* cand[64];
    const int n = engine_rt_candidates(p.rt_fmt, p.rt_min_w, p.rt_bind, cand);
    if (p.rt_ordinal < 0 || p.rt_ordinal >= n) return 0;
    return cand[p.rt_ordinal]->handle;
}

// Does a promoted custom pass already resolve to this live RT this frame? Used by the Buffers gallery
// to flag a buffer as already-added, so "+ Add as pass" can't silently create a duplicate pass
// pointing at the same underlying buffer.
static RecordPass* find_promoted_pass_for_handle(uint64_t handle) {
    for (int i = 0; i < N_CUSTOM; ++i) {
        RecordPass& c = g_passes[N_FIXED + i];
        if (c.promoted && resolve_engine_rt(c) == handle) return &c;
    }
    return nullptr;
}

static void live_destroy_all(device* dev) {
    std::lock_guard<std::mutex> lock(g_live_mutex);
    g_live_current = -1; // clear() is the one operation that shrinks the vector
    for (auto& s : g_live_rts) if (s.srv.handle) dev->destroy_resource_view(s.srv);
    g_live_rts.clear();
    // Teardown (device loss / "Reset All GPU Resources"): the deferral no longer buys anything --
    // nothing will draw these again, and the device is going away -- so flush rather than retire.
    for (auto& r : g_live_retired) dev->destroy_resource_view(r.view);
    g_live_retired.clear();
    g_live_seq_ctr = 0;
    g_live_current = -1;
    g_preview_handle = 0;  // every handle just became invalid
}

// When the game frees a render target (e.g. on a resolution change), drop its live-registry
// slot + cached SRV so we never sample a dangling view. The slot is BLANKED, not erased: erasing
// shifted every later element, which invalidated g_live_current (the next draw then credited its
// count to whichever buffer slid into that index) and re-sorted the gallery under the user's
// cursor. live_touch reuses blank slots, so the 64-slot budget is unaffected.
static void on_destroy_resource_evict(device* dev, resource res) {
    if (res.handle == 0) return;
    std::lock_guard<std::mutex> lock(g_live_mutex);
    // The Viewfinder holds a bare handle, not a registry index, and preview_capture_tick used to
    // dereference it with no liveness test at all — so freeing a previewed buffer left a handle
    // that was dead FOREVER and hit D3D12 on freed memory every single frame after. Clearing it
    // here is the fix at the source; live_handle_is_live() in the preview tick is the backstop for
    // a buffer that goes away without a destroy_resource event ever reaching us.
    if (g_preview_handle == res.handle) g_preview_handle = 0;
    for (size_t i = 0; i < g_live_rts.size(); ++i)
        if (g_live_rts[i].handle == res.handle) {
            // Retire, don't destroy: this frame's overlay draw list may already name this view.
            live_retire_view(dev, g_live_rts[i].srv);
            // Retract `handle` FIRST and on its own. It is the field every lockless reader tests
            // before it touches last_frame/desc, so blanking it ahead of the rest is what stops a
            // reader mid-eviction from seeing a live handle beside an already-zeroed slot and
            // handing the capture path a buffer the game is in the middle of freeing.
            g_live_rts[i].handle.retract();
            g_live_rts[i] = LiveRT{};
            if (g_live_current == (int)i) g_live_current = -1;
            break;
        }
}

// Resolve a live preview SRV for a pass's current source (for the settings-table thumbnail).
// SRC_NAMED uses the .fx texture binding; SRC_ENGINE_RT maps through the live registry.
static resource_view pass_preview_srv(effect_runtime* rt, RecordPass& p) {
    device* dev = rt->get_device();
    // SRC_FX_LAYER owns its target outright (mpr_fxlayer.inc), so its SRV is just a field read --
    // and routing it through here is what gives FX passes the Viewfinder, scopes, Debug View and
    // pass-table thumbnails for free.
    if (p.source == SRC_FX_LAYER) return p.fx_srv;
    // Same deal for the addon-computed passes (mpr_native.inc): they own their target, so the
    // Viewfinder, scopes, Debug View and thumbnails all come free off this one field read.
    if (src_is_native(p.source)) return p.native_srv;
    if (p.source == SRC_NAMED) {
        resource_view srv = { 0 };
        effect_texture_variable var = rt->find_texture_variable(nullptr, p.tex_name);
        if (var != 0) rt->get_texture_binding(var, &srv, nullptr);
        return srv;
    }
    const uint64_t h = resolve_engine_rt(p); // SRC_ENGINE_RT
    if (h == 0) return { 0 };
    for (auto& s : g_live_rts)
        if (s.handle == h && g_live_frame - s.last_frame <= LIVE_FRESH_FRAMES) return live_get_srv(dev, s, h);
    return { 0 };
}

// One-button auto-profiler: scan the live RT registry and guess this game's engine buffers by
// heuristic — native normals = the 2nd full-res r8g8b8a8 target in bind order (albedo, then
// normals, as in most deferred engines). Applies the guess to the normals_native pass and
// persists it to this game's ini.
// Heuristic, not magic: it gets you a working starting point; refine in the Buffers tab if off.
static std::string auto_profile_scan() {
    uint32_t maxw = 0;
    for (auto& s : g_live_rts)
        if (s.handle && g_live_frame - s.last_frame <= LIVE_FRESH_FRAMES && s.desc.texture.width > maxw)
            maxw = s.desc.texture.width;
    if (maxw == 0) return "No RTs seen yet - open a 3D scene (not a menu) and scan again.";
    const uint32_t fullw = (uint32_t)(maxw * 0.9f); // "full-res" = within 90% of the widest RT

    std::vector<const LiveRT*> g8;
    for (auto& s : g_live_rts) {
        if (s.handle == 0) continue;
        if (g_live_frame - s.last_frame > LIVE_FRESH_FRAMES || s.desc.texture.width < fullw) continue;
        format f = s.desc.texture.format;
        if (f == format::r8g8b8a8_typeless || f == format::r8g8b8a8_unorm) g8.push_back(&s);
    }
    std::sort(g8.begin(), g8.end(), [](const LiveRT* a, const LiveRT* b) { return a->order < b->order; });

    std::string r;
    char buf[160];
    const LiveRT* nrm = g8.size() >= 2 ? g8[1] : (g8.size() == 1 ? g8[0] : nullptr);
    if (nrm) {
        int ord = 0;
        for (const LiveRT* o : g8) if (o->desc.texture.format == nrm->desc.texture.format && o->order < nrm->order) ord++;
        g_passes[3].source = SRC_ENGINE_RT;
        g_passes[3].rt_fmt = nrm->desc.texture.format; g_passes[3].rt_min_w = fullw; g_passes[3].rt_ordinal = ord;
        sprintf_s(buf, "normals_native = o%02d %s (guess #%d of %zu). ", nrm->order,
                  probe_format_name(nrm->desc.texture.format), ord + 1, g8.size());
        r += buf;
    } else r += "normals: no full-res 8-bit G-buffer target found. ";

    if (g_runtime) save_addon_config(g_runtime);
    r += "Saved to this game's ini. Verify/adjust in the list below.";
    return r;
}

// Shared bind/draw hooks driving the live RT registry (the Buffers gallery).
//
// These four are registered ONCE, at addon registration, and never unregistered — so the ONLY
// thing standing between an idle session and their cost is the g_live_enum test each one opens
// with, before any work, any call and any lock. Keep it that way. Registering/unregistering them
// to track g_live_enum is what this replaced, and it was a D3D12 crash: reshade::addon_event_list
// is a bare std::vector<void*> that register push_back()s and unregister erase()s, while
// invoke_addon_event walks it with no synchronization at all — and a D3D12 title records command
// lists (firing these very events) on worker threads while present is toggling the panel.
static void on_scene_bind_rts(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view) {
    if (!g_live_enum) return;
    live_on_bind(cmd, count, rtvs);
}

// Descriptor pushes carry DX11 compute UAV binds; forward them to the live registry so the RT
// Browser also enumerates compute-shader outputs. Gated by the same enumeration flag as the RT
// bind hook, and the UAV type check inside is the first thing done, so idle cost stays negligible.
static void on_scene_push_desc(command_list* cmd, shader_stage stages, pipeline_layout layout,
                               uint32_t param, const descriptor_table_update& update) {
    if (!g_live_enum) return;
    live_on_push_desc(cmd, stages, layout, param, update);
}

// Credit a draw to the primary RT bound at the time. Re-validates the slot: g_live_current is an
// index, and the buffer it named can be freed between the bind and the draw.
// Deliberately does NOT take g_live_mutex — this fires on every draw call in the game, and the
// worst a race can do here is miscount a tooltip statistic. Slots are blanked, never moved, and
// only live_destroy_all shrinks the vector (which takes the lock and happens at device teardown).
static inline void live_credit_draw() {
    if (!g_live_enum || g_live_current < 0 || g_live_current >= (int)g_live_rts.size()) return;
    LiveRT& s = g_live_rts[g_live_current];
    if (s.handle) s.draws++;
}
static bool on_scene_draw(command_list*, uint32_t, uint32_t, uint32_t, uint32_t) {
    live_credit_draw();
    return false; // never suppress the game's draw
}
static bool on_scene_draw_indexed(command_list*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
    live_credit_draw();
    return false;
}

