#pragma once
// WST teletext pages (525-line from the VBI, 625-line from DVB .ts): the
// packet stream goes through the page builder of the original Teletext
// Rescue (version voting, X/26, character set) into a project folder.
#include "pagebuild.h"
#include "project.h"
#include <functional>
#include <string>

struct TtResult {
    std::string project;     // project folder (pages.json, extras.json, project.json, stream)
    size_t packets = 0;
    int pages = 0, subpages = 0;
    std::string service;     // channel / network / time from packet 8/30, if sent
    std::string system;      // "525-line WST" or "625-line WST (DVB)"
};

// stream: .t34 bytes (34 per packet) when is525, otherwise .t42 bytes (42 per packet).
// dir: where the project goes. log receives the builder's messages.
bool ttBuild(const Bytes& stream, bool is525, const std::string& dir, const std::string& sourceName, TtResult& out,
             const std::function<void(const std::string&)>& log, std::string& err);
// working folder for a recording's teletext project: %TEMP%\TeletextRescue\<name>_teletext
std::string ttWorkDir(const std::string& recording);

// Clean pages (as vhs-teletext "squash" and the original program's "Clean stream"): damaged words
// restored (built-in Russian / German / English dictionaries + the recording's own frequent words),
// then one copy of every subpage assembled by majority over its repeats, damaged copies and rare
// garbage versions dropped -> <project>/clean.t42, built into the project <project>/clean.
// Returns the cleaned project folder; note says what was done.
std::string ttClean(const std::string& project, Progress& pr, std::string& note);
