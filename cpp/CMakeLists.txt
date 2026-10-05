cmake_minimum_required(VERSION 3.20)
# Windows: программа с окнами (TeletextRescue.exe) и trcli.exe — MinGW-w64 g++.
# Linux, macOS: trcli — те же декодеры и отчёты из командной строки (окна программы — только Windows).
if(WIN32)
  project(TeletextRescue CXX RC)
else()
  if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "")   # std::filesystem
  endif()
  project(TeletextRescue CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-fno-char8_t -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wno-sign-compare -Wno-unused-variable -Wno-unused-function)
if(WIN32)
  add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")
else()
  add_compile_options(-Wno-unknown-pragmas -Wno-deprecated-declarations)
  set(CMAKE_CXX_FLAGS_RELEASE "-O2")
endif()

set(CORE
  src/util.cpp src/json.cpp src/teletext.cpp src/pages.cpp src/pagebuild.cpp src/project.cpp src/words.cpp src/flac.cpp
  src/png.cpp src/opencl.cpp src/vbidecode.cpp src/mlse.cpp src/decode_vbi.cpp src/vhs_wst.cpp
  src/vbi_probe.cpp src/vbi_auto.cpp src/nabts_slicer.cpp src/nabts.cpp src/nabts_html.cpp
  src/silent_radio.cpp src/datacast.cpp src/saa5050.cpp src/tools.cpp)

if(WIN32)
  add_library(core STATIC ${CORE})
  target_include_directories(core PUBLIC src)

  set(RES res/resources.rc)
  set(LIBS core comctl32 uxtheme dwmapi gdi32 comdlg32 shell32 ole32 winhttp shlwapi)

  add_executable(trcli src/cli.cpp ${RES})
  target_link_libraries(trcli ${LIBS})
  target_link_options(trcli PRIVATE -static -municode -B${CMAKE_SOURCE_DIR}/res/nomanifest/)

  add_executable(TeletextRescue WIN32 src/gui_main.cpp src/gui_widgets.cpp src/gui_page.cpp src/gui_windows.cpp src/gui_amol.cpp src/gui_epg.cpp src/gui_nabts.cpp ${RES})
  target_link_libraries(TeletextRescue ${LIBS})
  target_link_options(TeletextRescue PRIVATE -static -municode -mwindows -B${CMAKE_SOURCE_DIR}/res/nomanifest/)
else()
  # ресурсы (шаблоны, словари, таблицы) — массивами в resources_gen.cpp, по списку RCDATA из resources.rc
  set(GEN ${CMAKE_BINARY_DIR}/resources_gen.cpp)
  file(STRINGS ${CMAKE_SOURCE_DIR}/res/resources.rc RC_LINES REGEX "RCDATA")
  set(OUT "// generated at build time from res/ (do not edit)\n#include <cstddef>\n")
  set(TABLE "struct EmbeddedRes { const char *name; const unsigned char *data; size_t size; };\nextern const EmbeddedRes EMBEDDED_RES[] = {\n")
  set(DEPS "")
  foreach(L ${RC_LINES})
    if(L MATCHES "^([A-Za-z0-9_]+)[ \t]+RCDATA[ \t]+\"([^\"]+)\"")
      set(NAME ${CMAKE_MATCH_1})
      set(F ${CMAKE_SOURCE_DIR}/res/${CMAKE_MATCH_2})
      list(APPEND DEPS ${F})
      file(READ ${F} HEX HEX)
      file(SIZE ${F} SZ)
      string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HEX "${HEX}")
      string(APPEND OUT "static const unsigned char r_${NAME}[] = {${HEX}0};\n")
      string(APPEND TABLE "  {\"${NAME}\", r_${NAME}, ${SZ}},\n")
    endif()
  endforeach()
  string(APPEND TABLE "  {nullptr, nullptr, 0}};\n")
  file(WRITE ${GEN} "${OUT}${TABLE}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/res/resources.rc ${DEPS})

  add_library(core STATIC ${CORE} ${GEN})
  target_include_directories(core PUBLIC src)
  find_package(Threads REQUIRED)
  add_executable(trcli src/cli.cpp)
  target_link_libraries(trcli core Threads::Threads ${CMAKE_DL_LIBS})
endif()
