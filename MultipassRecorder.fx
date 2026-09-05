/**
 * MultipassRecorder.fx
 *
 * A REFLECTOR, not a processor. This shader exists for exactly one reason: ReShade resolves the
 * game's depth buffer for you (the "Generic Depth" logic compiled into reshade64.dll itself —
 * see examples/09-depth/generic_depth_addon.cpp), but it publishes the result ONLY as a texture
 * with the DEPTH semantic, which only an EFFECT can declare. There is no addon-facing API to ask
 * for it. So this file declares that texture and copies the buffer into one the addon can read
 * by name; everything else — linearization, normals reconstruction, the world plate, and all the
 * tuning that used to live here as uniforms — is done natively in the addon (mpr_native.inc).
 *
 * WHY R32F AND NOT RGBA8. The old version of this file wrote its linearized depth into an RGBA8
 * texture, which quantized a 32-bit depth buffer to 256 levels before the addon ever saw it —
 * the banding in every depth capture was created HERE, not by ReShade and not by the encoder.
 * Generic Depth preserves the buffer's native precision (a D32F stays D32F, made typeless so a
 * matching SRV is legal), so R32F hands the addon everything the engine actually rendered.
 *
 * KEEP THE TECHNIQUE ENABLED in the ReShade UI. The addon re-enables it on every effect reload
 * (mpr_enable_setup_technique), but if it is off, texDepthRaw is never written and the depth and
 * normals passes capture black.
 *
 * The RESHADE_DEPTH_INPUT_IS_* defines below are the escape hatch for engines whose depth buffer
 * Generic Depth's heuristic reads differently than the game means it (copy-before-clear, reversed
 * Z, flipped V). They are ReShade-global preprocessor settings, deliberately kept here so an
 * existing per-game override keeps working. The addon ALSO offers its own reversed / upside-down
 * / log-curve controls natively, applied when it linearizes.
 */
#ifndef RESHADE_DEPTH_INPUT_IS_COPY_BEFORE_CLEAR
#define RESHADE_DEPTH_INPUT_IS_COPY_BEFORE_CLEAR 1
#endif
#ifndef RESHADE_DEPTH_INPUT_IS_REVERSED
#define RESHADE_DEPTH_INPUT_IS_REVERSED 0
#endif
#ifndef RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
#define RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN 0
#endif

#include "ReShade.fxh"

// The one output. Read by the addon via find_texture_variable("texDepthRaw") -> get_texture_binding.
// R32F: raw, unmodified, full precision. No curve, no range remap, nothing baked in.
texture texDepthRaw { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F; };
sampler sDepthRaw { Texture = texDepthRaw; };

void PS_Reflect(float4 pos : SV_Position, float2 uv : TexCoord, out float o : SV_Target)
{
    o = tex2D(ReShade::DepthBuffer, uv).x;
}

technique MultipassRecorder_Setup
<
    ui_label   = "Multipass Recorder Setup";
    ui_tooltip = "Keep enabled. Hands ReShade's depth buffer to the MultipassRecorder addon at "
                 "full precision. Does no processing of its own - the depth and normals controls "
                 "live in the addon's own panel.";
>
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_Reflect; RenderTarget = texDepthRaw; }
}
