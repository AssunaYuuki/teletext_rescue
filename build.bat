@echo off
rem Build Teletext Rescue on Windows: the program with windows and trcli.
rem Needs MinGW-w64 g++ (e.g. WinLibs), CMake 3.20+, Ninja; for the installer Inno Setup 6.
rem   build.bat           build into cpp\build
rem   build.bat setup     also the installer dist\TeletextRescue-Setup-v1.1.exe
setlocal
cd /d "%~dp0"
where cmake >nul 2>nul || (echo CMake not found & exit /b 1)
where g++ >nul 2>nul || (echo g++ ^(MinGW-w64^) not found & exit /b 1)
cmake -S cpp -B cpp\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DCMAKE_RC_COMPILER=windres || exit /b 1
rem g++ 16 sometimes crashes on its own - retry
for /l %%i in (1,1,10) do (
    cmake --build cpp\build -j 2 && goto built
)
echo build failed & exit /b 1
:built
echo Done: cpp\build\TeletextRescue.exe and cpp\build\trcli.exe
if /i not "%1"=="setup" goto :eof
set "STAGE=dist\Teletext Rescue 1.1"
if not exist "%STAGE%" mkdir "%STAGE%"
strip -o "%STAGE%\Teletext Rescue.exe" cpp\build\TeletextRescue.exe
strip -o "%STAGE%\trcli.exe" cpp\build\trcli.exe
copy /y LICENSE "%STAGE%\" >nul
copy /y README.txt "%STAGE%\" >nul
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
"%ISCC%" setup.iss || exit /b 1
