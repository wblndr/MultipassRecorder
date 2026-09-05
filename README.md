# MultipassRecorder — ReShade Addon

> **Shelved.** This addon is feature-complete for what it was built to do and is no longer under
> active development. It works — it was validated by hand across several games — but expect rough
> edges, and don't expect fixes. Source is MIT; fork it freely.

A ReShade addon that records **several render passes at once** — depth, screen-space normals, a
clean colour plate, engine buffers, individual effect layers — each to its own video file, encoded
in real time through FFmpeg + NVENC.

It exists to make game footage **compositable**: instead of one flattened capture you get the
separate elements a compositor expects, so you can drop the set into Blender, Nuke, Fusion or After
Effects and treat the game like a render.

---

## What it can record

| Kind | Where it comes from |
|---|---|
| **depth** | ReShade's resolved depth buffer, linearized by the addon at 32-bit float |
| **normals** | reconstructed from depth — screen-space, with FOV and per-axis flips |
| **world** | the back buffer *before* ReShade's effects — a clean colour plate |
| **engine buffers** | any render target the game binds, found live in the **Buffers** tab |
| **effect layers** | a ReShade technique re-rendered privately, so you can record an AO/GI debug view while the game view stays normal |
| **stills** | the screen plus every enabled pass from one frame — EXR / PNG / TIFF / JPEG |

Up to 12 promoted passes run alongside the built-ins. Each pass gets its own codec, bit depth,
chroma, curve and exposure — the UI greys out the combinations that aren't legal.

---

## Requirements

- **ReShade 6.x** with add-on support enabled (the shipped binary registers as add-on API v13)
- **FFmpeg** with NVENC — path is set in the Setup tab
- **NVIDIA GPU**, for NVENC
- **Windows 10 / 11**
- **DirectX 11 is the only tested path.** DX12, OpenGL and Vulkan code paths exist but are
  unverified. DX9 games need the 32-bit build (`compile_dx9.ps1`).

---

## Install

Run this from inside your game's install folder — where the game `.exe` and ReShade's `dxgi.dll`
live:

```powershell
irm https://raw.githubusercontent.com/wblndr/MultipassRecorder/main/install.ps1 | iex
```

Or by hand: `MultipassRecorderAddon.addon` next to the game `.exe`, and `MultipassRecorder.fx`
into `reshade-shaders\Shaders\`. Both are on the [Releases](../../releases) page.

Then launch the game, open the ReShade overlay, and **enable the `MultipassRecorder_Setup`
technique** in the shader list.

> **That last step is not optional for depth work.** The shader is a ~15-line *reflector* — it does
> no processing, it just hands ReShade's resolved depth buffer to the addon at full precision, which
> no add-on API exposes any other way. With the technique off, `depth` and `normals` record black.

---

## Quick start

1. Open the panel from the ReShade overlay → **Record** tab. The "At a glance" strip shows a live,
   correctly-exposed thumbnail of every pass — that's your check that a pass sees something.
2. Enable the passes you want.
3. If depth looks flat, hit **Auto-calibrate**, then set the far plane by eye — it decides which
   distance becomes white, so that one is a creative call.
4. For normals, set **Vertical FOV** to match the game's. The test is a flat wall head-on: one
   solid colour that stays solid as you pan. Curved toward the edges means the FOV is wrong.
5. **F9** records every enabled pass at once; **F10** takes a still. Both rebindable.
6. Files land in `<ReShade folder>\captures\` as `<game>_<timestamp>_<pass>`.

Every control has a **(?)** marker explaining it. Watch the recording OSD while you shoot — it
names any pass whose encoder died and shows dropped frames as they happen.

HDR passes carry more range than a 10-bit file holds, so each has an **Exposure (stops)** control
deciding which part of that range survives the container — see
[`docs/hdr-exposure.md`](docs/hdr-exposure.md). Stills sidestep it: EXR is written scene-linear with
no exposure or curve baked in, next to a viewable `_preview.png` that does bake the look.

---

## Troubleshooting

**It says REC but files are empty or missing.** Almost always the FFmpeg path — check the Setup
tab. The panel and OSD name any pass whose encoder died.

**Depth and normals are black.** The `MultipassRecorder_Setup` technique is off. Turn it on.

**Some passes record, others don't.** NVENC caps concurrent sessions — 3 on older consumer
drivers, 8 since 2023 — and every video pass opens one. Passes past the cap fail at startup and are
reported by name. Move one to the PNG sequence codec, or record fewer at once.

**Depth is a flat field.** Usually mis-tuned, not broken. Run Auto-calibrate, then check the raw
depth readout: identical min and max means the source really is empty; otherwise it's the
multiplier or far plane.

**Frames are dropping.** All passes drop together by design, so they stay frame-synchronized.
Reduce resolution, capture frame rate, or the number of passes.

---

## Known limitations

- DX11 is the only tested path; native depth/normals are DX11-only.
- NVENC caps at 10-bit — use the 16-bit PNG sequence or stills for more.
- Effect layers that declare `SRGBWriteEnabled` come out dark (only one render target is handed to
  the technique). The AO/GI debug views this feature exists for aren't affected.
- No Spout/NDI live output.
- If the host runtime's ImGui is older than the build's, the overlay is unavailable — recording
  still works fully.

Log2 captures decode with `linear = 2^(code*20-14)`; the ready-made `.cube` LUTs are attached to
the [latest release](../../releases/latest).

---

## Building

```sh
git submodule update --init --recursive
git -C deps/reshade checkout v6.5.1
git -C deps/reshade submodule update --init deps/imgui

cmake -S . -B build64 -A x64
cmake --build build64 --config Release
```

`main.cpp` is the only translation unit — the `mpr_*.inc` fragments are `#include`d into it in
declaration order. For a 32-bit DX9 host use `compile_dx9.ps1`; for a legacy ReShade 5.x host add
`-DMPR_TARGET_API_VERSION=8`.

---

## License

**MIT** — see [`LICENSE`](LICENSE). Nothing is paywalled.
