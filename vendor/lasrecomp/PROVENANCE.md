# Vendored LASRecomp components

- Source: https://github.com/peiikai/LASRecomp
- Revision: `a4b53a3fc5e91bf101367063bafe45017dc23335`
- License: MIT, retained in `LICENSE` in this directory.

Copied components: `tools/gen.py`, `tools/lifter.py`, `tools/simd_c.py`,
`tools/analyze.py`, `tools/nso.py`, `runtime/cpu.h`, `runtime/simd.h`, and
`runtime/simd_fp.h`, together with the generic NVN/Vulkan/Maxwell runtime,
RomFS builder and SDL audio mixer. Runtime headers and minimal UI declarations
needed to compile these components are retained. SMAA source/textures and the
Ryujinx attribution accompanying the mixer retain their own license files in
`third_party/`. No game binaries, keys, game address tables, Zelda patch data or
randomizer data are included in this source tree.

Local changes:

- `gen.py`: remove Zelda build-ID gating and imports of game address/patch tables;
  remove randomizer-data loading; include the text entry in boundaries; validate
  generation sizes and C module identifiers; preserve full NSO build IDs; emit
  machine-readable translation diagnostics. Empty hook dictionaries remain for
  compatibility with the upstream emitter.
- `nso.py`: replace the reader with a bounded implementation retaining the
  upstream public attributes; support ELF32 as well as ELF64 symbols and REL/RELA
  tables; validate ranges, decompression and enabled section hashes; retain all
  32 build-ID bytes; handle empty exception-header ranges.
- `runtime/rt.h`: shared address-space layout aligned with the native host.
- `runtime/romfs_build.c`: omit executable modules from the flat asset directory;
  remove unrelated copy/delete helpers. The port reads asset files lazily.
- `services/audio.c`: adapt service objects and handle/event wrappers to this
  host. Original mixer and embedded Ryujinx attribution are retained.
- `runtime/nvn/maxwell/translate.c`: support the observed vertex VOTE.VTG.RA
  hint, ordinary LEA scaled additions, conservative vertex clip exits and W-scale
  registers. Unknown extended variants still fail with diagnostics.
- `runtime/nvn/vulkan/device.c`: connect native screenshot requests and emit
  integration-test completion statistics before terminating the test process;
  create the screenshot directory on the first capture.
- `runtime/nvn/tegra.c`: use one GOB in Y for 3D textures and select the tile
  depth using the depth-times-1.5 rule (maximum 16 GOBs). This fixes interleaved
  color-grading LUT slices. Public layout reference:
  https://github.com/devkitPro/deko3d/blob/master/source/dk_image.cpp.
- `runtime/nvn/cmdbuf.c`, `nvn.h`, `shader_prep.h`, `vulkan/exec.c`: record
  depth clamp as command-list state, include it in pipeline keys and prepared
  variants, and enable Vulkan depth clamping. Kirby's 2D menu planes lie beyond
  the near plane and require this state. Uniform diagnostics now use the actual
  hardware-to-user constant-buffer mapping.
- `runtime/nvn/maxwell/translate.c`: original GLSL TMML mipmap-query support for
  the observed non-array 2D fragment form, with component-mask packing and
  reversed signed 8.8 fixed-point components. ISA references:
  https://github.com/envytools/envytools/blob/master/envydis/gm107.c and
  Ryujinx's `InstEmitTexture.EmitTmml` semantics. Unsupported forms fail explicitly.
- `runtime/settings.h`, `nvn/output_scale.c`, `nvn/vulkan/device.c`: connect the
  original Kirby host settings to custom window/render dimensions; require the
  Vulkan depth-clamp feature used by the menus. The game's composition stays 16:9.
- Other copied NVN source files retain their upstream implementations.
  `runtime/graphics_bridge.c` in the parent project binds host entrypoints and
  disables Zelda-specific modifications; it does not load Zelda addresses.

The Python dependencies and compiler in `requirements*.txt` retain their own
licenses. They are separately installed in a local virtual environment.
