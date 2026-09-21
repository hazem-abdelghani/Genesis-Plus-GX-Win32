![Genesis Plus GX](images/Genesis-Plus-GX-Logo.png)
# Genesis Plus GX — Windows GUI

A native Windows frontend for the [Genesis Plus GX](https://github.com/ekeeke/Genesis-Plus-GX)
emulator core: Mega Drive / Genesis, Master System, Game Gear, SG-1000, Mega CD /
Sega CD and Pico, in a normal Windows program with menus, a ROM browser, and
32-bit and 64-bit builds.

This repository is a **complete, self-contained source tree**: `core/` and `sdl/`
are upstream Genesis Plus GX (unmodified, commit `27426f00aa68`, 2026-08-04),
and `win32/` is the frontend. Nothing else needs downloading to build it.

## Download

Prebuilt executables are on the **Releases** page:

| File | For |
|---|---|
| `Genesis-Plus-GX-Win32-x64` | 64-bit Windows |
| `Genesis-Plus-GX-Win32-x86` | 32-bit or 64-bit Windows |

No installer, and no extra DLLs: each needs only what ships with Windows. Put it
in a folder of your own — it creates its settings, saves, states and cheats
folders next to itself.

## Features

ROM browser (list or grid, with cover art) · Rewind · built-in render filters (Scale2x,
Scale3x, Eagle, two edge-smoothing filters, scanlines, an RGB-mask CRT filter,
sharp pixel scaling) and the core's NTSC filter · Game Genie / Action Replay
cheats · save states · Direct3D 9 or GDI video · XInput gamepads · light and dark
themes and a larger-UI mode.

Everything is described in [`win32/README.md`](win32/README.md), including what
has and has not been tested.

## Building

You need MinGW-w64 (the C compiler) and zlib. From the `win32/` folder:

```sh
make -f Makefile.win32 CROSS=x86_64-w64-mingw32-      # 64-bit
make -f Makefile.win32 CROSS=i686-w64-mingw32-        # 32-bit
```

On Windows, open an MSYS2 **MINGW64** (64-bit) or **MINGW32** (32-bit) shell and
leave `CROSS=` off. [`win32/README.md`](win32/README.md) has the package names
and every build option.

## Layout

| Folder | What it is |
|---|---|
| `core/` | Genesis Plus GX emulator core (upstream, unmodified) |
| `sdl/` | Upstream files the Windows build shares (unmodified) |
| `win32/` | The Windows frontend, and its own README |
| `LICENSE.txt`, `HISTORY.txt`, `UPSTREAM-README.md` | Upstream's files (unmodified) |

## The optional xBRZ add-on

The frontend can also offer the **xBRZ** scaler (`xBRZ 2x` to `6x` in
Video → Render Filter). It is **not in this repository** and is **off by
default**, because xBRZ is licensed under the GPLv3 and that cannot be combined
with Genesis Plus GX's non-commercial licence in one distributed program. It
lives in its own repository under the GPLv3,
[Genesis-Plus-GX-Win32-xBRZ-addon](https://github.com/hazem-abdelghani/Genesis-Plus-GX-Win32-xBRZ-addon).
To use it, clone it into `win32/xbrz` from the root of this tree:

```sh
git clone https://github.com/hazem-abdelghani/Genesis-Plus-GX-Win32-xBRZ-addon win32/xbrz
```

or download `Genesis-Plus-GX-xBRZ-addon.zip` from that repository's Releases and
unpack it inside `win32/`. Then build from `win32/` with `XBRZ=1`. Building it in
for your own use is fine; please do not distribute an executable that contains
it. The add-on's `README-xbrz.txt` has the details, and `.gitignore` lists
`win32/xbrz/` so it is not committed to this repository by accident.

## Licence

Genesis Plus GX and this frontend are under the **Genesis Plus GX licence**,
[`LICENSE.txt`](LICENSE.txt). In short: it is free to use and share, but it may
not be sold or used in a commercial product or activity; a modified version you
redistribute must come with the complete source code of everything its binary
uses (this repository is that); and the copyright notices must be kept. Read
`LICENSE.txt` itself for the actual terms; this summary is not a substitute for
it, and none of this is legal advice.

Some third-party parts carry their own licences, kept beside them:

- Nuked OPN2 (LGPL 2.1, quoted in `LICENSE.txt`) and `core/ntsc`, Blargg's NTSC
  filter (LGPL 2.1)
- `core/sound/minimp3` (CC0) and `core/sound/tremor` (BSD-style)
- `core/cd_hw/libchdr` and the lzma, zlib and zstd sources it bundles
  (see the licence files in those folders)
- `win32/stb_image.h`, which states its own licence in its header

## Credits

Genesis Plus GX is by Charles MacDonald, Eke-Eke and contributors, with portions
from Nicola Salmoria and the MAME team. xBRZ is by Zenju.
