// ---------------------------------------------------------------------------
// Hotkeys — one bindable action per table row.
//
// Every in-game action the addon exposes lives in HOTKEY_DEFS below, and the
// config loop, the Setup-tab UI, conflict detection and capture mode all just
// iterate that table. Adding an action is three edits and nothing else:
//   1. a HK_* entry in HotkeyId (append only — the ini key is what persists,
//      but the UI order follows the enum),
//   2. its row in HOTKEY_DEFS (ini key, label, help, default binding),
//   3. a case in hotkey_tick's dispatch (mpr_control.inc).
//
// Only HK_RECORD ships with a default binding. Everything else starts UNBOUND
// on purpose: this addon is injected into someone else's key map, and any key
// we pick for you is a key the game may already want. You bind what you use.
//
// A binding is a key plus an exact modifier state, packed into one int for the
// ini (see hk_pack). Modifiers must match exactly when the key fires — same
// semantics as ReShade's own `force_shortcut_modifiers` — so a binding on
// Ctrl+F9 does not also trigger a plain-F9 action, and vice versa.
// ---------------------------------------------------------------------------

enum HotkeyId {
    HK_RECORD = 0,      // start/stop a take
    HK_DEBUG_VIEW,      // project the previewed pass over the game
    HK_PASS_PREV,       // step the previewed/Debug-View pass backwards
    HK_PASS_NEXT,       // ...and forwards
    HK_OVERLAYS,        // hide/show the addon's own on-screen furniture
    HK_STILL,           // take a still of the screen + every enabled pass
    HK_COUNT
};

struct Hotkey {
    int  vk    = 0;     // virtual-key code; 0 = unbound
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;
};

struct HotkeyDef {
    const char* ini_key;   // key under [MultipassRecorder] in the ReShade ini
    const char* label;     // Setup-tab row label
    const char* help;      // (?) hover
    Hotkey      def;       // shipped default ({} = unbound)
};

static const HotkeyDef HOTKEY_DEFS[HK_COUNT] = {
    { "hk_record", "Record start/stop",
      "Starts or stops a take on every enabled pass at once. Works with the ReShade menu "
      "closed. Default: F9.",
      { VK_F9 } },
    { "hk_dbgview", "Debug View on/off",
      "Projects the previewed pass full-screen over the game, menu open or not - the renderer-"
      "style buffer view. If no pass is selected yet, the first one is picked for you.",
      {} },
    { "hk_passprev", "Debug View: previous pass",
      "Steps the Viewfinder / Debug View back one pass, so you can flip through what each pass "
      "is actually seeing without opening the menu.",
      {} },
    { "hk_passnext", "Debug View: next pass",
      "Steps the Viewfinder / Debug View forward one pass.",
      {} },
    { "hk_overlays", "Hide/show on-screen overlays",
      "Hides the addon's own screen furniture - the recording OSD, the exposure meter and the "
      "composition guides - for a clean look at the shot. Session-only: overlays come back on "
      "the next game launch. Debug View has its own key and is not affected.",
      {} },
    // The one action besides Record that ships BOUND, and deliberately so: a screenshot key that
    // does nothing until you configure it isn't a screenshot key. F10 sits next to Record's F9 and
    // is rarely claimed by games. Rebind or unbind it like any other row if your game wants it.
    { "hk_still", "Take still",
      "Writes a still of the game view (after your ReShade effects, without any menu or overlay in "
      "the pixels) plus one of every enabled pass - all from the SAME game frame, so the depth and "
      "AO stills line up with the beauty. HDR buffers are written as scene-linear EXR with no "
      "exposure baked in, with a viewable PNG beside them. Settings live in the Stills tab. "
      "Default: F10.",
      { VK_F10 } },
};

static Hotkey g_hotkeys[HK_COUNT];
// Setup tab is waiting for the next key press to (re)bind this action; -1 = not capturing.
// Session-only — the bindings themselves persist.
static int g_hotkey_capture = -1;
// Frames left during which hotkeys are suppressed because a UI widget owns the keyboard (typing a
// path into a text field must not also toggle recording). Refreshed by the overlay draw, counted
// down in hotkey_tick. TTL rather than a bool: the UI callbacks and reshade_present run in a fixed
// but version-dependent order within a frame, and only the addon's own windows are visible to us.
static int g_hotkey_block_ttl = 0;

// Seed every binding from the table. Called from AddonInit so F9 records even on a host too old
// for the config API (v13), and per-action from the UI's Reset button.
static void hotkeys_apply_defaults() {
    for (int i = 0; i < HK_COUNT; ++i) g_hotkeys[i] = HOTKEY_DEFS[i].def;
}

// Bindable range: 0x08 (Backspace) up, so mouse buttons (VK 0x01-0x06) can never bind themselves
// from a click on the bind button itself.
static bool hk_vk_valid(int vk) { return vk >= 0x08 && vk <= 0xFE; }

// Modifier keys are captured as *state*, never as the bound key itself.
static bool hk_is_modifier(int vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           (vk >= VK_LSHIFT && vk <= VK_RMENU) ||   // 0xA0-0xA5: L/R shift, ctrl, alt
           vk == VK_LWIN || vk == VK_RWIN;
}

static int hk_pack(const Hotkey& h) {
    return (h.vk & 0xFF) | (h.ctrl ? 0x100 : 0) | (h.shift ? 0x200 : 0) | (h.alt ? 0x400 : 0);
}
static Hotkey hk_unpack(int packed) {
    Hotkey h;
    h.vk    = packed & 0xFF;
    if (!hk_vk_valid(h.vk)) h.vk = 0;             // stale/garbage binding -> unbound, never crash-bound
    h.ctrl  = (packed & 0x100) != 0;
    h.shift = (packed & 0x200) != 0;
    h.alt   = (packed & 0x400) != 0;
    return h;
}
static bool hk_same(const Hotkey& a, const Hotkey& b) {
    return a.vk == b.vk && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt;
}

// Human-readable chord for UI labels: "F9", "Ctrl+Shift+P", or "unbound".
// Static buffer (UI thread only, consume before the next call) — and it must COPY out of
// vk_name's own static buffer before formatting into it.
static const char* hotkey_label(const Hotkey& h) {
    static char buf[64];
    if (h.vk == 0) return "unbound";
    char k[32];
    strncpy_s(k, vk_name(h.vk), _TRUNCATE);
    _snprintf_s(buf, _TRUNCATE, "%s%s%s%s",
                h.ctrl ? "Ctrl+" : "", h.shift ? "Shift+" : "", h.alt ? "Alt+" : "", k);
    return buf;
}
static const char* hotkey_label(HotkeyId id) { return hotkey_label(g_hotkeys[id]); }

// Did this action's chord fire this frame? Modifier state must match exactly (see header).
static bool hotkey_pressed(effect_runtime* rt, HotkeyId id) {
    const Hotkey& h = g_hotkeys[id];
    if (h.vk == 0 || !rt->is_key_pressed((uint32_t)h.vk)) return false;
    return rt->is_key_down(VK_CONTROL) == h.ctrl &&
           rt->is_key_down(VK_SHIFT)   == h.shift &&
           rt->is_key_down(VK_MENU)    == h.alt;
}

// Another action bound to the same chord, or -1. Two actions on one chord both fire (dispatch
// doesn't stop at the first match), so this is reported in the UI rather than silently resolved.
static int hotkey_conflict(int id) {
    if (g_hotkeys[id].vk == 0) return -1;
    for (int i = 0; i < HK_COUNT; ++i)
        if (i != id && hk_same(g_hotkeys[i], g_hotkeys[id])) return i;
    return -1;
}
