# Renderer and shutdown fixes — 27 October 2026 (AI-assisted)

Texture conversion now distinguishes opacity from material masks. Opaque and mixed-use
materials retain RGB at alpha zero during resizing, mip generation, transparent-color cleanup
and ETC quality decisions. Cutouts retain alpha-aware filtering and coverage preservation.
DrawMode's TCC getter now reads bit 7, matching its setter, rather than the filtering bit.

The v21 static reflection path now runs for Jak 1 as well as Jak II, including conversion,
backend shader loading, game-specific tint and reflection strength. Existing C3L files must be
regenerated to receive the texture and reflection improvements; the format version is unchanged.

Low-resolution terrain can be selected beyond the detailed draw radius even within a level's
large static detail bounds. A conservative overlap margin includes hysteresis and the detail
chunks' sphere radii. This addresses missing distant terrain without replacing nearby detail.

The full ocean's wave texture is restored from 64x64 to 128x128. Ocean shaders sample projective
STQ while retaining screen-affine color, fog and reflection coordinates. Native triangles are
clipped at depth before projecting position to W=1. Software previews now interpolate static
terrain and reflective ties with perspective-correct attributes and full-ocean UVs projectively.

Other pending asset corrections preserve full vista vertex attributes at welds, include UVs,
lighting and skin weights in generated model LOD errors, preserve atlas draw order and hash
complete VRAM uploads. CPU regression tests cover these cases. Citadel lighting, command-cache,
sprite and impostor optimizations are described in [performance.md](performance.md).

Forced HOME/power closing previously called `_exit()` with live workers. SDK teardown unmapped
their heap-backed stacks before ending the process, allowing a waking worker to fault. The
replacement completes the APT close handshake while retaining shared heaps and service state
until kernel termination. Pending saves, log draining and DSP shutdown are retained. Both the
ordinary forced-close caller and a crash during HOME closing use this path.

## Validation and limits

- Host runtime, converter, regression and 3DS builds passed.
- Converter/VRAM/lighting/sprite regressions, command-cache paging under ASan/UBSan, shadow
  reference decoding and ocean replay/depth clipping passed. All 31 vertex shaders passed the
  output-write check.
- All 27 Jak 1 levels were reconverted with readback verification. One audited metal texture's
  RGB RMS error fell from 10.30 to 3.29 on a 0..255 scale.
- Actual Azahar title and final-boss runs completed. A fixed-camera comparison showed the
  expected reflection tint change; disabling mips and optimized drawing did not change the
  sampled foreground patch.
- Directing the old active graphics loop into its forced-close branch under GDB reproduced a
  worker resuming on an unmapped stack. The repaired close function completed process cleanup
  with workers active. A subsequent title-to-final-boss run and normal shutdown also passed.
  This was debugger-directed validation, not a physical HOME-menu test.

Intermittent model-to-ground shadow extrusion remains unresolved. Camera-dependent texture
stretching in Azahar has not been established as fixed. Wave generation, texture compression,
geometry LODs and vistas still impose fidelity limits; PS2 pixel parity is not claimed. Physical
3DS validation and hardware performance measurements remain outstanding.
