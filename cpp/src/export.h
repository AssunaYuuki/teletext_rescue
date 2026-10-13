#pragma once
#include "decoder.h"

// Writes everything that was decoded into dir (created if needed), one file
// or folder per service:
//   report.txt            what the decoder found and did
//   captions.txt, xds.txt, silent_radio.txt, amol.csv
//   starsight.json        StarSight database (schema of KCET_1994-05-03.json)
//   nabts.t33             NABTS packets (33 bytes each), opens in Teletext Rescue
//   nabts_pages/          every NAPLPS page as PNG + records.txt
//   nabts_html/           the pages as HTML, drawn as on the receiver
//   data_57.txt           5.7 Mbit/s summary and recovered text
//   framing2d.bin         raw framing-2D datacast packets (34 bytes each)
// Returns a short list of what was written.
std::string exportResult(const DecodeResult& r, const std::string& dir, Progress& pr);
