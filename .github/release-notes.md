**Windows 10/11:** `TeletextRescue-Setup-v1.3.exe` (64-bit) or `TeletextRescue-Setup-v1.3-x86.exe` (32-bit) — the program with windows and `trcli`; the same as zip: `TeletextRescue-windows-x64.zip`, `TeletextRescue-windows-x86.zip`.
**Linux** (x86_64, ARM64, x86) and **macOS** (Apple Silicon and Intel in one app): the program with windows (`TeletextRescue`, `Teletext Rescue.app`) and `trcli`. On macOS the first time: right click → Open, or `xattr -dr com.apple.quarantine "Teletext Rescue.app" trcli`.

### What is new in 1.3

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

