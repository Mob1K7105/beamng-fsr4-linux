# Install and use

## 1. Install the package

Download `beamng-fsr4-linux.tar.gz` from the latest release, unpack it and run the installer as your
normal user (no root). In one line:

```sh
curl -fsSL https://github.com/Mob1K7105/beamng-fsr4-linux/releases/latest/download/beamng-fsr4-linux.tar.gz | tar xz && ./beamng-fsr4-linux-*/install.sh
```

To update, run the same line with the new release. The installer replaces the old version.

The installer checks the package against `SHA256SUMS` and then installs:

| What | Where |
|---|---|
| The layer and the FSR 4.1.2 assets | `~/.local/share/bng-fsr4/` |
| The Vulkan layer manifest | `~/.local/share/vulkan/implicit_layer.d/VkLayer_bng_fsr4.json` |
| The menu-label mod (optional, `--no-menu-mod` skips it) | `<BeamNG user folder>/mods/bng_fsr4_linux.zip` |

The layer stays off for every program until it is started with `BNG_FSR4=1`. Even with that variable set,
it does nothing in programs other than BeamNG.drive.

## 2. Turn it on

1. Steam → BeamNG.drive → Properties → Launch Options: `BNG_FSR4=1 %command%`
2. In the game: Options → Graphics → Anti-aliasing: **FSR 3 (custom)**. With the menu-label mod enabled,
   this entry reads **FSR 4.1.2 (Linux)** while the layer runs. Pick any render scale; FSR 4 replaces
   FSR 3's output at all of them.
3. The game's Sharpness slider also controls FSR 4's sharpening.

If the game turned your mods off after an update, the menu-label mod is off too. Turn mods back on in the
game's mod manager. FSR 4 itself does not need the mod.

Steam starts the native game inside the Steam Linux Runtime (sniper) container. The layer loads there next to
the Steam overlay and Fossilize layers. Keep the install under your home folder (the default) so that the
container can see it. On the launch after the first FSR 4 run, Steam may show "Processing Vulkan shaders"
for up to about a minute. This happens once, and you can skip it.

Launching without Steam works too:
`cd ~/.local/share/Steam/steamapps/common/BeamNG.drive && BNG_FSR4=1 SteamAppId=284160 ./BinLinux/BeamNG.drive.x64`

## 3. Check that it works

`~/.cache/bng-fsr4/events.log` shows `FSR4: backend constructed`, then
`FSR4: frames replaced N, fail-open M … hard 0` every 600 frames. `fail-open` counts frames that FSR 3 rendered
(start-up, resets, other modes); `hard` should stay 0.

## Options

| Variable | Effect |
|---|---|
| `BNG_FSR4=1` | turn the layer on (required) |
| `BNG_FSR4_SHARPNESS=0.5` | fixed FSR 4 sharpness (0–1) instead of the game's slider |
| `BNG_FSR4_NO_SHARPEN=1` | no FSR 4 sharpening |
| `BNG_FSR4_KEEP_FSR3=1` | keep running FSR 3's passes under FSR 4 (about 1.2 ms slower; for comparison) |
| `BNG_FSR4_VARIANT=generic` / `navi2` | INT8 kernels to use. Default: `navi2` on Radeon RX 6000 (Navi 2x), `generic` on other GPUs |
| `BNG_FSR4_DISABLE=1` | force the layer off |
| `BNG_FSR4_USERFOLDER=…` | BeamNG user folder, if it is not `~/.local/share/BeamNG/BeamNG.drive/current` (used for the menu-label status) |

While playing, files in `~/.cache/bng-fsr4/` switch modes:

| File | Effect |
|---|---|
| `fsr4.disable` (create) | switch to the game's FSR 3 (after a 32-frame handover); delete it to switch back |
| `fsr3skip.disable` (create) | keep FSR 3's passes running under FSR 4 |

The layer checks these files every 8 frames.

## Uninstall

```sh
~/.local/share/bng-fsr4/uninstall.sh
```

Then remove `BNG_FSR4=1` from the launch options.

## Building from source

From the repository: `git clone --recursive`, then `./build.sh` (layer) or `./package.sh` (release tarball).
The pinned Vulkan-Headers come in as a submodule.

The release package also contains the complete source in `source/`:

```sh
cd source
VULKAN_HEADERS=/path/to/Vulkan-Headers/include ./build.sh    # needs g++ (C++20); headers new enough for VK_VALVE_shader_mixed_float_dot_product
```

Development switches (`FSR4STUDY_*`, read at start-up): `FSR4STUDY_DIAG=1` turns on the full diagnostic layer
(detailed log, command-stream capture, shader dumps), and `FSR4STUDY_TIME_CHAIN=1` logs the GPU time of the upscale block.
