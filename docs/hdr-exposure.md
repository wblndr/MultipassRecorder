# HDR Exposure & Bit Allocation

Why the **Exposure (stops)** slider exists, and what it actually does. Exposure never
touches the source buffer's information — it decides *where that information lands inside a
smaller, fixed-shape output container*.

## The core mismatch

| | Source | Output |
|---|---|---|
| Format | `r11g11b10_float` (floating point) | 10-bit HEVC (fixed point) |
| Range | ~2⁻¹⁴ … ~64000 linear | fixed curve window |
| Precision | ~6-bit mantissa (modest) | **1024 code values, total** |

A float spends bits on an *exponent*, so it has huge range but modest precision. The 10-bit
file has 1024 codes spread across a **fixed input window** set by the curve:

- **PQ** — 0…10 000 nits, with `paper_nits = 100` meaning scene-linear `1.0` = 100 nits.
- **Log2** — a fixed 2⁻¹⁴ … 2⁺⁶ (20 stops).
- **Rec.709** (Reinhard+gamma) — roughly 0…1.

Wide float in, narrow fixed bucket out. **Exposure = which slice of the float the bucket
points at.** It multiplies linear by `2^stops` *before* the curve. It is ISO on a camera.

## Why not "just capture the whole range, period"

1. **The output window is narrower than the buffer.** PQ@100 clips above scene-linear 100,
   even though the buffer holds up to ~64000. You always capture a *slice*; exposure (+
   paper-nits) aims it.
2. **Warframe's buffer is pre-exposure (~10 stops dark)** — the whole scene sits at the
   bottom of the window. At 0 stops on the gamma/Rec.709 path a scene at ~0.0009 linear comes
   out around code **8 of 255** → banding ("crushed as hell"). Lifting exposure spreads that
   same scene across hundreds of codes. No information is added or removed — the finite codes
   are just spent on the data instead of on empty headroom.

## Curve nuance (it depends)

- **PQ / Rec.709 (gamma):** codes are distributed *non-uniformly*, so repositioning the scene
  into the fine-step part of the curve **buys real precision** (fewer bands). The big win.
- **Log2:** ~uniform codes-per-stop, so exposure doesn't add precision — but it keeps the
  scene off the 2⁻¹⁴ floor (hard black) and centers it for the grade.
- **Two-sided:** too high → highlights clip past the ceiling (PQ 10 000 nits / Log2 2⁺⁶) =
  real, unrecoverable loss. Too low → crushed shadows. Balance with the stops scope.

## It's invertible

Exposure is baked into the pixels but **mathematically invertible**: subtract the same stops
in post → true scene-linear values (as long as nothing clipped). It protects *precision*
through a 10-bit pipe; it does not change what the data *means*. AE gets correct absolute
values either way.

## So which workflow needs it

- **HEVC 10-bit (PQ/Log2)** — compact, real-time, gradeable. **Needs exposure**: 1024 codes
  force you to say where they go.
- **PNG 16-bit linear** — near-verbatim buffer dump (`linear × 4096` → uint16, no curve),
  exposure = 0. Captures the whole range, but files are huge and you lift in post anyway.

**Rule of thumb:** max-fidelity / zero-decisions → PNG 16-bit. Gradeable real-time video →
HEVC 10-bit, and there exposure is the one knob you can't skip.
