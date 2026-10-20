# Licensing and credits

*Not legal advice. Check before distributing.*

## Components

| Component | License | Notes |
|---|---|---|
| This layer (`diag_layer.cpp`, build scripts) | GPL-2.0-or-later | Required: it links the GPL runtime below |
| FSR 4.1.1 Vulkan runtime (`fsr411.cpp/.h`) from [FSR-4.1.1-linux](https://github.com/johnpanos/FSR-4.1.1-linux), adapted from [bbport](https://github.com/deadinside28/bloodborne_pc) | GPL-2.0-or-later | **Modified** (2026-10-20): per-pass required subgroup size (`kWave`), descriptor bindings resolved at load, `subgroup_size_control` constructor argument. Output unchanged (byte-exact against the AMD DLL). Two device queries are redirected by build macros |
| Menu-label BeamNG mod (`bng_fsr4_linux.zip`) | GPL-2.0-or-later (same as the layer) | Adds one translation key and relabels one menu entry at runtime; overrides no game file |
| FSR 4.1.1 shader programs and model weights (`assets/`) | AMD's MIT exception for `amd_fidelityfx_upscaler_dx12.dll` (FidelityFX SDK 2.3.0) | Modified (DXIL→SPIR-V, bindings); not an AMD-signed distribution. Ship `notices/AMD-SDK-LICENSE.md` and `assets/NOTICE.md` |
| dxil-spirv (asset preparation only) | MIT | Not part of the runtime |

## Obligations when distributing

- Release the complete layer source (GPL) with every binary, plus the GPL text. `mod/packaging/package.sh`
  puts the source, `LICENSE` and `notices/` into every package.
- Keep AMD's SDK notice and the MIT exception text with the assets, and keep the "modified, unofficial"
  wording.
- Do not imply endorsement by AMD or BeamNG GmbH. "FSR" and "FidelityFX" are AMD trademarks; use them only
  to describe compatibility.
- No AMD DLL is redistributed.

## BeamNG

No game files are modified or redistributed. The layer only intercepts Vulkan calls of the native Linux
build. Check BeamNG's EULA and modding policy before publishing. The layer itself is not a BeamNG mod and is
distributed separately. The optional menu-label zip is a normal BeamNG mod: it follows the game's
`NOTE_FOR_MODDERS.md` (unique names, no overridden game files, works when dropped into `mods/`), but on
its own it only changes a label. It needs the layer.

## Credits

- AMD: FSR 4.1.1 and the FidelityFX SDK.
- deadinside28: bbport, the original Vulkan FSR 4 implementation.
- johnpanos: the standalone FSR-4.1.1-linux port and its verification tooling.
- daniel-h-0: bc250-fsr4-fork patches used for reference-DLL preparation.
- Hans-Kristian Arntzen: dxil-spirv.
