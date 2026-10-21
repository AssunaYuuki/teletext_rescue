cmake_minimum_required(VERSION 3.20)
# Windows: the program with windows (TeletextRescue.exe, Win32) and trcli.exe, MinGW-w64 g++.
# Linux, macOS: the program with windows (TeletextRescue, SDL3 + Dear ImGui) and trcli.
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
if(MSVC)
  add_compile_options(/utf-8)
else()
  add_compile_options(-fno-char8_t -Wall -Wno-sign-compare -Wno-unused-variable -Wno-unused-function)
endif()
if(WIN32)
  add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wno-unknown-pragmas -Wno-deprecated-declarations)
endif()

# the engine of the original Teletext Rescue, compiled inside namespace legacy
file(GLOB LEGACY ${CMAKE_SOURCE_DIR}/src/legacy/w_*.cpp)
set_source_files_properties(${LEGACY} PROPERTIES COMPILE_OPTIONS "-w")

# OpenCL is loaded at run time (OpenCL.dll / libOpenCL / OpenCL.framework), no SDK needed.
set(CORE
  src/amol.cpp src/vitc.cpp src/gemstar.cpp src/gemguide.cpp src/jtes.cpp src/jtesrx.cpp src/tvinform.cpp src/tvinorpak.cpp src/tviftr3.cpp src/biznestext.cpp src/arjcheck.cpp src/finds.cpp src/pcxfix.cpp src/faxg4.cpp src/onair.cpp src/ttpages.cpp src/ttsquash.cpp src/vbirepair.cpp src/export.cpp
  src/decoder.cpp src/ttdata.cpp src/starsight.cpp src/textservices.cpp src/silentradio.cpp src/ssjson.cpp src/rescache.cpp src/colour.cpp src/legacy_bridge.cpp src/services.cpp src/linecolour.cpp
  src/live.cpp src/tvtime.cpp src/vbicodes.cpp
  src/flac.cpp src/vbifile.cpp src/slicer_cpu.cpp src/slicer_gpu.cpp src/fastdata.cpp src/fastmlse.cpp src/mlse.cpp
  src/speller.cpp src/ui/util.cpp src/ui/json.cpp
  src/nabts/nabts.cpp src/nabts/nabts_html.cpp src/nabts/teletext.cpp src/nabts/png.cpp src/nabts/saa5050.cpp src/nabts/wavetop.cpp
  src/nabts/pagebuild.cpp src/nabts/words.cpp src/nabts/pages.cpp src/nabts/project.cpp src/nabts/dvbts.cpp
  ${LEGACY})

if(WIN32)
  add_library(trcore STATIC ${CORE})
  target_include_directories(trcore PUBLIC src src/ui src/nabts)
  set(LIBS trcore comctl32 uxtheme dwmapi gdi32 comdlg32 shell32 ole32 winhttp shlwapi advapi32 uuid winmm strmiids oleaut32)

  add_executable(TeletextRescue WIN32 src/gui_main.cpp src/gui_services.cpp src/gui_finds.cpp src/gui_nabts.cpp src/gui_teletext.cpp src/gui_lines.cpp src/gui_capture.cpp src/gui_repair.cpp src/gui_vps.cpp src/capture.cpp src/ui/gui_widgets.cpp src/app.rc)
  target_link_libraries(TeletextRescue ${LIBS})
  add_executable(trcli src/cli.cpp src/capture.cpp src/app.rc)
  target_link_libraries(trcli ${LIBS})
  if(MINGW)
    # src/nomanifest holds an empty default-manifest.o, so MinGW does not add a second manifest next to ours
    target_link_options(TeletextRescue PRIVATE -municode -mwindows -static -B${CMAKE_SOURCE_DIR}/src/nomanifest/)
    target_link_options(trcli PRIVATE -municode -static -B${CMAKE_SOURCE_DIR}/src/nomanifest/)
  endif()
else()
  # the resources (templates, dictionaries, tables) as arrays, from the RCDATA lines of src/app.rc
  set(GEN ${CMAKE_BINARY_DIR}/resources_gen.cpp)
  file(STRINGS ${CMAKE_SOURCE_DIR}/src/app.rc RC_LINES REGEX "RCDATA")
  set(OUT "// generated at build time from src/app.rc (do not edit)\n#include <cstddef>\n")
  set(ENTRIES "")
  set(DEPS "")
  foreach(L ${RC_LINES})
    if(L MATCHES "^([A-Za-z0-9_]+)[ \t]+RCDATA[ \t]+\"([^\"]+)\"")
      set(NAME ${CMAKE_MATCH_1})
      set(F ${CMAKE_SOURCE_DIR}/src/${CMAKE_MATCH_2})
      if(EXISTS ${F})
        list(APPEND DEPS ${F})
        file(READ ${F} HEX HEX)
        file(SIZE ${F} SZ)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HEX "${HEX}")
        string(APPEND OUT "static const unsigned char r_${NAME}[] = {${HEX}0};\n")
        string(APPEND ENTRIES "  {\"${NAME}\", r_${NAME}, ${SZ}},\n")
      endif()
    endif()
  endforeach()
  # the same table for the program and for the engine of the original program (namespace legacy)
  set(DECL "struct EmbeddedRes { const char *name; const unsigned char *data; size_t size; };\nextern const EmbeddedRes EMBEDDED_RES[] = {\n${ENTRIES}  {nullptr, nullptr, 0}};\n")
  set(TABLE "${DECL}namespace legacy {\n${DECL}}\n")
  file(WRITE ${GEN} "${OUT}${TABLE}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/app.rc ${DEPS})

  add_library(trcore STATIC ${CORE} ${GEN})
  target_include_directories(trcore PUBLIC src src/ui src/nabts)
  find_package(Threads REQUIRED)
  add_executable(trcli src/cli.cpp)
  target_link_libraries(trcli trcore Threads::Threads ${CMAKE_DL_LIBS})
endif()

# The windowed program on Linux and macOS (TeletextRescue), and on Windows for testing (TeletextRescueX,
# -DTR_XGUI=ON): SDL3 (fetched by CMake, built static) and Dear ImGui (src/third_party/imgui, docking branch: every
# window of the program is a window of the system, drawn with OpenGL).
option(TR_XGUI "build the SDL3 / Dear ImGui program" OFF)
if(NOT WIN32 OR TR_XGUI)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.2.10 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL3)
  set(IMGUI src/third_party/imgui)
  file(GLOB XGUI src/xgui/*.cpp)
  if(WIN32)
    set(XNAME TeletextRescueX)
  else()
    set(XNAME TeletextRescue)
  endif()
  add_executable(${XNAME} ${XGUI} ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
                 ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(${XNAME} PRIVATE ${IMGUI} src/xgui)
  target_link_libraries(${XNAME} trcore SDL3::SDL3-static)
  if(WIN32)
    target_link_libraries(${XNAME} ${LIBS})
    if(MINGW)
      target_link_options(${XNAME} PRIVATE -static)
    endif()
  else()
    target_link_libraries(${XNAME} Threads::Threads ${CMAKE_DL_LIBS})
  endif()
  if(APPLE)
    target_link_libraries(${XNAME} "-framework OpenGL")
    target_compile_definitions(${XNAME} PRIVATE GL_SILENCE_DEPRECATION)
  endif()
  if(APPLE)
    set_target_properties(${XNAME} PROPERTIES MACOSX_BUNDLE TRUE MACOSX_BUNDLE_BUNDLE_NAME "Teletext Rescue"
      MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.assunayuuki.teletextrescue" MACOSX_BUNDLE_BUNDLE_VERSION "1.3" MACOSX_BUNDLE_SHORT_VERSION_STRING "1.3")
  endif()
endif()
