# LASRecomp architecture and porting status

Inspected source: [peiikai/LASRecomp](https://github.com/peiikai/LASRecomp),
commit `a4b53a3fc5e91bf101367063bafe45017dc23335`.
The required upstream components are vendored with their license and provenance.

## How the upstream project works

1. `tools/nso.py` decompresses NSO segments and reads MOD0 metadata, dynamic
   symbols and relocation tables.
2. `tools/gen.py` finds functions through symbols, unwind records, relocation
   targets, immediate calls and materialized code addresses. It repeatedly
   partitions functions until branch targets agree with the boundaries.
3. `tools/lifter.py` translates AArch64 integer, memory and control-flow
   instructions into C using a guest CPU structure. `simd_c.py` and `simd.h`
   implement portable floating-point and vector operations. On ARM64 hosts the
   upstream generator can also use native ARM64 inline assembly.
4. Generated C is compiled into a game-code library on the player's machine.
   Its function-address tables connect indirect branches to native functions.
5. `runtime/loader.c`, `dispatch.c` and `kernel.c` load the supplied modules,
   relocate symbols, route branches and provide guest thread/SVC behavior.
   SDK HLE functions and IPC services supply host behavior for console APIs.
6. The NVN layer and Maxwell shader translator implement graphics on Vulkan
   or Metal. SDL and FFmpeg provide other host facilities.

These are source observations, not a claim that another game is compatible.
The upstream generator explicitly selects Link's Awakening build IDs and loads
its patches. The runtime uses Zelda-specific address tables, hooks, randomizer
logic and asset assumptions. Its loader and dispatch library are organized
around just `main` and `sdk` at fixed bases. Replacing a title ID is insufficient.

## What is implemented here

- Public PFS0 package-directory inspection, without reading ticket contents.
- Bounded NSO parsing, optional LZ4 decompression and enabled SHA-256 checks.
- Both ELF32 and ELF64 dynamic symbol and relocation metadata parsing.
- NPDM program-ID and instruction-width checks.
- Module discovery for `rtld`, `main`, `subsdk0` through `subsdk9`, and `sdk`.
- Import/export analysis and optional upstream HLE candidate matching.
- ARM32 linear instruction inventory; ARM64 lifter coverage inventory.
- ARM64 C generation with Zelda build restrictions, patches and randomizer
  data loading removed; module entry addresses included in discovery.
- Build identity manifests, native object compilation and synthetic execution
  comparisons against Unicorn.
- Windows native host: relocated Kirby modules, native dispatch, guest stacks/TLS,
  Windows worker threads, event/mutex/address arbitration, HIPC/CMIF domain IPC,
  virtual RomFS and confined local saves.
- Ported LASRecomp NVN/Maxwell/Vulkan renderer, SDL audio mixer and native input.
  The title, story and village tutorial render with corrected color grading.

The generator still inherits the upstream discovery and instruction-support
limits. Unknown instructions can produce explicit traps, and unresolved
indirect targets need runtime investigation. C generation or compilation is
not evidence of a bootable game.

## Mario Kart 8 Deluxe finding

The supplied `main.npdm` identifies program `0100152000022000` and has
`Is64BitInstruction` unset. Its executable contains ARM32 instructions,
ELF32 symbols and ARM relocation types 2, 21, 22 and 23.

The local copy contains eight NSO modules: `rtld`, `main`, `subsdk0`, `subsdk1`,
`subsdk2`, `subsdk3`, `subsdk4`, and `sdk`. Together their guest exports cover
the strong imports found by the metadata analysis. This says nothing about
whether the imported functions' console dependencies work on a PC.

LASRecomp's AArch64 lifter and `Cpu` ABI cannot execute that code. An ARM32
translator, ARM32 register/stack ABI, guest address handling, ARM/Thumb branch
handling, VFP/NEON semantics and ARM32 SVC/IPC marshalling would be required.
The current generator explicitly rejects it rather than misreading it as ARM64.

## Super Kirby Clash status

The supplied ExeFS is ARM64. All three modules translate and compile. The Windows
host applies 53,615 relocations and provides the services observed during startup.
Native Vulkan runs on the local RTX 4070 SUPER. Captured runs have reached the title,
opening story, village dialogue and controllable village. The scene runs around
30 fps and the title around 60 fps on this machine. This is observed execution,
not a compatibility guarantee for other stages or machines.

Three shader issues encountered in Kirby were addressed: the observed VOTE.VTG.RA
resource/clip hint, ordinary LEA address addition, and EXIT.FCSM_TR. The conditional
clip exit must not become an unconditional return before position exports. Vulkan
performs clipping on exported positions, so the extra early clip exit is skipped
conservatively. Unknown LEA extensions/EXIT conditions still report errors.
The two W-scale system registers use conservative +/-w clip bounds.

The scene's neon color fault came from treating 3D grading LUTs as 2D block-linear
textures. Their 16x16x16 RGBA slices require a single GOB in Y and 16 GOBs in Z.
The old layout interleaved the upper Y rows with Z slices. The corrected layout
has been checked in the village and against an original synthetic coordinate LUT
whose tiled input is constructed independently from explicit hardware address bits.

Native services implement an offline, single-user profile. Network requests remain
disconnected; account services do not fabricate Nintendo online authentication.
Delivery cache and DLC enumeration are empty. Host UTC supplies local time.
The game profanity system dictionary is absent from RomFS; the independent host
fallback handles a small ASCII word list only. It does not reproduce Nintendo's
language dictionaries.

Remaining verification/work: exercise the first quest/battle and save restoration
after completing it, check later stages,
handle additional services as observed, and broaden controller/device coverage.
The source is a development port, not a validated complete-game release.

## Format references

- [Switchbrew NSO0](https://switchbrew.org/wiki/NSO0)
- [Switchbrew NPDM](https://switchbrew.org/wiki/NPDM)
- [Switchbrew NCA/PFS0](https://switchbrew.org/wiki/NCA)

The CPU width finding comes from the local files themselves; the format
documentation explains how the fields are interpreted.
