#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may
# not use this file except in compliance with the License. You may obtain
# a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Regenerates components/muse/fonts/muse_font_noto_sc_20.c, the chat page's
# Chinese font: Noto Sans CJK SC (SIL Open Font License 1.1, free) at 20 px,
# 4 bpp antialiased, for CJK punctuation, kana, fullwidth forms and the
# GB2312 level-1 common characters (3755). Characters outside that set fall
# back to muse_font_cjk_16 at runtime, so a rare character still shows.
#
# Why a subset: the full CJK set at 20 px / 4 bpp is about 2.4 MB of bitmaps,
# more than the app slot's free space (about 1.1 MB); the common subset is
# about 430 KB of bitmaps. Needs curl, python3 and npx (Node.js).
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_noto_sc_20.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fsSL -o "$TMP/NotoSansCJKsc-Regular.otf" \
    "https://raw.githubusercontent.com/googlefonts/Noto-CJK/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
echo "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b  $TMP/NotoSansCJKsc-Regular.otf" | shasum -a 256 -c - >/dev/null

# GB2312 level-1 characters, in order, as the --symbols list.
SYMBOLS="$(python3 -c "
import sys
chars = []
for hi in range(0xB0, 0xD8):
    for lo in range(0xA1, 0xFF):
        try:
            c = bytes([hi, lo]).decode('gb2312')
        except Exception:
            continue
        if len(c) == 1 and '\u4e00' <= c <= '\u9fff':
            chars.append(c)
sys.stdout.write(''.join(chars))
")"

npx -y lv_font_conv@1.5.3 --font "$TMP/NotoSansCJKsc-Regular.otf" --size 20 --bpp 4 \
    --format lvgl --lv-font-name muse_font_noto_sc_20 --no-kerning \
    -r 0x3000-0x30FF -r 0xFF00-0xFFEF --symbols "$SYMBOLS" -o "$TMP/font.c"
# The project includes LVGL as "lvgl.h"; the header names no temp paths.
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" > "$OUT"
echo "wrote $OUT"
