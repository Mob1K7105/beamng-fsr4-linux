#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# [POSTPASS=0] apply.sh <asset set dir>   (a dir holding t1080_m0 ... t4320_m1, straight from extract.py,
#                             i.e. before this script has run on it)
#
# The exact (byte-for-byte same output) shader rewrites of
# https://github.com/zgauthier2000/fsr4-shader-performance-overrides (GPL-2.0-or-later,
# tools/ at commit c4531961), applied to the assets of this package:
#   postpass   model_tail.py, model_clamp.py, postpass_taps.py, then postpass_direct.py
#              (stores through 16 KB of workgroup memory, one image at a time; patched here to
#              accept dxil-spirv's named image bindings). Input: postpass_orig.spv.
#   pass1-12   model_clamp.py, model_tail.py where they find something to rewrite.
# POSTPASS=0 (used for assets/lite): the postpass stays AMD's; postpass_direct.py measured 2% slower
# on FSR 4.1.2 Lite's postpass on an RX 6700 XT (3.19 -> 3.25 ms at 5120x1440), postpass_lds.py 9%.
# Not applied: prepass_route.py (FSR 4.1.1's prepass; 4.1.2's is a different shader),
# zconst.py/pass11_stores.py (the Navi2 set already has AMD's newer pass 11),
# postpass_sharetaps.py (needs more registers; does not match this postpass).
# Measured on an RX 6700 XT, navi2 set: 2.59 -> 2.40 ms at 1707x960 -> 2560x1440,
# 5.12 -> 4.75 ms at 2970x835 -> 5120x1440; output byte-identical (docs/VERIFICATION.md).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT
for tier in "$1"/t*_m*; do
    if [[ ${POSTPASS:-1} == 1 ]]; then
        spirv-dis "$tier/postpass_orig.spv" -o "$w/pp"
        python3 -I "$here/model_tail.py" < "$w/pp" 2>/dev/null | python3 -I "$here/model_clamp.py" 2>/dev/null \
            | python3 -I "$here/postpass_taps.py" 2>/dev/null | python3 -I "$here/postpass_direct.py" > "$w/pp2"
        spirv-as --target-env spv1.3 "$w/pp2" -o "$tier/postpass.spv"
        spirv-val --target-env vulkan1.3 "$tier/postpass.spv"
    fi
    for f in "$tier"/pass{1,2,3,4,5,6,7,8,9,10,11,12}.spv; do
        [[ -f $f ]] || continue
        spirv-dis "$f" -o "$w/m"
        python3 -I "$here/model_clamp.py" < "$w/m" 2>/dev/null | python3 -I "$here/model_tail.py" > "$w/m2" 2>/dev/null
        cmp -s "$w/m" "$w/m2" && continue
        spirv-as --target-env spv1.3 "$w/m2" -o "$f"
        spirv-val --target-env vulkan1.3 "$f"
    done
    echo "$(basename "$tier"): rewritten"
done
