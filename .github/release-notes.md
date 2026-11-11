**Windows 10/11:** `TeletextRescue-Setup-v1.4.exe` (64-bit) or `TeletextRescue-Setup-v1.4-x86.exe` (32-bit) — the program with windows and `trcli`; the same as zip: `TeletextRescue-windows-x64.zip`, `TeletextRescue-windows-x86.zip`.
**Linux** (x86_64, ARM64, x86) and **macOS** (Apple Silicon and Intel in one app): the program with windows (`TeletextRescue`, `Teletext Rescue.app`) and `trcli`. On macOS the first time: right click → Open, or `xattr -dr com.apple.quarantine "Teletext Rescue.app" trcli`.

### What is new in 1.4

- **Russian teletext in the right alphabet**: Russian services sent no character-set packets, the receiver
  took the set from the page header — most pages start their rows in Cyrillic and ESC switches to Latin, but
  pages with national option 0 (Ostankino 1993-94, ORT 1995-96, TELEINF) start in Latin and switch to Cyrillic.
  Every page is now read the way its words say (Russian and English dictionaries): English text is no longer
  shown in Cyrillic letters and Russian text no longer as transliteration — in the pages, the HTML / PNG export
  and in live.
- **Teletext pages put together better**: pages that rotate several texts under one subcode (TELEINF 100) are
  separate subpages again instead of a mix of letters; half-empty pages after voting fixed; every subpage keeps
  its own number in the written `.t42`; rows with a damaged row number go back to their place; teletext on
  lines 6 and 318 is read (Партнер-Телетекст, Donetsk 1999: 1.3% -> 0.27% errors).
- **Live** (Windows): all teletext reading runs on the graphics card and the processor stays almost free;
  the card starts reading after a few frames; the learnt templates are used live too; carousel pages are
  kept apart; the capture window no longer flickers; no administrator rights needed (a card that wants them
  asks for a restart).
- **Graphics card**: the built OpenCL programs are kept, so a recording no longer waits 1-4 s for them on
  every opening; short probes run on the processor.
- **NTSC recordings**: the capture layout is found also without captions (a 720-sample reading of a
  1440-sample capture no longer wins on Nielsen AMOL), captions of a recording that changes channels are no
  longer thrown away, a crash on some PAL-looking NTSC captures fixed; Japanese teletext from GV-USB2
  captures is found by the probe.
- **Files found in the signal**: ITS / IBS data channels (packets X/31, TELEINF) and **InterTVNet**
  (BCH + CRC, files, pieces `.@N`, the access list); a RAR unpacker of its own (RAR 2.9-4.x, LZ and PPMd,
  filters); ARJ archives unpacked through damage (lost start, cut-off end, holes) and mended by their CRCs,
  copies of a file under other names combined; TV-Inform fT3 files put together by themselves from a full
  capture (the order of the lines is learnt, single bit errors mended by CRC-32); Бизнес-Текст: open files of
  the 1996 catalogue written as received, the key search no longer fooled by them.
- Page editor: Ctrl+S also writes `<recording> edit.t42`. A `.t42` of one page opens.
- **macOS and Linux**: the start screen is in the main window again (it went into a borderless window of its
  own, and the empty main window, once clicked, covered all the other windows); a window that is opened or
  clicked comes to the front, as on Windows.

