Teletext Rescue — recovers teletext, NABTS and other data hidden in VBI recordings

Author: AssunaYuuki
Teletext Rescue — Copyright (c) 2026 AssunaYuuki.
Licensed under the GNU General Public License v3.0 (see LICENSE).

Start: double-click Start.bat (or run: python teletext_gui.py).
Interface (gui_parts.py): dark theme, start screen with recent files (also File > Recent),
"What is in the recording" window with a colour map of VBI lines and buttons for every result,
the Silent Radio LED sign plays inside the program; the start screen has an 80s-style LED
time-and-temperature board (local time; weather for where the computer is, via wttr.in).
Requires Python 3 with numpy and pillow (pip install numpy pillow).
For GPU decoding of .vbi recordings (WST and NABTS) also install pyopencl (without it the CPU is used).

What it can do:
  WST teletext (European / Electra)
  - decodes raw .vbi captures (bt8x8 PAL/NTSC) even from blurred VHS tapes: Viterbi decoder
    on the GPU (OpenCL) or CPU, per-line channel fit as a fallback, run-in offset calibration,
    detection of lost fields, byte repair
  - reads .t42 (625 lines) and .t34 (525 lines) packet streams
  - assembles pages and subpages, merges repeated copies into the most complete page,
    shows a tape quality report
  - shows pages as on a TV: Level 1 (flash, double size, boxes, conceal, mosaics),
    second character set (ESC), Latin national variants and Cyrillic (ru/sr/uk, guessed
    automatically, can be chosen in the "Character set" menu)
  - page editor: type over characters, colour and mosaic codes, undo, FLOF colour keys,
    search by page number or title
  - export: output.t42 + HTML pages (all or only complete ones), subtitles .srt from
    subtitle pages
  NABTS / NAPLPS (US: CBS ExtraVision, NBC Teletext)
  - reads cx23885 NTSC .vbi dumps: finds the NABTS lines itself (also rare packets on other
    lines), decodes them on the GPU (seconds) or CPU into a .t33 stream
  - assembles groups and records, votes damaged records from repeated copies
  - draws NAPLPS pages as the receiver did: drawing in time, WAIT pauses, colour-map
    changes, blinking; page text; choice of receiver resolution
  - saves a page as PNG, all pages as PNG + text, or all pages as HTML — each page its own
    file with only the screen and the animation (click on it to replay)
  Silent Radio (US LED news-sign service, e.g. WTTW Chicago 1989)
  - reads 16-bit 4fsc NTSC .vbi dumps (910 samples x 16 lines, as vhs-decode TBC):
    line 21 at 1.25 Mbit/s, packets for three sign zones, votes intact copies
  - sign player (112x15 LEDs) in HTML: graphics, frame animation, pauses, headlines,
    scrolling stories, score lines; plus all texts as text.txt (pages\silent_radio.py)
  Encrypted datacast (5.73 Mbit/s, probably PBS National Datacast; WTTW lines 19, 20, 22)
  - blind MLSE read of VHS-limited lines, packets voted from their copies, header fixes
  - report: packet types (streams and carousel), addresses, block order and rate, repeat
    periods, burst timeline; packets as CSV/BIN (the data itself is encrypted) (pages\datacast.py)
  Tools
  - vbi_lines.py — what each VBI line of a recording carries; service_packets.py — packet statistics

What it opens (File menu):
  .vbi  — raw VBI capture (bt8x8 PAL/NTSC, cx23885, 16-bit 4fsc NTSC): opened automatically —
          the format and what each line carries are detected (pages\vbi_probe.py), everything
          readable is decoded and opened (pages\vbi_auto.py): WST pages, NABTS, Silent Radio,
          CC captions; a report "<name>_vbi\report.txt" lists every line
  .t42  — WST packet stream (625 lines);  .t34 — WST 525 lines (Electra etc.)
  .t33  — NABTS stream (CBS ExtraVision, NBC Teletext): NAPLPS pages in their own window,
          drawn as on screen (WAIT pauses, colour-map changes, blinking)
  an existing project — a folder "<name>_teletext" with pages.json
A project is created next to the source file; the source file itself is never changed.
Results of a .vbi go into one folder "<name>_vbi": report.txt/json, teletext\ (WST project),
<name>.t33 (NABTS), silentradio\ (sign player), datacast\ (datacast report), cc_*.txt. Opening a
decoded recording again asks: open the results or decode again.
