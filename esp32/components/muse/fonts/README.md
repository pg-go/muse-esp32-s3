# Fonts

`muse_font_cjk_16.c` is the CJK fallback for the caption font, built in only
with `CONFIG_MUSE_CJK_FONT`. It holds GNU Unifont 16.0.04's 16x16 bitmaps,
the same cell as unscii-16, for CJK punctuation, kana, every CJK Unified
Ideograph (U+4E00 to U+9FFF) and the fullwidth forms: about 850 KB of flash.
`tools/muse/gen_cjk_font.sh` regenerates it.

GNU Unifont is by Roman Czyborra, Paul Hardy and contributors
(https://unifoundry.com/unifont/). Its compiled fonts are licensed under the
SIL Open Font License, version 1.1 (https://openfontlicense.org), and under
the GNU GPL version 2 or later with the GNU font embedding exception. This
file is a conversion of an unaltered subset of the font.

`muse_font_noto_sc_20.c` is the chat page's Chinese font only, also built in
with `CONFIG_MUSE_CJK_FONT`. It holds Noto Sans CJK SC at 20 px, 4 bpp, for
general punctuation (including U+2026 …, U+2014 —, U+201C/U+201D “”),
CJK punctuation, kana, fullwidth forms and the GB2312 level-1 common
characters (3755): about 730 KB of bitmaps, uncompressed. Characters outside
that set fall back to `muse_font_cjk_16` at runtime.
`tools/muse/gen_noto_sc_chat_font.sh` regenerates it. Noto Sans CJK is by
Google (SIL Open Font License 1.1). The font must be uncompressed
(`--no-compress`): this project builds LVGL with `LV_USE_FONT_COMPRESSED`
off, and a compressed font renders as garbled text on the device.
