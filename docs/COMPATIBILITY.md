# Compatibility

| Area | Tested | Expected / notes |
|---|---|---|
| BeamNG.drive | 0.40.4.0, 0.40.1.0 and 0.40.0.0 native Linux, Vulkan | Other versions: shader fingerprints may differ → automatic FSR 3 fallback until updated |
| Anti-aliasing mode | FSR (custom) (`fsrCustom`, upscale mode 5) | Other modes untouched |
| Render scale | 0.5, 0.67, 1.0 (mode off) | Any scale ≥ 1/3; ≥ 2.9× upscale uses the Ultra Performance model (untested in-game) |
| Output | 2560×1440 borderless | Up to 3840×2160 supported by the backend; ≤1080p uses the smaller tier (untested in-game) |
| GPU | RX 6700 XT (RDNA 2, Navi 22) | RDNA 3/4 should work (same INT8 path); untested |
| Driver | Mesa 26.2.5 and 26.2.4 RADV | Needs `VK_KHR_compute_shader_derivatives`; AMDVLK/proprietary untested |
| NVIDIA / Intel | — | No `VK_VALVE_shader_mixed_float_dot_product`: the portable prepass runs, which is not bit-exact against AMD |
| Other layers | Mesa device-select, anti-lag, MangoHud manifest present (not enabled) | Steam overlay / Fossilize / gamescope coexistence untested |
| Launch | Direct `BinLinux/BeamNG.drive.x64` | Steam launch options untested |
