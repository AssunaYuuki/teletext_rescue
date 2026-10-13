// DVB teletext (EN 300 472) from an MPEG transport stream .ts -> 42-byte
// teletext packets (.t42). From the original Teletext Rescue (tools.cpp).
#pragma once
#include "pagebuild.h"

std::vector<Packet42> ts_teletext_packets(const std::string &ts, Progress &pr);
// writes <out>.t42; -> number of packets
size_t ts_to_t42(const std::string &ts, const std::string &out, Progress &pr);
