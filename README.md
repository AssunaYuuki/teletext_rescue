# Teletext Rescue

Recovers teletext, programme guides, captions and other data hidden in the vertical blanking
interval (VBI) of old video recordings — and shows it the way viewers saw it in the 1980s–90s.

Open a raw VBI capture and the program works out the rest by itself: the capture chip and
format, the colour system, what every VBI line carries, and which decoders to run. Everything
readable is decoded and opened, each service in its own window.

## What it reads

| Service | What you get |
|---|---|
| **WST teletext** (625 lines, 525-line NTSC, DVB `.ts`) | pages as on a TV (Level 1), page editor, clean pages, word restoration, export to `.t42` + HTML, subtitles `.srt` |
| **Japanese teletext** (文字多重放送, JTES / BT.653 system D) | pages exactly as the tuner drew them: kanji and kana (TU-TX100 fonts), photographic pictures, smooth mosaics, DRCS, animations, and the tunes they play |
| **NABTS / NAPLPS** (CBS ExtraVision, NBC Teletext) | NAPLPS pages drawn as the receiver drew them, in time; HTML export |
| **StarSight** programme guide | the guide grid with titles and descriptions (Huffman-compressed text decoded) |
| **Gemstar GUIDE Plus+** | programme table from the guide blocks; packet table |
| **CC captions and XDS** (line 21, NTSC and PAL) | caption text; station, network, time of day, programme, rating |
| **AMOL** | source ID and broadcast clock |
| **Silent Radio** (LED news-sign service) | the sign plays inside the program; all texts |
| **TV-Inform** (Russia, Channel 1) and **Interactive Network** | packets and their decoded records |
| **Datacasts** (5.73 Mbit/s) | packets voted from their copies, streams and carousel |
| **Test signals (VITS)**, **VPS / WSS** | what the recording carries on each line |

Capture chips: bt8x8 (NTSC and PAL), cx88, saa713x, cx23885, ivtv/cx18, em28xx, 16-bit 4fsc
`.tbc` from vhs-decode; FLAC-compressed captures (`.vbi.flac`, `.flac`); streams `.t42`, `.t34`,
`.t33`, DVB `.ts`.

Signals worn by VHS are read with MLSE (Viterbi) decoders that fit the tape channel by
themselves. Decoding runs on the graphics card through OpenCL — NVIDIA, AMD or Intel — or on the
processor.

## Installing and running

**Windows 10/11:** run `TeletextRescue-Setup-v1.3.exe` (64-bit) or `TeletextRescue-Setup-v1.3-x86.exe`
(32-bit), or unpack the zip of the same name. Nothing else is needed.

**File → Open .vbi recording…** — the recording is examined and decoded; the results are saved in one
folder `<name>_vbi` next to the recording (the recording itself is never changed), so opening it
again is instant. The start screen lists what was found; every service opens in its own window,
and **Export everything decoded…** writes all results into a folder.

**Linux** (x86_64, ARM64, x86) and **macOS** (Apple Silicon and Intel): unpack the archive and run
`TeletextRescue` (on macOS `Teletext Rescue.app`) — the same program with the same windows. On macOS,
the first time: right click → Open, or `xattr -dr com.apple.quarantine "Teletext Rescue.app" trcli`.

**Command line** (all systems): `trcli`, the same decoders without windows:

```
trcli recording.vbi                     # decode, print the report
trcli recording.vbi --export out        # write everything decoded into the folder out
trcli recording.vbi --cpu               # on the processor (--gpu: on the graphics card)
trcli recording.vbi --again             # decode again instead of opening the saved results
```

## What is new in 1.3

- **Capture card, live** (Windows): the picture, every VBI line labelled with what it carries, teletext pages
  as they arrive, recording to `.vbi` + `.t42`; Behold (SAA7231), BT848, AVerTV (SAA7134).
- **Found files and texts**: files sent in the signal are put together and checked. **Бизнес-Текст**
  (NTV, 1994-96): the catalogue, the cipher of both years broken, files decrypted; every file shows how
  much of it was read (%) and whether it checks — ARJ archives are unpacked in memory and every CRC-32
  compared (a damaged archive, which other unpackers refuse, is still unpacked as far as it goes), PCX pictures
  decoded line by line. **TV-Inform** (Channel 1, RTR): Norpak and fT3 data
  channels, files (a Hydromet fax map, newspaper PDFs), WMO bulletins, the inserter's texts.
- **Japanese teletext** from 27 MHz captures (GV-USB2, Hauppauge) as JTES Decoder reads them, on the
  processor or the graphics card; mosaics drawn exactly as the Panasonic TU-TX100 draws them (STL
  underline, combined mosaics); the tape channel chosen per recording.
- Teletext: Clean pages and the VHSTTX squash, VBI repair, more character sets (Polish, Turkish,
  Baltic, Greek, Hebrew), subpage rotations, pencil drawing in the page editor.
- VITC and the line-6 time code; Gemstar GUIDE Plus+ window; StarSight, Gemstar, VITC in live.
- **Linux and macOS**: every window of the program is a window of its own on the desktop, as on Windows.
- **32-bit builds**: Windows x86 and Linux x86 next to the 64-bit ones.

## What is new in 1.2

- **Japanese teletext (JTES)**: the whole service — 1400+ pages from a 1996 NHK recording.
  Errors are corrected with the majority-logic decoder of the (272,190) difference-set code
  (up to 8 bits per line), every page is assembled from all its repeated copies and checked by
  CRC. Photographic pictures (logos, illustrations), smooth mosaics, double-size characters,
  DRCS, the page raster colours, animated pages (played in the window, exported frame by frame)
  and tunes (played, exported as WAV).
- **StarSight**: titles and descriptions decoded (the Huffman table recovered), guide grid.
- **Gemstar GUIDE Plus+**: guide blocks decrypted and decompressed into a programme table.
- **TV-Inform** (Channel 1, Russia), **Interactive Network** 2D service, **AMOL** text frames,
  CBS clock in XDS, VPS / WSS, SECAM / PAL / NTSC colour of the VBI lines.
- Captions on any line, also from PAL tapes; PAL teletext from bttv captures at 35.47 MHz.
- A VBI line viewer: waveform, spectrum, eye diagram and the colour as on a TV.
- Saved results: a recording decoded once opens again without decoding.
- New program window design: a window per service, one export for everything.
- **Linux and macOS get the program with windows** (before: the command line only).

## Building

**Windows** — MinGW-w64 g++, CMake and Ninja: `build.bat` (the program and `trcli`);
`build.bat setup` also makes the installer with Inno Setup 6.

**Linux and macOS** — the program with windows (SDL3 + Dear ImGui) and `trcli`:

```
sh build.sh
cpp/build-linux/TeletextRescue           # cpp/build-darwin/TeletextRescue.app on macOS
```

Needs a C++20 compiler (g++ 10+, clang 12+ / Xcode 13+), CMake 3.20+ and Git (CMake fetches SDL3);
on Linux the X11 / Wayland development packages (Debian/Ubuntu: `libx11-dev libxext-dev libxrandr-dev
libxcursor-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libegl1-mesa-dev libasound2-dev
libpulse-dev`). The graphics card is used through OpenCL when it is there (Linux: `ocl-icd` plus the
card's driver; macOS: built in), otherwise the processor. Ready-made builds for Linux (x86_64,
ARM64) and macOS (one app for Apple Silicon and Intel) are attached to each release (built by
GitHub Actions, `.github/workflows/build.yml`).

## Licence

Copyright (c) 2026 AssunaYuuki. Licensed under the GNU General Public License v3.0 — see
[LICENSE](LICENSE).
