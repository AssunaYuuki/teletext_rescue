cmake_minimum_required(VERSION 3.20)
project(TeletextRescue CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-fno-char8_t -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wno-sign-compare -Wno-unused-variable -Wno-unused-function)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

set(CORE
  src/util.cpp src/json.cpp src/teletext.cpp src/pages.cpp src/pagebuild.cpp src/project.cpp src/words.cpp src/flac.cpp
  src/png.cpp src/opencl.cpp src/vbidecode.cpp src/mlse.cpp src/decode_vbi.cpp src/vhs_wst.cpp
  src/vbi_probe.cpp src/vbi_auto.cpp src/nabts_slicer.cpp src/nabts.cpp src/nabts_html.cpp
  src/silent_radio.cpp src/datacast.cpp src/saa5050.cpp src/tools.cpp)
add_library(core STATIC ${CORE})
target_include_directories(core PUBLIC src)

set(RES res/resources.rc)
set(LIBS core normaliz comctl32 uxtheme dwmapi gdi32 comdlg32 shell32 ole32 winhttp shlwapi)

add_executable(trcli src/cli.cpp ${RES})
target_link_libraries(trcli ${LIBS})
target_link_options(trcli PRIVATE -static -municode -B${CMAKE_SOURCE_DIR}/res/nomanifest/)

add_executable(TeletextRescue WIN32 src/gui_main.cpp src/gui_widgets.cpp src/gui_page.cpp src/gui_windows.cpp src/gui_amol.cpp src/gui_nabts.cpp ${RES})
target_link_libraries(TeletextRescue ${LIBS})
target_link_options(TeletextRescue PRIVATE -static -municode -mwindows -B${CMAKE_SOURCE_DIR}/res/nomanifest/)
