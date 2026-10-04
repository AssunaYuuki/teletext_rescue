# Teletext Rescue

Recovers teletext, NABTS and other data hidden in the vertical blanking interval (VBI) of old
video recordings — and shows it the way viewers saw it in the 1980s–90s.

Open a raw VBI capture and the program works out the rest by itself: the capture format, what
every VBI line carries, and which decoders to run. Everything readable is decoded and opened.

## What it reads

| Service | What you get |
|---|---|
| **WST teletext** (PAL, 525-line) | pages with Level 1 rendering (flash, double size, boxes, conceal), a page editor, export to `.t42` + HTML, subtitles `.srt` |
| **NABTS / NAPLPS** (CBS ExtraVision, NBC Teletext) | NAPLPS pages drawn as the receiver drew them — in time, with pauses, colour-map changes and blinking; HTML export |
| **Silent Radio** (LED news-sign service, line 21) | the sign plays inside the program: graphics, animations, headlines, scrolling stories, score lines; all texts |
| **Encrypted datacast** (5.73 Mbit/s, probably PBS National Datacast) | packets voted from their copies, addresses, streams and carousel, the reassembled carousel file (the data itself is encrypted) |
| **CC captions** (line 21) | caption text |
| **Test signals (VITS)** | the frequency response of the recording, measured from the multiburst |

Recording formats: bt8x8 PAL/NTSC, cx23885 (27 MHz), 16-bit 4fsc NTSC (vhs-decode TBC).
VHS-limited signals are read with MLSE (Viterbi) decoders that fit the tape channel by themselves;
the heavy parts run on the GPU through OpenCL when one is available.

## Running

Requires Python 3 with `numpy` and `Pillow`; `pyopencl` is optional (GPU decoding).

```
pip install numpy pillow
pip install pyopencl          # optional
python teletext_gui.py        # or double-click Start.bat
```

**File → Open .vbi recording…** — the recording is examined automatically. Results go into one
folder `<name>_vbi` next to the recording (the recording itself is never changed):
`report.txt`, `teletext\`, `<name>.t33`, `silentradio\`, `datacast\`, `cc_*.txt`.
A window *What is in the recording* shows a colour map of the VBI lines and opens each result.

Command-line tools live in `pages\` (e.g. `python pages\vbi_auto.py recording.vbi`,
`python pages\vbi_probe.py recording.vbi`).

## Licence

Copyright (c) 2026 AssunaYuuki. Licensed under the GNU General Public License v3.0 — see
[LICENSE](LICENSE).

The NABTS / NAPLPS part is a port of [decode-orc](https://github.com/simoninns/decode-orc)
by Simon Inns (GPL-3.0).
