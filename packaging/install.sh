#!/usr/bin/env bash
# Installs BeamNG FSR 4 for Linux for the current user. Nothing changes for any program until BeamNG is
# started with BNG_FSR4=1 (Steam: Properties > Launch Options: BNG_FSR4=1 %command%).
set -euo pipefail
cd "$(dirname "$0")"

usage() {
    echo "usage: ./install.sh [--no-menu-mod]"
    echo "  --no-menu-mod  do not copy bng_fsr4_linux.zip (menu label) into BeamNG's mods folder"
}
menu=1
for arg in "$@"; do
    case $arg in
        --no-menu-mod) menu=0 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

data=${XDG_DATA_HOME:-$HOME/.local/share}
dest=$data/bng-fsr4
manifests=$data/vulkan/implicit_layer.d
userfolder=${BNG_FSR4_USERFOLDER:-$data/BeamNG/BeamNG.drive/current}

if ! sha256sum --quiet -c SHA256SUMS; then
    echo "The package is damaged (SHA256SUMS does not match). Download it again." >&2
    exit 1
fi

mkdir -p "$dest" "$manifests"
rm -rf "$dest/assets"
cp -r assets "$dest/assets"
install -m 0755 libVkLayer_bng_fsr4.so "$dest/libVkLayer_bng_fsr4.so"
install -m 0755 uninstall.sh "$dest/uninstall.sh"
sed "s|@LIBRARY@|$dest/libVkLayer_bng_fsr4.so|" VkLayer_bng_fsr4.json.in > "$manifests/VkLayer_bng_fsr4.json"
echo "Installed the layer:   $dest"
echo "Installed the manifest: $manifests/VkLayer_bng_fsr4.json"

if [[ $menu == 1 ]]; then
    if [[ -d $userfolder/mods ]]; then
        cp bng_fsr4_linux.zip "$userfolder/mods/bng_fsr4_linux.zip"
        echo "Installed the menu label mod: $userfolder/mods/bng_fsr4_linux.zip"
    else
        echo "BeamNG user folder not found ($userfolder); skipped the menu label mod."
        echo "Copy bng_fsr4_linux.zip into your BeamNG mods folder by hand, or set BNG_FSR4_USERFOLDER."
    fi
fi

cat <<'MSG'

Next steps:
  1. Steam > BeamNG.drive > Properties > Launch Options:   BNG_FSR4=1 %command%
  2. In the game: Options > Graphics > Anti-aliasing: "FSR 3 (custom)"
     (shown as "FSR 4.1.1 (Linux)" while the layer runs and the menu label mod is enabled).
  3. The log is in ~/.cache/bng-fsr4/events.log. Look for "FSR4: frames replaced".
To remove everything: ~/.local/share/bng-fsr4/uninstall.sh
MSG
