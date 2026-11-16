# Known issues and limitations

- **Costs more than FSR 3 on RDNA 2.** FSR 4.1.2 INT8 (RDNA 2 kernels) takes about 2.6 ms per frame at 1440p
  output on an RX 6700 XT; with FSR 3's passes skipped, the net extra cost is about 1 ms. See [PERFORMANCE.md](PERFORMANCE.md).
- **One frame after a same-size game reset** (for example toggling the upscaler) uses a stale jitter value,
  because the reset is only visible in the late readback. FSR 3 then takes over for 17–32 frames while the
  jitter is relearned.
- **Output-resolution changes while the game runs** are untested. The game ignored resolution changes
  made from Lua.
- **Only `fsrCustom` is supported.** With any other anti-aliasing mode the layer does nothing.
- **Menu label:** the mod needs BeamNG's mods to be enabled. After a game update BeamNG may turn all mods
  off; the label then stays "FSR 3 (custom)", while FSR 4 still runs. Only English text is provided; other
  languages fall back to it. The label and its text were checked through the game's Lua and locale data, not
  in a screenshot of the menu.
- **Steam's shader processing:** Fossilize records FSR 4's compute pipelines too. On the next launch, Steam
  processed them once for about a minute ("Processing Vulkan shaders").
- **Not tested:** MangoHud or gamescope active together with the layer (both were loaded but not switched on),
  the Steam overlay drawn in-game (it loaded and the game ran normally), GPUs other than the RX 6700 XT,
  RDNA 3, RDNA 4 (one user report, below), NVIDIA (no `VK_VALVE_shader_mixed_float_dot_product`: the portable path is not bit-exact),
  Intel, outputs other than 2560×1440, HDR, VR, multi-monitor. The generic INT8 set (used on GPUs other than
  Navi 2x) was verified on the RX 6700 XT only, by forcing it with `BNG_FSR4_VARIANT=generic`.
- **High output resolutions cost much more.** FSR 4's cost follows the output size: at 5120×1440 it is about
  twice the 1440p cost (4.8 ms on an RX 6700 XT, 3.3 ms with Lite), and it can eat most of what a lower
  render scale saves. See [PERFORMANCE.md](PERFORMANCE.md), "High output resolutions".
- **RDNA 3 and RDNA 4 run the INT8 generic set.** There is no FP8 path, so RDNA 4 does not use its FP8
  hardware. One RX 9060 XT report (5120×1440) showed FSR 4 running correctly on the generic set, but with a
  high GPU cost.
- **0.3.0 in-game:** run on 2026-11-16 (default, Lite and generic sets, installed package, BeamNG 0.40.4.0):
  frames replaced, hard 0, clean output in frame dumps. A live-vs-offline byte-exact replay was not repeated for
  0.3.0; the shaders were checked byte-exact against AMD's DLL offline (docs/VERIFICATION.md).
- **Lite is a different network.** A third-party comparison (fsr4-shader-performance-overrides, docs/lite-builds.md)
  found it slightly less accurate on still images, better on some motion (thin detail, swaying foliage) and worse
  on others (ghosting beside moving objects, fades).
- **Image-quality verdict under motion** is still open. Static scenes look clean, and a wrong-sign
  motion-vector test confirmed the motion handling. A systematic side-by-side in motion has not been done.
- **Diagnostic code is still compiled in.** In its default lean mode it only writes a short event log;
  `FSR4STUDY_DIAG=1` turns on the detailed log (several MB per 30 minutes) and shader dumps.
