#!/usr/bin/env bash
# Removes BeamNG FSR 4 for Linux for the current user. Remove BNG_FSR4=1 from the Steam launch options too.
set -euo pipefail
data=${XDG_DATA_HOME:-$HOME/.local/share}
userfolder=${BNG_FSR4_USERFOLDER:-$data/BeamNG/BeamNG.drive/current}
rm -f "$data/vulkan/implicit_layer.d/VkLayer_bng_fsr4.json"
rm -f "$userfolder/mods/bng_fsr4_linux.zip"
rm -rf "$userfolder/temp/bng_fsr4"
rm -rf "$data/bng-fsr4"
echo "Removed BeamNG FSR 4 for Linux. Remove BNG_FSR4=1 from BeamNG's Steam launch options."
