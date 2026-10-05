Teletext Rescue 1.1 — recovers teletext, NABTS and other data hidden in VBI recordings

Author: AssunaYuuki
Teletext Rescue — Copyright (c) 2026 AssunaYuuki.
Licensed under the GNU General Public License v3.0 (see LICENSE).

Start: "Teletext Rescue" in the Start menu (or Teletext Rescue.exe). Windows 10/11, 64-bit.
Interface: dark theme, start screen with recent files (also File > Recent), "What is in the
recording" window with a colour map of VBI lines and buttons for every result; the Silent Radio
LED sign plays inside the program; the start screen has an 80s-style LED time-and-temperature
board (local time; weather from an online weather service).
Decoding runs on the graphics card (OpenCL: NVIDIA, AMD or Intel); when a recording is opened you choose
the graphics card or the processor (Tools > Decode on... changes it later).

What it can do:
  WST teletext (European / Electra)
  - decodes raw .vbi captures from every common capture chip, even from worn VHS tapes:
    Viterbi decoders with a per-line channel fit, run-in offset calibration, detection of lost
    fields, byte repair, Hamming/parity-constrained reading of tape recordings
  - reads .t42 (625 lines) and .t34 (525 lines) packet streams and DVB .ts teletext
  - assembles pages and subpages, merges repeated copies into the most complete page,
    shows a tape quality report
  - shows pages as on a TV: Level 1 (flash, double size, boxes, conceal, mosaics),
    second character set (ESC), Latin national variants and Cyrillic (ru/sr/uk, guessed
    automatically, can be chosen in the "Character set" menu)
  - shows next to the page which channel or service sent it (packet 8/30 status display and
    network code, the fixed part of the page header) and when it was on air
  - page editor: type over characters, colour and mosaic codes, undo, FLOF colour keys,
    search by page number or title
  - Tools > Clean stream: damaged words restored, clean.t42 with one assembled copy of every
    subpage in page order, opened at once; Export > output.t42 can clean the stream as well
  - Tools > Restore words: words damaged on the tape are restored from built-in Russian, German
    and English dictionaries and the recording's own text (shown as a list first; undoable)
  - export: output.t42 + HTML pages (all or only complete ones), subtitles .srt from
    subtitle pages
  NABTS / NAPLPS (US: CBS ExtraVision, NBC Teletext)
  - finds the NABTS lines itself (also rare packets on other lines) and decodes them into a
    .t33 stream
  - assembles groups and records, votes damaged records from repeated copies
  - draws NAPLPS pages as the receiver did: drawing in time, WAIT pauses, colour-map
    changes, blinking; page text; choice of receiver resolution
  - saves a page as PNG, all pages as PNG + text, or all pages as HTML
  Silent Radio (US LED news-sign service, e.g. WTTW Chicago 1989)
  - line 21 at 1.25 Mbit/s, packets for three sign zones, votes intact copies
  - sign player (112x15 LEDs) in HTML: graphics, frame animation, pauses, headlines,
    scrolling stories, score lines; plus all texts as text.txt
  Encrypted datacast (5.73 Mbit/s)
  - blind MLSE read of VHS-limited lines, packets voted from their copies, header fixes
  - report: packet types (streams and carousel), addresses, block order and rate, repeat
    periods, burst timeline; packets as CSV/BIN (the data itself is encrypted)
  CC captions and XDS (line 21): caption text; station, network, time of day, programme, rating
  AMOL: broadcast clock and source ID in its own window; VITC time code; test signals (VITS)

What it opens (File menu):
  .vbi  — raw VBI capture (bt8x8, cx88, saa713x, cx23885, ivtv/cx18, em28xx; 16-bit 4fsc .tbc;
          also FLAC-compressed .vbi.flac / .flac):
          the format and what each line carries are detected, everything readable is decoded
          and opened: WST pages, NABTS, Silent Radio, CC captions; a report
          "<name>_vbi\report.txt" lists every line
  .t42  — WST packet stream (625 lines);  .t34 — WST 525 lines (Electra etc.);  .ts — DVB
  .t33  — NABTS stream (CBS ExtraVision, NBC Teletext): NAPLPS pages in their own window
  an existing project — a folder with pages.json
A project is created next to the source file; the source file itself is never changed.
Opening a decoded recording again asks: open the results or decode again.

Command line: trcli.exe vbi <recording.vbi> | squash <project> | words <project> [--apply] | ...
