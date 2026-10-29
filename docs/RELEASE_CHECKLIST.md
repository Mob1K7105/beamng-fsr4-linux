# Release checklist

Done:
- [x] FSR 4.1.1 identity verified byte-exact (DLL ≡ port ≡ in-game output)
- [x] Validation clean against the game's own baseline
- [x] Jitter prediction ≥ 99.9% across resets and level loads
- [x] Fail-open on every identification/consistency check; runtime FSR 4 ↔ FSR 3 switch with handover
- [x] FSR 3 passes skipped under FSR 4 (output unchanged, ~1.2 ms saved), plus the engine's FSR 3 reactive-mask pass
- [x] Lean hook set by default (draws/copies/barriers/submits not intercepted); per-pass wave size (−0.035 ms, byte-exact)
- [x] Sharpening follows the game's Sharpness slider (read from the game's RCAS constants)
- [x] Package 0.1.0 (`package.sh`): one switch `BNG_FSR4=1`, user-level install/uninstall with
      checksum verification, implicit-layer manifest, assets next to the library, log in `~/.cache/bng-fsr4`,
      inert outside BeamNG, GPL source bundled and building standalone. Tested: scratch install, inert in
      `vulkaninfo`, direct game launch with only `BNG_FSR4=1` (1,801 frames replaced, hard 0)
- [x] Menu label: "FSR 3 (custom)" reads "FSR 4.1.1 (Linux)" while the layer runs (BeamNG mod
      `bng_fsr4_linux.zip`, status file `/temp/bng_fsr4/status.json`). Verified with 20 user mods mounted: the
      key swap, and `core_locales` (the UI's string source) returns "FSR 4.1.1 (Linux)" on a normal startup
- [x] **Steam launch** with `BNG_FSR4=1 %command%` and the real `install.sh`: runs in the Steam Linux Runtime
      sniper container; the layer, the Steam overlay and Fossilize are all loaded; 10,801 frames replaced in
      gridmap, hard 0, jitter mismatches 0, no amdgpu errors

0.2.0 (FSR 4.1.2):
- [x] Generic and Navi2 INT8 sets captured from the SDK 2.4.0 DLL; each 7/7 byte-exact against it
- [x] Ground-truth quality test (pan and static): same quality as 4.1.1; Navi2 2–3% less GPU time
- [x] Layer picks `assets/navi2` on Navi 2x; `BNG_FSR4_VARIANT` overrides
- [x] In-game (BeamNG 0.40.1.0): BeamNG loads the Navi2 set (`events.log`, SPIR-V identical), live frames equal
      an offline replay (22/22), 10,801 frames hard 0, output-size change while running
- [x] Installed package, direct launch with only `BNG_FSR4=1`: 1,801 frames replaced, hard 0
- [x] Menu label reads "FSR 4.1.2 (Linux)"
- [x] Steam launch on BeamNG 0.40.4.0 (Steam Linux Runtime sniper, overlay and Fossilize loaded): 10,801
      frames replaced in gridmap, hard 0

Before a public release:
- [ ] **Release build:** the package uses the lean mode (small log in `$XDG_CACHE_HOME/bng-fsr4`); the diagnostic
      code paths are still compiled in and could be removed behind a build flag
- [x] **Naming:** project and package `beamng-fsr4-linux`; layer `VK_LAYER_BNG_fsr4`, switch `BNG_FSR4`
- [ ] **Assets:** fetch and verify from the pinned FSR-4.1.1-linux revision at install time, or bundle with notices
- [ ] **Other layers switched on:** MangoHud (`MANGOHUD=1`), gamescope, and the Steam overlay drawn in-game (Shift+Tab)
- [ ] **Coverage:** 1080p and 4K output, Ultra Performance (3×) model, output-resolution change while running
- [ ] **Hardware:** RDNA 3 and RDNA 4 testers, ideally NVIDIA/Intel (portable dot path)
- [ ] **Quality:** side-by-side in motion vs FSR 3 (ghosting, particles, thin geometry); decide default scale
- [ ] **Game updates:** document the tested BeamNG version; fingerprints fail open on mismatch
- [ ] **Licensing:** GPL source release, AMD notice bundle; check BeamNG's EULA/mod policy
- [ ] **User docs:** screenshots, FAQ, bug-report template (GPU, Mesa version, the `frames replaced … fail-open` log line)
