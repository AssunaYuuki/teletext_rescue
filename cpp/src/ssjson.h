#pragma once
#include "decoder.h"
#include "starsight.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

struct JValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JValue> a;
    std::vector<std::pair<std::string, JValue>> o;
    const JValue* get(const std::string& k) const;
    int i(const std::string& k, int def = 0) const;
    std::string str(const std::string& k) const;
};

bool jsonParse(const std::string& text, JValue& v, std::string& err);

// Same schema as the community StarSight export (KCET_1994-05-03.json):
// channels, themes, programs, descriptions, listings, clock,
// daylightSavingChanges, sequenceNumbers.
bool ssImportJson(const std::string& path, SsDatabase& db, std::string& err);
bool ssExportJson(const std::string& path, const SsDatabase& db, const DecodeResult* extra, std::string& err);
