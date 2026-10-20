# Known issues and limitations

- **Costs more than FSR 3 on RDNA 2.** FSR 4.1.1 INT8 takes about 2.7 ms per frame at 1440p output on an
  RX 6700 XT; with FSR 3's passes skipped, the net extra cost is about 1.1 ms. See [PERFORMANCE.md](PERFORMANCE.md).
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
  RDNA 3/4, NVIDIA (no `VK_VALVE_shader_mixed_float_dot_product`: the portable path is not bit-exact),
  Intel, outputs other than 2560×1440, HDR, VR, multi-monitor.
- **Image-quality verdict under motion** is still open. Static scenes look clean, and a wrong-sign
  motion-vector test confirmed the motion handling. A systematic side-by-side in motion has not been done.
- **Diagnostic code is still compiled in.** In its default lean mode it only writes a short event log;
  `FSR4STUDY_DIAG=1` turns on the detailed log (several MB per 30 minutes) and shader dumps.
