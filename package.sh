#!/usr/bin/env bash
# Builds dist/beamng-fsr4-linux-<version>.tar.gz plus an unversioned copy (the "latest" download link): the layer,
# the FSR 4.1.2 assets, the menu-label BeamNG mod, installer, docs, licences and the complete layer source.
set -euo pipefail
cd "$(dirname "$0")"
version=${1:-$(cat VERSION)}
name=beamng-fsr4-linux-$version
out=dist/$name

rm -rf "$out" "dist/$name.tar.gz"
mkdir -p "$out/docs" "$out/source/layer" "$out/source/fsr411"
./build.sh >/dev/null
install -m 0755 build/libVkLayer_bng_fsr4.so "$out/"
install -m 0644 packaging/VkLayer_bng_fsr4.json.in "$out/"
install -m 0755 packaging/install.sh packaging/uninstall.sh "$out/"
cp -r assets "$out/assets"
(cd game-mod/bng_fsr4_linux && zip -qrX "../../$out/bng_fsr4_linux.zip" lua scripts locales)
cp README.md LICENSE "$out/"
cp docs/*.md "$out/docs/"
cp -r notices "$out/notices"
cp layer/bng_fsr4_layer.cpp "$out/source/layer/"
cp fsr411/fsr411.cpp fsr411/fsr411.h "$out/source/fsr411/"
install -m 0755 build.sh "$out/source/build.sh"
echo "$version" > "$out/VERSION"
(cd "$out" && find . -type f ! -name SHA256SUMS -printf '%P\0' | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar -C dist --owner=0 --group=0 --sort=name -czf "dist/$name.tar.gz" "$name"
cp "dist/$name.tar.gz" dist/beamng-fsr4-linux.tar.gz
echo "dist/$name.tar.gz"
