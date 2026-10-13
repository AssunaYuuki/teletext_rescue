// System headers of the original Teletext Rescue engine, included once outside namespace legacy
// so the engine sources can be compiled inside it (src/legacy/w_*.cpp).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
// Windows macros that clash with names in the engine (decode_vbi.cpp "near"/"OUT", nabts.cpp "TRANSPARENT")
#undef near
#undef far
#undef IN
#undef OUT
#undef OPTIONAL
#undef TRANSPARENT
#undef OPAQUE
