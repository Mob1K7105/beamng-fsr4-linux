# FSR 4.1.1 for BeamNG.drive on Linux (unofficial)

Replaces the final stage of BeamNG.drive's built-in FSR upscaler with AMD FSR 4.1.1 (the machine-learning
upscaler) in the **native Linux** version of the game. It runs as a Vulkan layer: no Wine, Proton or
DirectX, and no game files are modified.

> **Status: early release (0.1.0).** Tested on one system (RX 6700 XT, Mesa RADV, BeamNG 0.40, launched from
> Steam). Other GPUs are untested; reports are welcome (see [Reporting problems](#reporting-problems)).
> Unofficial, not affiliated with or endorsed by AMD or BeamNG GmbH.

## What you get

- **The real FSR 4.1.1 network.** The shaders and weights derive from AMD's signed FSR 4.1.1 DLL
  (FidelityFX SDK 2.3.0, INT8 path). On the test GPU the Vulkan port matches AMD's DLL byte for byte, and
  the frames shown in-game match that port byte for byte. See [docs/VERIFICATION.md](docs/VERIFICATION.md).
- **Image quality instead of speed.** On RDNA 2, FSR 4 costs more GPU time than BeamNG's FSR 3. Use it
  for a cleaner image, or pair it with a lower render scale. Measured numbers are in
  [docs/PERFORMANCE.md](docs/PERFORMANCE.md).
- **Safe fallback.** If anything looks unexpected (game update, other upscaler mode, unsupported size),
  the game's own FSR 3 runs untouched.

## Requirements

| | |
|---|---|
| Game | BeamNG.drive **0.40.0.0**, native Linux build (`BinLinux/BeamNG.drive.x64`), Vulkan renderer |
| In-game setting | Anti-aliasing: **FSR 3 (custom)**, i.e. BeamNG's `fsrCustom` upscaler, at any render scale (shown as "FSR 4.1.1 (Linux)" with the menu-label mod) |
| GPU | AMD RDNA 2 or newer on Mesa RADV (tested: RX 6700 XT, Mesa 26.2.4) |
| Vulkan features | 1.3, shaderInt8, shaderIntegerDotProduct, 16-bit arithmetic, `VK_KHR_push_descriptor`, `VK_KHR_compute_shader_derivatives`; `VK_VALVE_shader_mixed_float_dot_product` used when present |
| Output size | Up to 3840×2160 |

Other GPUs and drivers are untested; see [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Install (3 steps)

**1. Download and install** (paste into a terminal, no root needed):

```sh
curl -fsSL https://github.com/Mob1K7105/beamng-fsr4-linux/releases/latest/download/beamng-fsr4-linux.tar.gz | tar xz && ./beamng-fsr4-linux-*/install.sh
```

Or download `beamng-fsr4-linux.tar.gz` from the [latest release](https://github.com/Mob1K7105/beamng-fsr4-linux/releases/latest),
unpack it and run `./install.sh` inside.

**2. Steam** → right-click BeamNG.drive → Properties → Launch Options:

```
BNG_FSR4=1 %command%
```

**3. In the game:** Options → Graphics → Anti-aliasing: **FSR 3 (custom)**, at any render scale. With the
included menu mod it reads **FSR 4.1.1 (Linux)** while FSR 4 runs. The Sharpness slider still works.

To uninstall: `~/.local/share/bng-fsr4/uninstall.sh`, then remove the launch option. Options, logs and
troubleshooting: [docs/INSTALL.md](docs/INSTALL.md).

## How it works

[docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) covers which BeamNG passes are intercepted, where FSR 4 gets
its inputs, and how jitter, resets and fallback are handled.

## Known limitations

[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).

## Reporting problems

Open an issue with your GPU, Mesa version (`vulkaninfo --summary | grep driverInfo`), BeamNG version, output
resolution, and the last `FSR4: frames replaced … hard …` lines of `~/.cache/bng-fsr4/events.log`.

## Building from source

```sh
git clone --recursive https://github.com/Mob1K7105/beamng-fsr4-linux.git
cd beamng-fsr4-linux
./build.sh      # g++ with C++20 and libvulkan; output: build/libVkLayer_bng_fsr4.so
./package.sh    # release tarball in dist/
```

## Licensing and credits

The layer links GPL-2.0-or-later code and is therefore GPL-2.0-or-later. The FSR 4.1.1 shader/model
assets are distributed under AMD's MIT exception for the upscaler DLL. Details and full credits are in
[docs/LICENSING.md](docs/LICENSING.md).
