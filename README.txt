Teletext Rescue 1.2 — recovers teletext, programme guides, captions and other data hidden in VBI recordings

Author: AssunaYuuki
Teletext Rescue — Copyright (c) 2026 AssunaYuuki.
Licensed under the GNU General Public License v3.0 (see LICENSE).

Start: "Teletext Rescue" in the Start menu (or Teletext Rescue.exe). Windows 10/11, 64-bit.
File > Open .vbi recording...: the recording is examined and decoded; the results are kept in the
folder <name>_vbi next to the recording (the recording itself is never changed), so it opens again
at once. The start screen lists what was found; every service opens in its own window;
"Export everything decoded..." writes all results into a folder.
Decoding runs on the graphics card (OpenCL: NVIDIA, AMD or Intel) or on the processor.

What it reads:
  WST teletext (625 lines, 525-line NTSC, DVB .ts): pages as on a TV, page editor, clean pages,
    word restoration, export .t42 + HTML, subtitles .srt
  Japanese teletext (JTES, BT.653 system D): pages as the tuner drew them — kanji and kana,
    photographic pictures, smooth mosaics, DRCS, animations, tunes (WAV)
  NABTS / NAPLPS (CBS ExtraVision, NBC Teletext): pages drawn in time; HTML export
  StarSight programme guide: grid with titles and descriptions
  Gemstar GUIDE Plus+: programme table
  CC captions and XDS (line 21, NTSC and PAL); AMOL; Silent Radio LED sign
  TV-Inform (Russia), Interactive Network, 5.73 Mbit/s datacasts; VITS, VPS, WSS

What it opens:
  .vbi  — raw VBI capture (bt8x8, cx88, saa713x, cx23885, ivtv/cx18, em28xx; 16-bit 4fsc .tbc;
          FLAC-compressed .vbi.flac / .flac)
  .t42 / .t34 / .t33 — teletext and NABTS packet streams; .ts — DVB

Command line: trcli.exe <recording.vbi> [--export folder] [--cpu | --gpu] [--again]
