**Windows 10/11:** `TeletextRescue-Setup-v1.6.2.exe` (64-bit) or `TeletextRescue-Setup-v1.6.2-x86.exe` (32-bit) — the program with windows and `trcli`; the same as zip: `TeletextRescue-windows-x64.zip`, `TeletextRescue-windows-x86.zip`.
**Linux** (x86_64, ARM64, x86) and **macOS** (Apple Silicon and Intel in one app): the program with windows (`TeletextRescue`, `Teletext Rescue.app`) and `trcli`. On macOS the first time: right click → Open, or `xattr -dr com.apple.quarantine "Teletext Rescue.app" trcli`.

### What is new in 1.6.2

- **Numbered teletext subpages are kept**: in a sequence such as `1/13`…`13/13`, parts received only twice are no longer
  mistaken for noise and dropped. The exception is limited to subcodes with consistent page counters; ordinary pages
  keep the existing repeat threshold.
- **Already assembled `.t42` files keep their text**: a cleaned stream with one copy of each subpage is recognised even
  when a page has many subpages. Reopening it no longer removes every display row and leaves only empty page headers.

### What is new in 1.6.1

- **Silent Radio fixed** (the LED news sign in the VBI of WTTW, WNET ...): the sign line is found on every line of a
  recording (the preamble is matched as a whole, not by its first cycles — a CX23885 capture lost one line in 25, and
  every lost line took two letters with it); a line read without its preamble keeps its place, so the repeats of a
  packet stay lined up and are voted bit by bit — the text comes out clean instead of mixed (WNET 1988: 187 clean
  items instead of 461 with garbage). A repeat with a piece of tape missing is left out; the same item sent again
  under a new number helps too; where the recording ends the packet ends.
- **The sign shows what it showed**: pictures stay next to their headlines (Olympic rings, the birthday cake, the golf
  flag), headlines are two lines, animations play in place (the turning film reel of MOVIE REVIEW, blinking eyes,
  SPORTS letter by letter over a sign memory wider than the 112 LEDs), a narrow space between words no longer eats a
  letter (MEN'S GOLF, THE VERDICT), lowercase letters have their tails.
- **Transitions**: wipes, slides, rolling up and down, dissolve, the two lines of a headline each in its own way,
  the window panning; scrolling text at a readable speed.
- **The sign player** (`silent_radio/index.html` of an export): glowing LEDs that fade, every transition, all zones
  in the order they were sent, speed, colour, picture by picture, search, a link to a moment (`#9BAB/1/0.3`).
  The sign window of the program shows the transitions too.
- `packets.json` of the sign no longer breaks a character in two in titles.

### What is new in 1.6

- **Pages of a rotation stay apart**: a page that turns through several texts under one subcode or one number
  (TELEINF 101: championships, exchange rates, tours ...) is shown as one subpage a text, also in the text and picture
  export; before, the export voted them row by row into one mix, and a page whose transmissions a capture cut short
  was not taken for a rotation at all.
- **Rows no page shows** are written to `unplaced_rows.txt` — versions of pages whose header was lost, rows a service
  sent without a page — instead of being guessed onto a page.
- **NABTS datacasts with error correction**: each row is put right by its own code, the bundle's layout (how many
  data rows, which check rows are sent, before or after their data) is learnt from the stream, lost and damaged rows
  are rebuilt by the row and column codes together. WaveTop gets the records the old reading missed; other channels
  with this protection are written as `datacast_<address>.bin`.
- **Antiope, the French teletext**: the Didon packets (6.2 Mbit/s) of French recordings of the 1980s are read with the
  per-line channel decoder, and the Antiope pages are put together and voted over their copies. They are drawn as
  the receiver showed them — the colour of the screen, colours, double height and width, mosaics, accents, concealed
  text — and shown in their own window, written as pictures and text (`antiope/<page>.png`, `.txt`, `all.txt`) with
  a page viewer (`antiope/index.html`). Antenne 2, January 1986: the SAVOIR/A2 service, 99 pages (news, sports,
  exchange rates, books, the viewers' poll).
- **VideoCrypt lines named and read**: the blocks on lines 8-15 / 321-328 of satellite recordings of the 1990s
  (MTV Europe 1995) are VideoCrypt I and II data — the lines are named so, and their messages are read, checked and
  listed in `videocrypt.txt` with what each says: soft-scrambled (any decoder shows the picture) or for card holders,
  the month it was issued for (July 1995 on MTV, the date of its teletext), the channel, a name for the decoder's
  screen when one is sent.
- **Teletext pages no longer show over each other** when a recording is opened (in live they were right): a
  recording of a capture card that drops frames or whose VBI window misses lines was built "field-aware", and
  most rows of its pages (69-95%) were then filled in from other transmissions — rows of two or three versions,
  or of another page, on one page (BBC 1 April: the index rows on every page of a recipe). Such a build is not
  used any more; recordings opened before are built again from the stream they kept (the recording is not read
  again).
- **Pages saved as text**: export and opening a recording write every page and subpage as clean UTF-8 text
  (`pages/100.txt`, `pages/800_2.txt`, all together in `pages/all.txt`) and `teletext/clean.t42` — one assembled
  copy of every subpage, which the program opens as a stream. In live, **Save pages** asks for the folder once a
  session and updates it after that (Shift = a new folder); a `.t42` dropped on the capture window is decoded.
- **TeletextData Petersburg / ТхТ 2/200** (5th channel, St Petersburg): files sent on magazine-8 pages are put
  together from their blocks — the catalogue on page 800 gives each file's name, size and pages — in recordings
  and in live.
- **Бизнес-Текст**: binary files of the service (`.C01`…`.C04`, `.COD`) get a `.txt` with their records and a hex
  dump in the found files; the files inside a received ARJ archive are listed one by one (texts as UTF-8, PCX
  pictures as PNG) with their CRC; rows of a block sent apart are put back by the checksum of the catalogue; a
  1994 batch where no file reads as text is opened with the transport key of the tape of 9.07.94.
- **InterTVNet** files are found in live too, not only in recordings.
- **Syster / Nagravision** report: ECM and EMM messages are listed more completely (EMMs sent directly, the
  operating mode of the messages).
- **`trcli --follow <recording.vbi>`** (Windows): reads a recording while it is still being written, through the
  live decoder; the ready pages are written as text every few hundred frames and the session as `live.t42` at
  the end.
