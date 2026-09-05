// === MultipassRecorder — mpr_theme.inc ===
// UI theme — the semantic color tokens and small shared widgets every panel uses.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.
//
// One color = one meaning, defined once. The addon's visual language is:
//   green = ok / recording healthy / kept    amber = warning / limited / attention
//   red   = error / dropped / clipped white  blue  = crushed black / info / depth
// Every panel (settings, OSD, scopes, RT gallery) draws from these tokens so the
// same state always looks the same, instead of re-typing near-miss literals.

// ---- ImVec4 tokens (ImGui text/widget colors) ----
static constexpr ImVec4 MPR_OK       { 0.35f, 0.95f, 0.40f, 1.0f }; // healthy / active / known
static constexpr ImVec4 MPR_OK_DIM   { 0.55f, 0.85f, 0.58f, 1.0f }; // ok readouts (softer, for numbers)
static constexpr ImVec4 MPR_WARN     { 1.00f, 0.68f, 0.25f, 1.0f }; // needs attention, not fatal
static constexpr ImVec4 MPR_ERR      { 1.00f, 0.35f, 0.35f, 1.0f }; // drops / clip / broken state
static constexpr ImVec4 MPR_INFO     { 0.55f, 0.75f, 1.00f, 1.0f }; // neutral-informative (UAV tag, AE mode)
static constexpr ImVec4 MPR_CRUSH_TXT{ 0.55f, 0.65f, 0.90f, 1.0f }; // crushed-shadows readout text
static constexpr ImVec4 MPR_DIM      { 0.55f, 0.55f, 0.55f, 1.0f }; // de-emphasized values
static constexpr ImVec4 MPR_BTN_STOP { 0.70f, 0.10f, 0.10f, 1.0f }; // Stop Recording button fill
static constexpr ImVec4 MPR_BTN_ON   { 0.20f, 0.55f, 0.25f, 1.0f }; // latched-on toggle button fill
static constexpr ImVec4 MPR_ROW_ADDED{ 0.50f, 0.40f, 0.05f, 0.35f }; // table row whose buffer/technique is already a pass

// RT-gallery role tints (mpr_format_util.inc). FOUR, and deliberately no more: these are read at a
// glance on top of a moving game image, where near-miss greens are simply not separable — a colour
// the eye can't tell apart carries no information but still costs the reader a thing to remember.
// So one tint = one decision about what a buffer is worth to you, not one tint per pixel format
// (the format's own name is right next to it, and format_role() still classifies at full detail).
static constexpr ImVec4 MPR_ROLE_GRADE  { 0.42f, 0.92f, 0.48f, 1.0f }; // gradeable colour: HDR float / 16-bit / 10-bit
static constexpr ImVec4 MPR_ROLE_LIMITED{ 0.98f, 0.74f, 0.28f, 1.0f }; // 8-bit colour: usable, bands if pushed
static constexpr ImVec4 MPR_ROLE_DEPTH  { 0.45f, 0.70f, 1.00f, 1.0f }; // depth/stencil: distance, not a picture
static constexpr ImVec4 MPR_ROLE_DATA   { 0.72f, 0.72f, 0.78f, 1.0f }; // 1-/2-channel data: not a viewable image

// ---- Chrome tokens (neutral grey backdrop for the addon's own windows) ----
// These are STRUCTURAL, not semantic -- never reused where an MPR_OK/WARN/ERR/etc. token above
// would be the correct choice. A minimal, sharp-cornered "camera/monitor OS" backdrop (Blackmagic/
// Atomos/SmallHD/Leica register) for MprThemeScope to push, distinct from ReShade's default rounded
// panel look. See MprThemeScope below for where these are applied and the toggle that can disable
// all of them at once.
static constexpr ImVec4 MPR_CHROME_BG          { 0.102f, 0.102f, 0.110f, 1.0f }; // WindowBg
static constexpr ImVec4 MPR_CHROME_BG_ELEV     { 0.125f, 0.125f, 0.137f, 1.0f }; // ChildBg, PopupBg
static constexpr ImVec4 MPR_CHROME_BG_SUNKEN   { 0.078f, 0.078f, 0.086f, 1.0f }; // ScrollbarBg, TitleBg*, MenuBarBg
static constexpr ImVec4 MPR_CHROME_FRAME       { 0.165f, 0.165f, 0.180f, 1.0f }; // FrameBg, ScrollbarGrab
static constexpr ImVec4 MPR_CHROME_FRAME_HOV   { 0.212f, 0.212f, 0.235f, 1.0f }; // FrameBgHovered, ScrollbarGrabHovered, SliderGrab, SeparatorHovered, ResizeGripHovered
static constexpr ImVec4 MPR_CHROME_FRAME_ACT   { 0.251f, 0.251f, 0.278f, 1.0f }; // FrameBgActive, ScrollbarGrabActive, SliderGrabActive, SeparatorActive, ResizeGripActive
static constexpr ImVec4 MPR_CHROME_BTN         { 0.188f, 0.188f, 0.208f, 1.0f }; // Button
static constexpr ImVec4 MPR_CHROME_BTN_HOV     { 0.243f, 0.243f, 0.271f, 1.0f }; // ButtonHovered
static constexpr ImVec4 MPR_CHROME_BTN_ACT     { 0.294f, 0.294f, 0.329f, 1.0f }; // ButtonActive
static constexpr ImVec4 MPR_CHROME_HEADER      { 0.196f, 0.196f, 0.220f, 1.0f }; // Header, TableHeaderBg
static constexpr ImVec4 MPR_CHROME_HEADER_HOV  { 0.251f, 0.251f, 0.282f, 1.0f }; // HeaderHovered
static constexpr ImVec4 MPR_CHROME_HEADER_ACT  { 0.306f, 0.306f, 0.345f, 1.0f }; // HeaderActive
static constexpr ImVec4 MPR_CHROME_TAB         { 0.133f, 0.133f, 0.149f, 1.0f }; // Tab
static constexpr ImVec4 MPR_CHROME_TAB_HOV     { 0.235f, 0.235f, 0.263f, 1.0f }; // TabHovered
static constexpr ImVec4 MPR_CHROME_TAB_SEL     { 0.204f, 0.204f, 0.227f, 1.0f }; // TabSelected
static constexpr ImVec4 MPR_CHROME_TAB_DIM     { 0.110f, 0.110f, 0.122f, 1.0f }; // TabDimmed
static constexpr ImVec4 MPR_CHROME_TAB_DIM_SEL { 0.173f, 0.173f, 0.192f, 1.0f }; // TabDimmedSelected
static constexpr ImVec4 MPR_CHROME_ACCENT_LINE { 0.588f, 0.596f, 0.620f, 1.0f }; // TabSelectedOverline, TabDimmedSelectedOverline
static constexpr ImVec4 MPR_CHROME_TEXT        { 0.839f, 0.843f, 0.859f, 1.0f }; // Text, CheckMark
// Same hue family as MPR_OUTLINE (70,74,86) below, so window chrome and the scope/waveform
// instrument frame read as one system rather than two different greys.
static constexpr ImVec4 MPR_CHROME_BORDER      { 0.259f, 0.259f, 0.290f, 1.0f }; // Border, Separator, TableBorderStrong
static constexpr ImVec4 MPR_CHROME_BORDER_DIM  { 0.180f, 0.180f, 0.200f, 1.0f }; // TableBorderLight
static constexpr ImVec4 MPR_CHROME_ROW_ALT     { 0.133f, 0.133f, 0.149f, 0.35f}; // TableRowBgAlt

// ---- ImU32 tokens (draw-list instruments: scopes, chips, guides) ----
// Bases carry full alpha; use mpr_alpha() at the call site where a zone/line needs transparency.
static constexpr ImU32 MPR_SCOPE_BG = IM_COL32( 10,  10,  13, 255); // instrument background (scope/waveform)
static constexpr ImU32 MPR_CHIP     = IM_COL32(  6,   6,  10, 255); // dark label-backing chip
static constexpr ImU32 MPR_OUTLINE  = IM_COL32( 70,  74,  86, 255); // instrument frame line
static constexpr ImU32 MPR_WHITE    = IM_COL32(255, 255, 255, 255); // reference lines (diffuse white)
static constexpr ImU32 MPR_TEXT_HI  = IM_COL32(225, 227, 232, 255); // bright on-instrument text/brackets
static constexpr ImU32 MPR_TEXT_AXIS= IM_COL32(185, 185, 195, 235); // axis/scale text
static constexpr ImU32 MPR_HIST     = IM_COL32(205, 205, 212, 255); // histogram bars
static constexpr ImU32 MPR_CRUSH    = IM_COL32(130, 160, 255, 255); // crushed-black zone/lines/labels
static constexpr ImU32 MPR_CLIP     = IM_COL32(255, 130, 130, 255); // clipped-white zone/lines/labels
static constexpr ImU32 MPR_KEPT     = IM_COL32(145, 220, 155, 255); // kept-range label/swatch
static constexpr ImU32 MPR_MEDIAN   = IM_COL32(220, 220, 120, 220); // median tick
static constexpr ImU32 MPR_GRID     = IM_COL32(255, 255, 255,  16); // stop-interval graticule
// Transfer-curve overlay palette (scope): active curve amber, SDR baseline teal, PNG linear green.
static constexpr ImU32 MPR_CURVE_ACTIVE = IM_COL32(255, 200,  95, 235);
static constexpr ImU32 MPR_CURVE_SDR    = IM_COL32(120, 205, 220, 150);
static constexpr ImU32 MPR_CURVE_PNG    = IM_COL32(160, 225, 160, 230);
// Dark zone fills for the condensed OSD meter (flat, no hatching at 40px tall).
static constexpr ImU32 MPR_ZONE_CRUSH = IM_COL32( 38,  58, 110,  90);
static constexpr ImU32 MPR_ZONE_KEPT  = IM_COL32( 36,  92,  52,  90);
static constexpr ImU32 MPR_ZONE_CLIP  = IM_COL32(120,  44,  44,  90);
// Waveform trace/legend channels.
static constexpr ImU32 MPR_CH_R = IM_COL32(232,  96,  96, 255);
static constexpr ImU32 MPR_CH_G = IM_COL32(110, 210, 110, 255);
static constexpr ImU32 MPR_CH_B = IM_COL32(120, 150, 235, 255);

// Replace a token's alpha (keeps the hue single-sourced while zones/lines fade independently).
static constexpr ImU32 mpr_alpha(ImU32 col, int a) {
    return (col & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
}
// Scale a token's existing alpha. Draw-list pixels ignore ImGuiStyleVar_Alpha (only the widget
// colors go through GetColorU32), so anything drawn by hand inside a dimmed row has to fade itself.
static constexpr ImU32 mpr_fade(ImU32 col, float f) {
    return mpr_alpha(col, (int)((float)((col >> IM_COL32_A_SHIFT) & 0xFFu) * f));
}
static inline ImU32 mpr_u32(const ImVec4& c) {
    return IM_COL32((int)(c.x * 255.0f), (int)(c.y * 255.0f), (int)(c.z * 255.0f), (int)(c.w * 255.0f));
}
// ImVec4 analogue of mpr_alpha -- replace a token's alpha without re-literaling its hue.
static constexpr ImVec4 mpr_va(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// Pushes the whole grey "camera UI" chrome theme (see MPR_CHROME_* tokens above); the destructor
// pops exactly what the constructor pushed. Non-copyable so a scope can't be duplicated into an
// unpaired pop -- same reasoning as this codebase's mutex RAII: a push/pop bug class removed by
// construction rather than by discipline.
//
// Gated on g_ui_theme_camera (mpr_record_pass.h, Setup tab checkbox): when the user has switched
// back to ReShade's stock look, the constructor pushes nothing and every window using this guard
// falls straight through to whatever style was already active -- no special-casing needed at any
// of the call sites, which construct the guard unconditionally either way.
//
// `hud = true` is for the borderless on-screen tags (Debug View / REC OSD / shot toast): ReShade's
// own init_gui() sets WindowBorderSize = 0 (runtime_gui.cpp) precisely so those read as a
// translucent corner readout, not a window -- pushing this theme's WindowBorderSize = 1.0f
// unconditionally would draw a visible box around them. hud re-overrides it back to 0 on top.
struct MprThemeScope {
    explicit MprThemeScope(bool hud = false) {
        if (!g_ui_theme_camera) return;
        ImGui::PushStyleColor(ImGuiCol_Text, MPR_CHROME_TEXT);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, MPR_DIM);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, MPR_CHROME_BG);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, MPR_CHROME_BG_ELEV);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, MPR_CHROME_BG_ELEV);
        ImGui::PushStyleColor(ImGuiCol_Border, MPR_CHROME_BORDER);
        ImGui::PushStyleColor(ImGuiCol_BorderShadow, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, MPR_CHROME_FRAME);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, MPR_CHROME_FRAME_HOV);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, MPR_CHROME_FRAME_ACT);
        ImGui::PushStyleColor(ImGuiCol_TitleBg, MPR_CHROME_BG_SUNKEN);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, MPR_CHROME_BG_SUNKEN);
        ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, MPR_CHROME_BG_SUNKEN);
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg, MPR_CHROME_BG_SUNKEN);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, MPR_CHROME_BG_SUNKEN);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, MPR_CHROME_FRAME);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, MPR_CHROME_FRAME_HOV);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, MPR_CHROME_FRAME_ACT);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, MPR_CHROME_TEXT);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, MPR_CHROME_FRAME_HOV);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, MPR_CHROME_FRAME_ACT);
        ImGui::PushStyleColor(ImGuiCol_Button, MPR_CHROME_BTN);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, MPR_CHROME_BTN_HOV);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, MPR_CHROME_BTN_ACT);
        ImGui::PushStyleColor(ImGuiCol_Header, MPR_CHROME_HEADER);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MPR_CHROME_HEADER_HOV);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, MPR_CHROME_HEADER_ACT);
        ImGui::PushStyleColor(ImGuiCol_Separator, MPR_CHROME_BORDER);
        ImGui::PushStyleColor(ImGuiCol_SeparatorHovered, MPR_CHROME_FRAME_HOV);
        ImGui::PushStyleColor(ImGuiCol_SeparatorActive, MPR_CHROME_FRAME_ACT);
        ImGui::PushStyleColor(ImGuiCol_ResizeGrip, MPR_CHROME_FRAME);
        ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, MPR_CHROME_FRAME_HOV);
        ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, MPR_CHROME_FRAME_ACT);
        ImGui::PushStyleColor(ImGuiCol_Tab, MPR_CHROME_TAB);
        ImGui::PushStyleColor(ImGuiCol_TabHovered, MPR_CHROME_TAB_HOV);
        ImGui::PushStyleColor(ImGuiCol_TabSelected, MPR_CHROME_TAB_SEL);
        ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, MPR_CHROME_ACCENT_LINE);
        ImGui::PushStyleColor(ImGuiCol_TabDimmed, MPR_CHROME_TAB_DIM);
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, MPR_CHROME_TAB_DIM_SEL);
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, MPR_CHROME_ACCENT_LINE);
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, MPR_CHROME_HEADER);
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, MPR_CHROME_BORDER);
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, MPR_CHROME_BORDER_DIM);
        ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, MPR_CHROME_ROW_ALT);
        m_colors = 45;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 1.0f);
        m_vars = 12;
        if (hud) { ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); m_vars = 13; }
    }
    ~MprThemeScope() {
        if (m_colors) ImGui::PopStyleColor(m_colors);
        if (m_vars) ImGui::PopStyleVar(m_vars);
    }
    MprThemeScope(const MprThemeScope&) = delete;
    MprThemeScope& operator=(const MprThemeScope&) = delete;
private:
    int m_colors = 0;
    int m_vars = 0;
};

// ---- Shared micro-widgets ----

// Dimmed "(?)" whose explanation shows only on hover — keeps reference text out of the panel.
// Call ImGui::SameLine() first to attach it to the preceding widget.
static void help_marker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(420.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Vertical breathing room between panel sections -- wider than a bare ImGui::Spacing() so a
// fully-expanded panel reads as distinct groups rather than a wall of controls.
static void section_gap() { ImGui::Dummy(ImVec2(0.0f, 8.0f)); }

// Small filled-triangle "!" drawn with the draw list (never a missing font glyph) to flag the
// line that follows it -- normal/OK lines get no icon. Leaves the cursor SameLine'd for the text.
static void mpr_icon_warn() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float sz = fs * 0.55f, top = p.y + 1.0f, bot = p.y + fs - 2.0f, cx = p.x + sz * 0.5f;
    dl->AddTriangleFilled(ImVec2(cx, top), ImVec2(p.x, bot), ImVec2(p.x + sz, bot), mpr_u32(MPR_WARN));
    dl->AddLine(ImVec2(cx, top + sz * 0.32f), ImVec2(cx, bot - sz * 0.32f), IM_COL32(30, 18, 6, 255), 1.4f);
    dl->AddCircleFilled(ImVec2(cx, bot - sz * 0.12f), 1.1f, IM_COL32(30, 18, 6, 255));
    ImGui::Dummy(ImVec2(sz + 5, fs));
    ImGui::SameLine(0, 4);
}

// Small sun glyph marking a row as exposure -- the same shorthand camera exposure-compensation
// menus use, drawn with the draw list since there's no icon font here. SameLine's for the row.
static void mpr_icon_sun() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = ImGui::GetFontSize() * 0.3f, cx = p.x + r + 1.0f, cy = p.y + ImGui::GetFontSize() * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(cx, cy), r * 0.5f, IM_COL32(255, 205, 110, 235));
    for (int k = 0; k < 8; ++k) {
        float a = k * (3.14159265f * 0.25f);
        ImVec2 d(cosf(a), sinf(a));
        dl->AddLine(ImVec2(cx + d.x * r * 0.85f, cy + d.y * r * 0.85f),
                    ImVec2(cx + d.x * r * 1.45f, cy + d.y * r * 1.45f), IM_COL32(255, 205, 110, 200), 1.3f);
    }
    ImGui::Dummy(ImVec2(r * 3.1f, ImGui::GetFontSize()));
    ImGui::SameLine(0, 5);
}

// Small padlock glyph -- "this value is frozen for the take", camera-style rec-lock. SameLine's.
static void mpr_icon_lock() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float lw = ImGui::GetFontSize() * 0.6f, lh = lw * 0.8f, top = p.y + 2.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PathArcTo(ImVec2(p.x + lw * 0.5f, top + lh * 0.4f), lw * 0.32f, 3.14159265f, 2.0f * 3.14159265f, 10);
    dl->PathStroke(mpr_alpha(IM_COL32(205, 205, 210, 255), 230), 0, 1.3f);
    dl->AddRect(ImVec2(p.x, top + lh * 0.32f), ImVec2(p.x + lw, top + lh), mpr_alpha(IM_COL32(205, 205, 210, 255), 230), 1.0f, 0, 1.3f);
    ImGui::Dummy(ImVec2(lw + 4, ImGui::GetFontSize()));
    ImGui::SameLine(0, 4);
}

// Anchored on-instrument label with an optional dark backing chip and a leading color swatch,
// clamped to [x0, x1]. anchor: 0 = x is the left edge, 1 = centered on x, 2 = x is the right edge.
// The swatch bonds a label to a zone/line color so meaning doesn't rely on memorizing a hue.
// `fade` scales every colour it draws (chip, swatch, text) — that is how a chip inside a row dimmed
// with ImGuiStyleVar_Alpha follows the row; leave it at 1.0 for on-instrument labels.
static void mpr_tag(ImDrawList* dl, float x, float y, float x0, float x1,
                    ImU32 col, int anchor, bool chip, const char* s, ImU32 swatch = 0,
                    float fade = 1.0f) {
    const float fs = ImGui::GetFontSize();
    ImVec2 ts = ImGui::CalcTextSize(s);
    const float sw = swatch ? fs * 0.42f : 0.0f, gap = swatch ? 4.0f : 0.0f;
    const float totalW = ts.x + sw + gap;
    float tx = anchor == 1 ? x - totalW * 0.5f : (anchor == 2 ? x - totalW : x);
    if (tx < x0 + 2) tx = x0 + 2;
    if (tx + totalW > x1 - 2) tx = x1 - 2 - totalW;
    if (chip) dl->AddRectFilled(ImVec2(tx - 3, y - 1), ImVec2(tx + totalW + 3, y + ts.y + 1), mpr_fade(mpr_alpha(MPR_CHIP, 210), fade), 3.0f);
    if (swatch) dl->AddRectFilled(ImVec2(tx, y + ts.y * 0.5f - sw * 0.5f), ImVec2(tx + sw, y + ts.y * 0.5f + sw * 0.5f), mpr_fade(swatch, fade), 1.0f);
    dl->AddText(ImVec2(tx + sw + gap, y), mpr_fade(col, fade), s);
}

// The same chip, laid out in the widget stream instead of anchored on an instrument: this is what
// turns a stack of label lines into ONE line of chips. It reserves a real item rect (id 0, exactly
// like ImGui::Text), so ImGui::IsItemHovered()/SetTooltip() attach to the chip and the detail that
// used to be a separate text line stays reachable on hover. The current style alpha is folded in by
// hand, since mpr_tag draws straight to the draw list.
// same_line = continue the chip line (do NOT call ImGui::SameLine yourself); it wraps to the next
// line rather than clipping the chip off the edge when the column is too narrow for the whole run.
static void mpr_chip(ImU32 col, const char* s, bool same_line = false, ImU32 swatch = 0) {
    const float fs = ImGui::GetFontSize();
    const ImVec2 ts = ImGui::CalcTextSize(s);
    const float sw = swatch ? fs * 0.42f : 0.0f, gap = swatch ? 4.0f : 0.0f;
    const float w = ts.x + sw + gap + 8.0f;   // mpr_tag pads the chip 3px per side (+2px of slack)
    if (same_line) {
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    mpr_tag(ImGui::GetWindowDrawList(), p.x + 3.0f, p.y + 1.0f, p.x, p.x + w,
            col, 0, true, s, swatch, ImGui::GetStyle().Alpha);
    ImGui::Dummy(ImVec2(w, ts.y + 3.0f));
}

// ---- Control glyphs ----
// There is no icon font in a ReShade overlay, so every icon is a handful of ImDrawList calls —
// same conventions as mpr_icon_warn / mpr_icon_sun / mpr_icon_lock above: sized off
// ImGui::GetFontSize() so it tracks the host's UI scale, drawn on the window draw list, colored
// from the tokens only. What's different here is WHERE they go: these have to sit INSIDE a
// button, which ImGui has no label path for. The draw list is append-only, so a glyph drawn
// immediately after the button lands on top of its frame — hence the primitive below takes an
// explicit center + radius, and the two wrappers place it either in a button's reserved label
// slot (mpr_button_glyph) or in the middle of a label-less one (mpr_glyph_button).
enum MprGlyph {
    MPR_GLY_RECORD,   // disc inside a thin bezel ring — "roll"
    MPR_GLY_STOP,     // filled square                — "cut"
    MPR_GLY_SHUTTER,  // iris ring + blades           — "take a still"
    MPR_GLY_CHEV_L,   // cycle back
    MPR_GLY_CHEV_R,   // cycle forward / collapsed row
    MPR_GLY_CHEV_D,   // expanded row
    MPR_GLY_CROSS,    // unbind / clear
    MPR_GLY_EYE,      // vesica eye + pupil      — "preview / look at this"
    MPR_GLY_RETICLE,  // AF-style corner brackets — "inspect / debug view"
};

// Record-disc red. Deliberately its OWN token rather than MPR_ERR: on the capture control red
// means "this is the record button" (the camera convention every shooter already reads), not
// "something failed" — reusing MPR_ERR would break one-color-one-meaning from both ends.
static constexpr ImU32 MPR_REC_DISC  = IM_COL32(235,  76,  76, 255);
// Neutral ink for chrome glyphs (chevrons, unbind) — the weight of MPR_TEXT_AXIS, on widgets.
static constexpr ImU32 MPR_GLYPH_INK = IM_COL32(200, 202, 210, 255);

static void mpr_glyph(ImDrawList* dl, MprGlyph g, ImVec2 c, float r, ImU32 col) {
    const float PI = 3.14159265f;
    switch (g) {
    case MPR_GLY_RECORD:
        dl->AddCircleFilled(c, r * 0.66f, col, 16);
        dl->AddCircle(c, r, mpr_alpha(col, 110), 20, 1.2f);  // faint bezel — reads as a button, not a bullet
        break;
    case MPR_GLY_STOP: {
        const float s = r * 0.70f;
        dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, 1.5f);
        break;
    }
    case MPR_GLY_SHUTTER: {
        // Iris: housing ring plus THREE blades, not the photographic six. At a 13px font the whole
        // glyph is ~11px across and six blades smudge into a filled circle.
        dl->AddCircle(c, r, col, 20, 1.4f);
        for (int k = 0; k < 3; ++k) {
            const float a = k * (2.0f * PI / 3.0f) - PI * 0.5f, b = a + 2.0f * PI / 3.0f;
            dl->AddLine(ImVec2(c.x + cosf(a) * r * 0.94f, c.y + sinf(a) * r * 0.94f),
                        ImVec2(c.x + cosf(b) * r * 0.34f, c.y + sinf(b) * r * 0.34f),
                        mpr_alpha(col, 200), 1.2f);
        }
        break;
    }
    case MPR_GLY_CHEV_L: case MPR_GLY_CHEV_R: case MPR_GLY_CHEV_D: {
        const float reach = r * 0.80f, depth = r * 0.52f;
        ImVec2 pts[3];
        if (g == MPR_GLY_CHEV_D) {
            pts[0] = ImVec2(c.x - reach, c.y - depth * 0.60f);
            pts[1] = ImVec2(c.x,         c.y + depth * 0.70f);
            pts[2] = ImVec2(c.x + reach, c.y - depth * 0.60f);
        } else {
            const float d = (g == MPR_GLY_CHEV_L) ? -1.0f : 1.0f;
            pts[0] = ImVec2(c.x - d * depth * 0.60f, c.y - reach);
            pts[1] = ImVec2(c.x + d * depth * 0.70f, c.y);
            pts[2] = ImVec2(c.x - d * depth * 0.60f, c.y + reach);
        }
        const float th = r * 0.30f;
        dl->AddPolyline(pts, 3, col, 0, th < 1.4f ? 1.4f : th);  // open polyline picks up ImGui's line AA
        break;
    }
    case MPR_GLY_CROSS: {
        const float s = r * 0.60f, th0 = r * 0.28f, th = th0 < 1.4f ? 1.4f : th0;
        dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, th);
        dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), col, th);
        break;
    }
    case MPR_GLY_EYE: {
        // Two hand-placed 5-point lids (a sine dip, not a bezier -- same primitive vocabulary as
        // the chevrons above) plus a filled pupil.
        const float PI2 = 3.14159265f;
        ImVec2 upper[5], lower[5];
        for (int i = 0; i < 5; ++i) {
            const float t = i / 4.0f;
            const float x = c.x - r + t * (2.0f * r);
            const float bow = r * 0.55f * sinf(t * PI2);
            upper[i] = ImVec2(x, c.y - bow);
            lower[i] = ImVec2(x, c.y + bow);
        }
        const float th = r * 0.24f < 1.2f ? 1.2f : r * 0.24f;
        dl->AddPolyline(upper, 5, col, 0, th);
        dl->AddPolyline(lower, 5, col, 0, th);
        dl->AddCircleFilled(c, r * 0.22f, col);
        break;
    }
    case MPR_GLY_RETICLE: {
        // Four AF-style corner brackets around a square -- the focus-assist reticle already
        // familiar from camera UIs, distinct from both the eye and the shutter iris.
        const float s = r * 0.75f, arm = s * 0.55f, th = r * 0.28f < 1.4f ? 1.4f : r * 0.28f;
        for (int k = 0; k < 4; ++k) {
            const float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? 1.0f : -1.0f;
            const ImVec2 corner(c.x + sx * s, c.y + sy * s);
            dl->AddLine(corner, ImVec2(corner.x - sx * arm, corner.y), col, th);
            dl->AddLine(corner, ImVec2(corner.x, corner.y - sy * arm), col, th);
        }
        break;
    }
    }
}

// A glyph-bearing label reserves its icon slot with THIS run of leading spaces, so icon + text
// stay one centered block however wide the widget is (and so a widget's ImGui id stops changing
// when its icon does — the old "v"/">" row prefixes rebuilt the id on every toggle).
#define MPR_GLYPH_PAD "    "

// Overlay a glyph onto the button/selectable submitted IMMEDIATELY before this call, centered in
// the MPR_GLYPH_PAD slot at the head of its label. The pad width is MEASURED, never assumed —
// every ReShade host loads its own font — and the glyph radius is clamped to it so a proportional
// font can't let the icon run into the text. Pass `text_x` when the label origin is known and the
// item rect is wider than the label (a SpanAllColumns Selectable starts at the column edge).
// NOTE: draw-list output does NOT inherit ImGui's BeginDisabled alpha, so a disabled control has
// to hand in an already-faded color.
static void mpr_button_glyph(MprGlyph g, const char* label, ImU32 col, float text_x = -1.0f) {
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float pad = ImGui::CalcTextSize(MPR_GLYPH_PAD).x;
    float tx = text_x;
    if (tx < 0.0f) { // derive it: ImGui centers a button label in the frame (ButtonTextAlign)
        const float room = (mx.x - mn.x) - st.FramePadding.x * 2.0f - ImGui::CalcTextSize(label).x;
        tx = mn.x + st.FramePadding.x + (room > 0.0f ? room * st.ButtonTextAlign.x : 0.0f);
    }
    const float fr = ImGui::GetFontSize() * 0.42f, pr = pad * 0.38f;
    mpr_glyph(ImGui::GetWindowDrawList(), g, ImVec2(tx + pad * 0.5f, (mn.y + mx.y) * 0.5f),
              fr < pr ? fr : pr, col);
}

// A compact button whose entire label IS a glyph (cycle arrows, unbind). `id` must be
// "##"-prefixed so the button carries no visible text; the glyph is centered on its frame.
static bool mpr_glyph_button(const char* id, MprGlyph g, ImU32 col) {
    const float fs = ImGui::GetFontSize(), h = fs + 2.0f;
    const bool hit = ImGui::Button(id, ImVec2(h + 5.0f, h));
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    mpr_glyph(ImGui::GetWindowDrawList(), g, ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f),
              fs * 0.36f, col);
    return hit;
}
