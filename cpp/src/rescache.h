#pragma once
// Saved decode results: "<recording>_vbi/results.trc". A recording that was decoded once opens
// from here at once, without decoding it again (the original program did the same with its
// results folder). The file remembers the size and time of the recording; if the recording
// changed, the saved results are not used.
#include "decoder.h"
#include <string>

std::string resultsCachePath(const std::string& recording);   // recording = the file the user opened
bool resultsCached(const std::string& recording);              // saved results exist and fit the recording
bool saveResults(const std::string& recording, const DecodeResult& r);
bool loadResults(const std::string& recording, DecodeResult& r, std::string& err);
