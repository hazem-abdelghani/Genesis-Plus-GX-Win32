# Genesis Plus GX — Windows GUI

A native Win32 frontend for [Genesis Plus GX](https://github.com/ekeeke/genesis-plus-gx),
built directly against the emulator core. It sits alongside the upstream `sdl/`
port as a new `win32/` port, and this repository includes the `core/` and
`sdl/` it builds against, so nothing else needs downloading.

No SDL. Video goes through GDI, sound through waveOut, gamepads through XInput.
The finished executable depends only on DLLs that ship with Windows.

Supports everything the core does: Mega Drive / Genesis, Master System,
Game Gear, SG-1000, Mega CD / Sega CD, and Pico.

---

## Building

You need MinGW-w64 (the C compiler) and zlib. Nothing else. (The optional xBRZ
filters, described under **Render Filters**, also need its C++ compiler, `g++`.)

**It builds as either 32-bit or 64-bit**, depending on which compiler you point
it at. The render filters are built into the executable, so nothing depends on
the pointer size. (An earlier version loaded Kega Fusion `.rpi` plugin DLLs,
which are 32-bit and cannot be loaded into a 64-bit process; that was the only
reason it used to be 32-bit only.)

**On Windows, from an MSYS2 shell** — the shell you open decides the
architecture:

```sh
# 64-bit: open the "MSYS2 MINGW64" shell
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-zlib make
# 32-bit: open the "MSYS2 MINGW32" shell
pacman -S mingw-w64-i686-gcc mingw-w64-i686-zlib make

cd win32
make -f Makefile.win32
```

**Cross-compiling from Linux:**

```sh
sudo apt install mingw-w64 libz-mingw-w64-dev make
cd win32
make -f Makefile.win32 CROSS=x86_64-w64-mingw32-     # 64-bit
make -f Makefile.win32 CROSS=i686-w64-mingw32-        # 32-bit
```

Either way you get `gpgx.exe`. To keep both builds side by side, give each its
own object directory and name:

```sh
make -f Makefile.win32 CROSS=x86_64-w64-mingw32- OBJDIR=./build_w64 NAME=gpgx64.exe
```

### Build options

| Option | Effect |
|---|---|
| `DEBUG=1` | Keep symbols, disable optimisation |
| `CHD=0` | Drop `.chd` support (skips libchdr and its lzma/zstd deps) |
| `OGG=0` | Drop OGG CD audio (skips the bundled Tremor decoder) |
| `XBRZ=1` | Build in the xBRZ render filters. **Off by default**, and needs the separate xBRZ add-on (GPLv3) unpacked into `win32/xbrz/` plus a C++ compiler; see **Render Filters** below |

`CHD` and `OGG` are on by default and build from sources already in the
repository, so neither adds an external dependency. Turning them off roughly
halves the build time if you only care about cartridge games. `XBRZ` is the
reverse: off unless you ask for it.

---

## Installing

Put `gpgx.exe` wherever you like. On first run it creates its own folders next
to itself:

```
gpgx.exe
gpgx.ini            settings
bios/               optional BIOS and add-on ROMs
saves/              battery saves (.srm) and Mega CD backup RAM (.brm)
states/             save states
screenshots/        PNG captures
cheats/             per-ROM cheat lists (.cht)
```

Everything is relative to the executable, so the whole folder can be copied to
a USB stick and it will keep working.

### Optional ROMs

Drop these in `bios/` if you have them. All are optional; games that do not
need them run without.

| File | Used for |
|---|---|
| `bios_MD.bin` | Mega Drive TMSS boot ROM |
| `bios_CD_U.bin`, `bios_CD_E.bin`, `bios_CD_J.bin` | Mega CD, by region |
| `bios_U.sms`, `bios_E.sms`, `bios_J.sms`, `bios.gg` | Master System / Game Gear |
| `ggenie.bin`, `areplay.bin` | Game Genie, Action Replay |
| `sk.bin`, `sk2chip.bin` | Sonic & Knuckles lock-on |

Turn the BIOS on under **Emulation → Boot from BIOS when available**.

---

## Using it

Open a ROM with **File → Open**, drag one onto the window, pass it on the
command line, or browse a folder with **File → ROM Browser** (Ctrl+B). ZIP
and GZ archives work directly. The window remembers its position, size and
maximized state between runs.

### Keyboard

| | |
|---|---|
| Arrow keys | D-pad |
| A, S, D | A, B, C |
| Q, W, E | X, Y, Z |
| Enter | Start |
| Right Shift | Mode |

A connected gamepad works with no setup, laid out the way a 6-button Control
Pad expects: face buttons give B and C, X gives A, the shoulders give X and Z.
Change any of it under **Input → Configure Player**.

### Shortcuts

| | |
|---|---|
| Ctrl+O / Ctrl+W | Open / close ROM |
| Ctrl+B | ROM Browser |
| Ctrl+R / Ctrl+Shift+R | Reset / hard reset |
| F2 | Pause |
| F5 / F8 | Save / load current slot |
| F6 / F7 | Previous / next slot |
| F11 | Screenshot |
| Alt+Enter / Esc | Enter / leave fullscreen |
| Tab (held) | Fast forward |
| `\` | Advance one frame while paused |

Assigning a control works by listening rather than by picking from a list:
select a button, choose Assign, then press what you want it to be. The same
pass catches keyboard keys and gamepad buttons, so there is no separate mode
for controllers.

### Cheats

**Emulation → Cheats...** opens a list where you can paste Game Genie or
Action Replay codes, tick them on and off, and give each one a name. Codes are
saved per ROM under `cheats/<name>.cht` and reload automatically the next time
that ROM runs.

**Enable Cheats** is off until you turn it on. Select a code in the list to see
its name and code in the fields underneath; change them and press **Save** to
update that entry where it is (it keeps its place in the list). **Add code**
adds a new one. The same list opens from **Edit Cheats** on a ROM's right-click
menu in the browser, without starting the game, and is saved the same way.

Accepted formats depend on the console, same as the codes you'd find in any
cheat database:

| Console | Formats |
|---|---|
| Mega Drive / Genesis | Game Genie (`ABCD-EFGH`), raw patch (`aaaaaa:dddd`) |
| Master System / Game Gear / SG-1000 | Game Genie (`ABC-DEF`, or `ABC-DEF-GHI` with a reference byte), Action Replay (`00XX-YYYY`), raw RAM (`aaaa:dd`) |

Toggling a code takes effect immediately, with no need to reset.

### ROM Browser

**File → ROM Browser** (Ctrl+B) scans a folder for ROMs and lists them —
double-click one, or select it and press **Play**, to launch it. **Change...**
picks a different folder; **Refresh** rescans after adding files. The scan
goes into subfolders (six levels deep, capped at 4000 files, generous for how
anyone actually organises a collection) and recognises the same file types
the Open dialog does. A standalone `.bin` is hidden when a `.cue` with the
same name sits next to it, since the `.cue` is the real entry point for that
Mega CD dump.

### Render Filters

**Video → Render Filter** applies a CPU pixel filter to the game image before
it is scaled to the window. They are built into the executable, so there is
nothing to install and nothing to load:

| Filter | Output | What it does |
|---|---|---|
| Scale2x | 2x | AdvMAME's pixel-art scaler; smooths diagonals, keeps everything else |
| Scale3x | 3x | The same idea at 3x, smoother still |
| Eagle 2x | 2x | The classic Eagle rules; tends to thin single-pixel lines |
| Smooth 2x (xBR-style) | 2x | Edge-directed corner smoothing; see below |
| Smooth 4x (xBR-style) | 4x | Two passes of the above; the softest, roundest result |
| **xBRZ 2x – 6x** *(optional add-on)* | 2x – 6x | **The real xBRZ.** Anti-aliased, almost vector-like curves and clean thin lines; only in builds made with the add-on, see below |
| Scanlines 2x | 2x | Every second row dimmed |
| CRT 3x (RGB mask) | 3x | Red/green/blue phosphor stripes plus scanlines, about 80% of source brightness |
| Sharp 4x | 4x | Plain pixel replication, no smoothing. With **Smooth scaling** on this is the well-known "sharp bilinear" look: crisp pixels without shimmer at non-integer window sizes |

Each filter's output size is fixed by the filter itself; the final resize to
the window is done afterwards through Direct3D or `StretchBlt` and follows your
**Smooth scaling** setting. A filter is mutually exclusive with the built-in
NTSC filter — turning one on turns the other off — since running one image
reshaper after the other is a combination nothing here has examined.

**xBRZ (optional add-on).** xBRZ is not part of this repository. It is GPLv3,
so it comes as a separate add-on (the top-level `README.md` says where to get it)
that you unpack inside `win32/` (giving `win32/xbrz/xbrz.cpp`) and enable with `XBRZ=1`; without it the menu simply
has the other eight filters. With the add-on, this is Zenju's actual xBRZ
code, not a lookalike, at its default
settings — the same algorithm the xBRZ render plugins for Kega Fusion were
built from. It is the best-looking filter here on curves, diagonals and thin
lines, and it is anti-aliased: unlike Scale2x or Smooth it blends colours, so
the result is softer, and very small details (a one-pixel eye) come out a touch
dimmer than the source. Higher multipliers give smoother curves; 3x or 4x is a
good everyday choice, 6x is for large displays.

Things to know about it:

- **Memory.** xBRZ keeps a 64 MB colour-distance table. It is built when you
  select an xBRZ filter (about a tenth of a second) and freed when you switch
  to anything else, so it costs nothing unless you are using it. Expect about
  80-100 MB more while it is active, more at 5x/6x on tall interlaced frames.
- **Cost.** About 1 ms per frame at 2x, 2 ms at 4x, under 4 ms at 6x on a
  320x224 frame; up to about 9 ms for a 6x interlaced frame. Frames that are
  nearly all noise, which real games do not produce, take up to 10-30 ms.
- **Direct3D texture size.** 6x of an interlaced frame is a 1920x2688 texture.
  Almost all GPUs accept that; one that does not falls back to the slower GDI
  path for those frames rather than showing nothing.
- **Fidelity.** The port was checked to give bit-for-bit the same output as
  the untouched original at every factor from 2x to 6x; the only step added is
  rounding its 8-bit result back to the 5/6/5 bits the frame buffer holds.
- **Licence.** xBRZ is GPLv3, which is not compatible with Genesis Plus GX's
  non-commercial licence. That is why it is a separate add-on and not part of
  this repository, and why the prebuilt executables published with it do not
  contain it. Building it in for your own use is fine; **do not distribute an
  executable that contains it.** Full details are in the add-on's
  `xbrz/README-xbrz.txt`.

**What "Smooth (xBR-style)" is, and is not.** It is not hqx and it is not
xBRZ. It is a compact, independently written edge-directed smoother built on
the same idea as xBR: for every corner of every source pixel, compare how much
colour changes along the two diagonals through it, and if the edge runs
across that corner, blend the corner toward the neighbour it belongs to.
It is cruder than xBRZ but crisper (it does not soften small details), much
cheaper, and — unlike xBRZ — carries no licence restriction. Two rules keep it
from damaging detail: a corner is only cut when the pixel actually continues
away from it (so single-pixel eyes, letter dots and 1-pixel lines are left
alone — an early version without this rule smudged them), and similarity uses
a tolerance so dither patterns and flat areas are untouched.

**Provenance.** Every filter except xBRZ is written from the published
algorithm descriptions, not derived from another project's source. xBRZ is
third-party GPLv3 code (the add-on's `xbrz/`), lightly modified for memory
handling only; what was changed is listed in its `README-xbrz.txt` and marked
in the files.

**Cost of the others.** Measured on a desktop CPU with an optimised build, on
320x224 frames: Scale2x, Eagle, Scanlines and Sharp are well under 1 ms; Scale3x
and CRT are under 1 ms; Smooth 2x is about 1 ms; Smooth 4x is about 3.5 ms
(about 7 ms on 320x448 interlaced frames). A frame of pure random noise, which
no game produces, takes up to 14 ms at Smooth 4x.

**Upgrading from the `.rpi` build.** `.rpi` files are no longer used, the
`filters\` folder is no longer created, and **Filter Scale** and **Rescan
filters folder** are gone (the optional xBRZ add-on comes in fixed 2x-6x entries instead). If
your settings file names a plugin that happens to match a built-in
("Scale2x.rpi"), it carries over; anything else falls back to no filter. The
obsolete `rpi_filter` / `rpi_scale` keys are removed the next time settings
are saved.

**Adding your own.** A filter is one function in `filters.c` —
`void f(const uint16_t *src, int src_pitch, int w, int h, uint16_t *dst, int dst_pitch)`
producing exactly `scale` times as many pixels each way — plus one line in the
table at the bottom of the file. The menu is built from that table, and
`filters.c` has no Windows dependency, so it can be built and tested on its own.
`tests/filters_test.c` does exactly that — no Windows and no emulator core
needed; the build commands are at the top of the file.

---

## How it fits together

| File | Role |
|---|---|
| `osd.h`, `main.h`, `config.h` | What the core expects from the host |
| `main.c` | Window, menus, ROM lifecycle, emulation loop |
| `video.c` | DIB rendering, scaling, fullscreen, screenshots, filter output buffer |
| `filters.c`, `filters.h` | The built-in render filters (no Windows dependency) |
| `xbrz/` *(add-on, not in this repository)* | Zenju's xBRZ scaler (GPLv3, third-party) plus `xbrz_glue.cpp`/`.h` (RGB565 <-> xBRZ pixel conversion), with licence and provenance notes; see **Render Filters** |
| `audio.c` | waveOut output |
| `input.c` | Keyboard, XInput, pointer devices |
| `cheats.c` | Game Genie / Action Replay decoding, patching, persistence |
| `dialogs.c` | Input mapping, audio levels, shortcuts, about |
| `browser.c` | ROM Browser: folder scan and launch |
| `config.c` | Defaults and INI persistence |
| `gpgx.rc`, `resource.h`, `gpgx.manifest` | Menus, accelerators, dialogs, icon, visual styles |

`fileio.c`, `unzip.c` and `error.c` are reused from `../sdl` rather than
duplicated — they are platform-neutral. The include path puts `win32/` ahead of
`sdl/` so this port's `osd.h` is the one the core sees.

### Decisions worth knowing about

**16-bit RGB565, not 32-bit.** The core's Blargg NTSC filter only supports 15-
and 16-bit output, so a 32-bit framebuffer would have meant giving up the
filter. Frames land in a top-down DIB section with bitfield masks and are
presented with a single `StretchBlt` from a memory DC. `StretchDIBits` would
have been one call shorter but it flips the meaning of its source rectangle
depending on the sign of `biHeight`, which is not worth the ambiguity.

**Single-threaded.** Emulation runs on the UI thread from a `PeekMessage` loop.
That costs a little responsiveness while a menu is open — Windows runs its own
modal loop there, which the code handles explicitly — but it means no locking
anywhere near the framebuffer or the core's state.

**The sound card is the clock.** One buffer is queued per emulated frame and
buffers are recycled by polling `WHDR_DONE` rather than through a callback, so
nothing runs on a second thread. The number of buffers still in flight is what
paces emulation, which keeps video locked to the audio clock instead of letting
the two drift. With sound off it falls back to `QueryPerformanceCounter`
against the core's own reported frame rate.

**BIOS paths resolve against the executable.** The SDL port hardcodes
`"./ggenie.bin"`, which breaks the moment you launch from a shortcut or drop a
ROM on the .exe. These resolve relative to the executable instead.

**XInput is loaded at runtime.** Linking it directly would stop the executable
from starting on a machine where the DLL is missing, and a gamepad is optional.
Controller slots 3 and 4 get the default layout automatically, so a Team Player
game works without extra setup.

**Render filters are built in, not plugins.** They are ordinary functions in
the executable, working on the same RGB565 buffer the core renders into and
writing to a second one that is presented in its place. That removes a whole
class of problems the earlier plugin loader had to defend against: no DLL to
find or trust, no calling-convention or struct-layout guesses, no crash guard
needed, and no restriction on the executable's bitness.

**xBRZ is C++ but the program is not.** With the add-on, xBRZ is compiled with no exceptions, no
RTTI and no thread-safe static guards, and its one `std::vector` became a
`malloc`, so the object code calls nothing in the C++ runtime library. The link
step stays plain `gcc`, and the executable still imports only DLLs that ship
with Windows (it does not need `libstdc++` or `libwinpthread`, whichever MinGW
threading model your toolchain uses).

---

## Verified, and not

Both the 32-bit and the 64-bit build compile and link with no warnings, with
and without the xBRZ add-on, and `file` confirms each is the architecture it claims
to be. The 64-bit executable imports only DLLs that ship with Windows.

**The filters themselves** were tested on their own, outside the emulator:
under AddressSanitizer and UBSan on twelve frame sizes (down to 1x1, up to the
720x576 maximum) and four image patterns each, all thirteen filters including
xBRZ 6x; checked so that flat input stays flat, the input frame is never
modified, every output pixel is written, and the colour-blend arithmetic stays
within one least-significant bit of a floating-point reference; and reviewed by
eye on pixel-art test scenes, which is what caught the eroded single pixels
and the over-dark CRT setting.

**The xBRZ add-on** was checked three ways. The patched xBRZ was compared
with the untouched original built from the same source, bit for bit, at every
factor from 2x to 6x on pixel art, random noise and degenerate sizes (25
comparisons, all identical). The complete frontend path (RGB565 in, xBRZ,
rounding back out) was compared with the original xBRZ plus an independent
conversion written separately in Python (15 comparisons, all identical). And
every one of the 65,536 possible RGB565 colours survives the pack/unpack round
trip unchanged.

**Both builds have been run under Wine (the 32-bit one under a 32-bit Wine), though not on real Windows.** With
a small synthetic test ROM (a hand-written 68000 program that draws a
pixel-art scene through the VDP), each of the thirteen filters was selected
through the settings file, the emulator was run fullscreen on a virtual
display sized to exactly that filter's output, and the screen was captured.
Recovering the game's own frame from an unfiltered capture and running the
filter code on it reproduced the emulator's on-screen pixels exactly — zero
differing pixels, across all thirteen filters. The menu was also driven with
real clicks: the Render Filter submenu lists the filters in groups and checks
the active one, choosing a filter saves it, turning NTSC on clears the filter
and choosing a filter turns NTSC off, a settings file from the old `.rpi` build
migrates as described above, and the process's memory rises by about 80 MB when
xBRZ is selected and falls again when another filter is chosen.

**Not tested:** real Windows
in any form, and the test ROM is synthetic — the filters have been judged on a
purpose-made pixel-art scene, not on a library of real commercial games. Also
untouched by any of the above: actual audio hardware (this sandbox has none, so
waveOut pacing has not been exercised against a real sound card — that is
still the part most likely to need tuning; if sound crackles, raise
**Audio → Levels and Latency → Buffered frames**), real GPU and display
drivers (Wine's Direct3D stands in for the real thing), real multi-monitor
setups for the window-position feature, and real XInput controller hardware.

## Known gaps

- Multitap input is mapped for four players via XInput, but only players 1 and
  2 have configurable keyboard bindings.
- No debugger, no netplay.
- No rewind.
- Render filters and the NTSC filter are mutually exclusive, not combinable.
- No hqx, 2xSaI or Super Eagle. (xBRZ is available as an add-on; the "Smooth (xBR-style)" filters are a separate, simpler design and not hqx.)
- xBRZ is not in this repository, and cannot be part of a distributed binary, because of its GPLv3 licence; see **Render Filters**.

## Licence

Same terms as Genesis Plus GX: source must accompany modified redistributions,
and it may not be sold or used commercially. See `LICENSE.txt` in the
repository root. `stb_image.h` carries its own licence in its header. The
optional xBRZ add-on is separately licensed (GPLv3); see **Render Filters**.
