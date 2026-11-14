// DVB teletext (EN 300 472) from an MPEG transport stream .ts -> 42-byte
// teletext packets (.t42). From the original Teletext Rescue (tools.cpp).
#pragma once
#include "pagebuild.h"

std::vector<Packet42> ts_teletext_packets(const std::string &ts, Progress &pr);
// an MPEG transport stream by its contents (whatever its name: "x.ts.del", no extension): sync bytes 0x47 every
// 188 bytes (TS) or 192 (M2TS, a 4-byte time stamp before each); 0 = not one, else the packet size
int ts_packet_size(const std::string &path);
// everything the stream says about itself, a line each: channels (SDT), network (NIT), programs and their streams with
// languages, teletext pages and subtitles (PAT / PMT), time (TDT), programme now / next (EIT), length, errors
std::vector<std::string> ts_info(const std::string &path);
// the DVB subtitles (EN 300 743) as PNG pictures into dir, one a display set; langs: PID -> language. Returns the
// index, a line each: "file<TAB>language<TAB>PID<TAB>from s<TAB>to s<TAB>width<TAB>height" (seconds from the start)
std::vector<std::string> ts_subtitles(const std::string &path, const std::string &dir, const std::map<int, std::string> &langs);
// writes <out>.t42; -> number of packets
size_t ts_to_t42(const std::string &ts, const std::string &out, Progress &pr);
