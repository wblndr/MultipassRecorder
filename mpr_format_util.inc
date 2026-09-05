// === MultipassRecorder — mpr_format_util.inc ===
// Pixel-format helpers — human names, bytes-per-pixel, role classification for the Buffers gallery.
//
// Part of the main.cpp unity build: this fragment is #included by main.cpp in a
// fixed order and is NOT a standalone translation unit — it has no include guard
// and relies on the headers/usings and the fragments included before it. See the
// "Source layout" section in CLAUDE.md.

// Names for the formats we're most likely to meet in the RT Browser; anything
// else is identified by its numeric value (reshade's format enum mirrors DXGI).
static const char* probe_format_name(format f) {
    switch (f) {
        case format::r8g8b8a8_unorm:      return "r8g8b8a8_unorm";
        case format::r8g8b8a8_unorm_srgb: return "r8g8b8a8_srgb";
        case format::r8g8b8a8_typeless:   return "r8g8b8a8_typeless";
        case format::b8g8r8a8_unorm:      return "b8g8r8a8_unorm";
        case format::b8g8r8a8_unorm_srgb: return "b8g8r8a8_srgb";
        case format::r10g10b10a2_unorm:   return "r10g10b10a2_unorm";
        case format::r11g11b10_float:     return "r11g11b10_float";
        case format::r9g9b9e5:            return "r9g9b9e5";
        case format::r16g16b16a16_float:  return "r16g16b16a16_float";
        case format::r16g16b16a16_unorm:  return "r16g16b16a16_unorm";
        case format::r32g32b32a32_float:  return "r32g32b32a32_float";
        case format::r16g16_float:        return "r16g16_float";
        case format::r16g16_unorm:        return "r16g16_unorm";
        case format::r8g8_unorm:          return "r8g8_unorm";
        case format::r16_float:           return "r16_float";
        case format::r16_unorm:           return "r16_unorm";
        case format::r8_unorm:            return "r8_unorm";
        case format::r32_float:           return "r32_float";
        case format::d32_float:           return "d32_float";
        case format::d32_float_s8_uint:   return "d32_float_s8";
        case format::d24_unorm_s8_uint:   return "d24_unorm_s8";
        case format::r24_g8_typeless:     return "r24g8_typeless";
        case format::d16_unorm:           return "d16_unorm";
        default:                          return "?";
    }
}

// Short form of the same name, for the Buffers gallery's one-line chip row where
// "r16g16b16a16_float" would be most of the line. Same information, industry shorthand; the exact
// name and the DXGI id stay one hover away. "" = no shorthand, caller falls back to the numeric id.
static const char* format_short_name(format f) {
    switch (f) {
        case format::r8g8b8a8_unorm:      return "rgba8";
        case format::r8g8b8a8_unorm_srgb: return "rgba8 sRGB";
        case format::r8g8b8a8_typeless:   return "rgba8 typeless";
        case format::b8g8r8a8_unorm:      return "bgra8";
        case format::b8g8r8a8_unorm_srgb: return "bgra8 sRGB";
        case format::r10g10b10a2_unorm:   return "rgb10a2";
        case format::r11g11b10_float:     return "r11g11b10f";
        case format::r9g9b9e5:            return "rgb9e5";
        case format::r16g16b16a16_float:  return "rgba16f";
        case format::r16g16b16a16_unorm:  return "rgba16";
        case format::r32g32b32a32_float:  return "rgba32f";
        case format::r16g16_float:        return "rg16f";
        case format::r16g16_unorm:        return "rg16";
        case format::r8g8_unorm:          return "rg8";
        case format::r16_float:           return "r16f";
        case format::r16_unorm:           return "r16";
        case format::r8_unorm:            return "r8";
        case format::r32_float:           return "r32f";
        case format::d32_float:           return "d32f";
        case format::d32_float_s8_uint:   return "d32f s8";
        case format::d24_unorm_s8_uint:   return "d24s8";
        case format::r24_g8_typeless:     return "r24g8";
        case format::d16_unorm:           return "d16";
        default:                          return "";
    }
}

// Plain-language "what this buffer holds" story for the RT Browser: bit depth, numeric type, and
// value range (HDR = floats can exceed 1.0; LDR = fixed 0..1). Complements the raw format id.
// Empty string = format not specifically annotated (raw name + fmt id still shown).
static const char* format_human(format f) {
    switch (f) {
        case format::r8g8b8a8_unorm:
        case format::r8g8b8a8_typeless:
        case format::b8g8r8a8_unorm:      return "8-bit RGBA, LDR 0..1";
        case format::r8g8b8a8_unorm_srgb:
        case format::b8g8r8a8_unorm_srgb: return "8-bit RGBA sRGB, LDR (gamma-encoded)";
        case format::r10g10b10a2_unorm:   return "10-bit RGB + 2-bit alpha, LDR 0..1";
        case format::r11g11b10_float:     return "11/10-bit float RGB, HDR (>1 ok), no alpha";
        case format::r9g9b9e5:            return "9-bit RGB shared-exponent, HDR";
        case format::r16g16b16a16_float:  return "16-bit half-float RGBA, HDR (>1 ok)";
        case format::r16g16b16a16_unorm:  return "16-bit RGBA, high-precision 0..1";
        case format::r32g32b32a32_float:  return "32-bit float RGBA, HDR (full precision)";
        case format::r16g16_float:        return "16-bit float, 2 channels";
        case format::r16g16_unorm:        return "16-bit, 2 channels, 0..1";
        case format::r8g8_unorm:          return "8-bit, 2 channels";
        case format::r16_float:           return "16-bit float, 1 channel";
        case format::r16_unorm:           return "16-bit, 1 channel, 0..1";
        case format::r8_unorm:            return "8-bit, 1 channel, 0..1";
        case format::r32_float:           return "32-bit float, 1 channel";
        case format::d32_float:
        case format::d32_float_s8_uint:   return "32-bit float depth buffer";
        case format::d24_unorm_s8_uint:
        case format::r24_g8_typeless:     return "24-bit depth + 8-bit stencil";
        case format::d16_unorm:           return "16-bit depth buffer";
        default:                          return "";
    }
}

// Bytes per pixel (0 = unknown/compressed) — "how much each pixel weighs".
static uint32_t format_bpp(format f) {
    switch (f) {
        case format::r8_unorm:                                                return 1;
        case format::r8g8_unorm: case format::r16_float: case format::r16_unorm:
        case format::d16_unorm:                                               return 2;
        case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb:
        case format::r8g8b8a8_typeless: case format::b8g8r8a8_unorm:
        case format::b8g8r8a8_unorm_srgb: case format::r10g10b10a2_unorm:
        case format::r11g11b10_float: case format::r9g9b9e5:
        case format::r16g16_float: case format::r16g16_unorm:
        case format::r32_float: case format::d32_float:
        case format::d24_unorm_s8_uint: case format::r24_g8_typeless:         return 4;
        case format::r16g16b16a16_float: case format::r16g16b16a16_unorm:
        case format::d32_float_s8_uint:                                       return 8;
        case format::r32g32b32a32_float:                                      return 16;
        default:                                                              return 0;
    }
}

// Cinematics-creator view of a render target. IMPORTANT: everything here is derived ONLY from the
// pixel format, so every statement is 100% certain (HDR-capable vs clamped, bit depth / banding,
// channel count, depth-format). We deliberately do NOT guess semantic role ("beauty", "normals",
// "motion", "bloom") - the format can't prove that, so it would be a lie. The live thumbnail is the
// ground truth for what a buffer actually is; text only adds what the eye can't see (range, depth).
enum RtRole { RTR_HDR, RTR_HIPREC, RTR_SDR10, RTR_SDR8, RTR_DEPTH, RTR_DATA, RTR_MASK, RTR_OTHER };
static RtRole format_role(format f) {
    switch (f) {
        case format::r11g11b10_float: case format::r9g9b9e5:
        case format::r16g16b16a16_float: case format::r32g32b32a32_float:     return RTR_HDR;
        case format::r16g16b16a16_unorm:                                      return RTR_HIPREC;
        case format::r10g10b10a2_unorm:                                       return RTR_SDR10;
        case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb:
        case format::r8g8b8a8_typeless: case format::b8g8r8a8_unorm:
        case format::b8g8r8a8_unorm_srgb:                                     return RTR_SDR8;
        case format::d16_unorm: case format::d24_unorm_s8_uint: case format::d32_float:
        case format::d32_float_s8_uint: case format::r24_g8_typeless:         return RTR_DEPTH;
        case format::r16g16_float: case format::r16g16_unorm:
        case format::r8g8_unorm:                                              return RTR_DATA;
        case format::r8_unorm: case format::r16_unorm:
        case format::r16_float: case format::r32_float:                       return RTR_MASK;
        default:                                                              return RTR_OTHER;
    }
}
// Label + one-line "what you'd use it for" + a tint. The eight roles above keep their full detail
// in the label/use text; the TINT collapses them onto four decisions the eye can actually separate
// on top of a game (see the token block in mpr_theme.inc):
//   green = colour worth grading (float HDR, 16-bit, 10-bit)   amber = 8-bit colour, limited
//   blue  = a depth format, not a picture                      grey  = 1-/2-channel data
// Merging HDR/16-bit/10-bit under one green is the deliberate part: they differ in how MUCH
// latitude you get, not in whether the buffer is a gradeable picture, and that "how much" is what
// the label spells out. Nothing about format_role()'s classification changed here.
struct RoleView { const char* label; const char* use; ImVec4 col; };
static RoleView role_view(RtRole r) {
    switch (r) {
        case RTR_HDR:    return { "HDR (float RGB)", "Floats exceed 1.0 - full grading latitude, push exposure in post", MPR_ROLE_GRADE };
        case RTR_HIPREC: return { "16-bit RGBA",     "Clamped 0..1 but 16-bit - grades cleanly, no banding",             MPR_ROLE_GRADE };
        case RTR_SDR10:  return { "10-bit RGB",      "Clamped 0..1, 10-bit - decent grading headroom",                   MPR_ROLE_GRADE };
        case RTR_SDR8:   return { "8-bit RGBA",      "Clamped 0..1, 8-bit - bands if pushed hard",                       MPR_ROLE_LIMITED };
        case RTR_DEPTH:  return { "Depth/stencil",   "A depth-buffer format - distance per pixel, not a color image",    MPR_ROLE_DEPTH };
        case RTR_DATA:   return { "2-channel",       "Only two channels - cannot be a full-color image",                 MPR_ROLE_DATA };
        case RTR_MASK:   return { "1-channel",       "Single channel - grayscale/scalar, not a color image",             MPR_ROLE_DATA };
        default:         return { "Buffer",          "",                                                                 MPR_ROLE_DATA };
    }
}

