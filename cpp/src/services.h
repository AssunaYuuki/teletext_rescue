// What a decoded recording holds, as the program's windows list it: the services found and the texts
// shared by the Windows program and the SDL / ImGui one (Linux, macOS).
#pragma once
#include "decoder.h"
#include <memory>
#include <string>
#include <vector>

namespace ui {
using ResultPtr = std::shared_ptr<DecodeResult>;

enum class ServiceWindow { StarSight, Captions, Xds, Sign, Data, Amol, Nabts, Teletext, Gemstar, Tvinform, Report, LegacyFile, Jtes };
struct Service {
    std::string name;       // shown on the result button
    std::string kind;       // for service_colour()
    std::string where;      // lines it was read from
    std::string note;       // what was found
    ServiceWindow window;
    std::string dataKind;   // Data window: "NABTS", "WST teletext" or "framing 2D"; LegacyFile: the file
};

std::vector<Service> services_of(const DecodeResult &r);
std::string full_report(const DecodeResult &r);
std::string gemstar_text(const DecodeResult &r);     // report + every message with its time
std::string tvinform_text(const DecodeResult &r);
std::string rec_time(double seconds);         // 00:03:19.1
double frame_rate(const DecodeResult &r);     // frames per second of the recording
std::vector<int> led_glyph(char32_t ch);      // 5x7 LED font: columns, bit 0 = top row
std::string file_name(const DecodeResult &r);
}
