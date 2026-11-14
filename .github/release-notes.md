**Windows 10/11:** `TeletextRescue-Setup-v1.5.exe` (64-bit) or `TeletextRescue-Setup-v1.5-x86.exe` (32-bit) — the program with windows and `trcli`; the same as zip: `TeletextRescue-windows-x64.zip`, `TeletextRescue-windows-x86.zip`.
**Linux** (x86_64, ARM64, x86) and **macOS** (Apple Silicon and Intel in one app): the program with windows (`TeletextRescue`, `Teletext Rescue.app`) and `trcli`. On macOS the first time: right click → Open, or `xattr -dr com.apple.quarantine "Teletext Rescue.app" trcli`.

### What is new in 1.5

- **ТВ-информ files from a damaged reception**: a file of the Norpak packet channel whose blocks failed their
  CRC (a capture card that drops frames cuts the records short) is now written as it came, the damaged blocks
  and bytes marked; a block whose header was lost is found at its place between its neighbours; a file whose
  description did not come is put together from its blocks. Text files (Windows CP1251 or DOS CP866) are shown
  in the report and in the found files — also in live — and written as `.txt`, `[…]` where bytes did not come
  (ORT 20.01.2002: the РИА «Новости» news file `3GL16425.DOC`).
- **VPS** is read in recordings: network and CNI, the station's own text (also through frames the capture
  lost), the programme label, sound, the time of the broadcast from the teletext clock and how many frames the
  capture lost (`vps.txt`). Lines in VPS form that carry a clock or a name (Eurosport) are no longer taken for
  ТВ-информ. The live VPS window shows one name, the time on the air and lost frames.
- **Graphics card**: live reads on the card from the first frame, also on channels without teletext; the card
  is chosen among all of them (a separate card before the built-in one, NVIDIA, AMD or Intel; `TR_GPU=<part of
  its name>` picks one).
- **BBC Micro and Ceefax telesoftware**: programs sent in teletext (Ceefax 1984) are collected from recordings
  and in live and run in a BBC Micro window of the program (File → Open a BBC Micro program, `.bbc` / `.ssd`).
  The BBC Micro ROMs (MOS 1.20, BASIC II) come with the installer and the packages (`roms`).
- **DVB streams (`.ts`)** are recognised by their contents whatever the name (issue #4): the tracks are described
  and DVB subtitles are written as pictures; the start screen has a **DVB stream .ts…** button.
- **Бизнес-Текст**: the 1995 form (29.12.1995, 25.05.1996) is decrypted; 1994 blocks whose rows were sent apart
  are complete again; a **Files table** window shows every file of the catalogue block by block (recording and
  live). The catalogue of 1993 (9.12.93, «Российские университеты» channel: every byte sent as a character with
  odd parity and a check byte, row 21 beginning `****`) is read, so its files are listed (`BNK.ARJ`, 26.11.93);
  the data of 1993 is not decrypted yet.
- **ТВ-информ test transmission** (Norpak, ORT 1999): every packet of it is now told apart from data — the test
  pattern read with errors where it has 7 zeros in a row and the marker packet every 4th bundle no longer make a
  "data stream"; a line where the SECAM identification is read as packets is not taken as a line of the service.
- **Teletext enhancements X/26** (issue #5): one wrong bit of a triplet is corrected, a bad triplet no longer
  loses the rest of the packet, and `.t42` export keeps the X/26 rows of each subpage (TVP2 1998: 185 pages with
  diacritics instead of 173, 43% more accented letters).
- **Lines named right**: a constant signal of the station (the same waveform in every frame) is no longer taken
  for a ТВ-информ carousel; a low-frequency signal on the SECAM identification lines 7-15 / 320-328 is shown as an
  unknown signal instead of "SECAM colour identification (0.8 MHz)" (the identification is 3.9-4.75 MHz); a test
  signal with the SECAM colour subcarrier (4.4 MHz, line 331 on Russian channels) is named as such, not "picture or
  unknown signal".
- **Live**: the found files window refreshes when a file's text grows; the line probe says why it did not run
  (`TR_DEBUG=1`).
