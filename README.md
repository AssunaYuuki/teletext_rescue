# Teletext Rescue

Recovers teletext, NABTS and other data hidden in the vertical blanking interval (VBI) of old
video recordings — and shows it the way viewers saw it in the 1980s–90s.

Open a raw VBI capture and the program works out the rest by itself: the capture chip and
format, what every VBI line carries, and which decoders to run. Everything readable is decoded
and opened.

## What it reads

| Service | What you get |
|---|---|
| **WST teletext** (PAL/SECAM, 525-line) | pages with Level 1 rendering (flash, double size, boxes, conceal), a page editor, export to `.t42` + HTML, subtitles `.srt`, a cleaned stream, word restoration, the channel and the time of the broadcast |
| **NABTS / NAPLPS** (CBS ExtraVision, NBC Teletext) | NAPLPS pages drawn as the receiver drew them — in time, with pauses, colour-map changes and blinking; HTML export |
| **Silent Radio** (LED news-sign service, line 21) | the sign plays inside the program: graphics, animations, headlines, scrolling stories, score lines; all texts |
| **Encrypted datacast** (5.73 Mbit/s) | packets voted from their copies, addresses, streams and carousel (the data itself is encrypted) |
| **CC captions and XDS** (line 21) | caption text; station, network, time of day, programme name and rating from XDS |
| **AMOL**, **VITC** | programme identification and time stamp in its own window; time code |
| **Test signals (VITS)** | the frequency response of the recording, measured from the multiburst |
| **DVB teletext** (`.ts`) | teletext pages from a digital broadcast recording |

Capture chips: bt8x8, cx88, saa713x, cx23885, ivtv/cx18, em28xx (PAL/SECAM/NTSC), 16-bit 4fsc
`.tbc`; FLAC-compressed captures (`.vbi.flac`, `.flac`); streams `.t42`, `.t34`, `.t33`, DVB `.ts`.

Signals worn by VHS are read with MLSE (Viterbi) decoders that fit the tape channel by
themselves. Decoding runs on the graphics card through OpenCL — NVIDIA, AMD or Intel, whichever
the computer has — and can be switched to the processor when a recording is opened.

## Installing and running

Run `TeletextRescue-Setup-v1.1.exe` (Windows 10/11, 64-bit). Nothing else is needed.

**File → Open .vbi recording…** — the recording is examined automatically. You choose what decodes
it (graphics card or processor; the choice can be remembered). Results go into one folder
`<name>_vbi` next to the recording (the recording itself is never changed): `report.txt`,
`teletext\`, `<name>.t33`, `silentradio\`, `datacast\`, `cc_*.txt`, `xds_*.txt`, `amol_*.txt`.
A window *What is in the recording* shows a colour map of the VBI lines and opens each result.

Next to every teletext page the program shows the channel and broadcast time (from the page header
and packet 8/30). The page list shows the section from the page header.

**Tools → Clean stream** restores damaged words, writes `clean.t42` with one assembled copy of every
subpage in page order, and opens it. **Export → output.t42** can clean the stream as well.
**Tools → Restore words** finds words damaged on the tape (a digit inside a word, letters with bit
errors) and restores them from built-in Russian, German and English dictionaries and from the
recording's own text — the list is shown before anything is changed, and every page can be undone.

`trcli.exe` is the same program from the command line, e.g. `trcli vbi recording.vbi`,
`trcli devices`, `trcli vbi recording.vbi --cpu`, `trcli squash <project folder>`,
`trcli words <project folder>`, `trcli flac recording.vbi.flac recording.vbi`.

## What is new in 1.1

- The whole program rewritten in C++: one small installer, no runtime to install, decoding many
  times faster, on the graphics card of any vendor (NVIDIA, AMD, Intel) or on the processor.
- FLAC-compressed captures open directly.
- Clean stream, word restoration with built-in dictionaries, channel and time next to the page.
- XDS on line 21: station, network, time of day, programme and rating.
- AMOL window: broadcast clock, source ID and the packet bits.
- DVB teletext from broadcasters that repeat rows is assembled into full pages.
- NAPLPS animations play at their real speed.
- Capture format detection checks the captions on line 21, so cropped 27 MHz captures are no
  longer mistaken for 13.5 MHz ones.

## Building

**Windows** — MinGW-w64 g++, CMake and Ninja: run `build.bat` (the program and `trcli`);
`build.bat setup` also makes the installer with Inno Setup.

**Linux and macOS** — `trcli`, the same decoders, reports and HTML exports from the command line
(the windowed program is Windows only):

```
sh build.sh
cpp/build-linux/trcli vbi recording.vbi      # cpp/build-darwin/trcli on macOS
```

Ready-made `trcli` for Linux (x86_64, ARM64) and macOS (one file for Apple Silicon and Intel) is
attached to each release. On macOS, after unpacking: `xattr -d com.apple.quarantine trcli`.
They can also be built on Windows with Zig: `python tools/cross_build.py path	o\zig.exe`.

Needs a C++20 compiler (g++ 10+, clang 12+ / Xcode 13+) and CMake 3.20+. Decoding uses the
graphics card through OpenCL when it is there (Linux: `ocl-icd` plus the card's driver; macOS: built
in), otherwise the processor.

## Licence

Copyright (c) 2026 AssunaYuuki. Licensed under the GNU General Public License v3.0 — see
[LICENSE](LICENSE).
