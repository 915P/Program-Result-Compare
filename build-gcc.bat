@echo off
setlocal
cd /d "%~dp0"

rem ============================================================
rem  Build gui_compare.exe with MinGW / GCC
rem  Uses g++ from PATH; falls back to RedPanda-C++ default install.
rem ============================================================
set "MINGW="
where g++ >nul 2>nul
if not errorlevel 1 goto :havegcc

if exist "%APPDATA%\RedPanda-CPP\mingw64\bin\g++.exe" set "MINGW=%APPDATA%\RedPanda-CPP\mingw64\bin"
if defined MINGW goto :havegcc

echo [ERROR] g++ not found.
echo Install MinGW-w64 and add its bin folder to PATH.
exit /b 1

:havegcc
if defined MINGW (
    set "GXX=%MINGW%\g++.exe"
    set "WINDRES=%MINGW%\windres.exe"
) else (
    set "GXX=g++"
    set "WINDRES=windres"
)

echo [1/2] Compiling resources...
set "EMBED="
if exist "vendor\txt_compare.exe" (
    set "EMBED=-DEMBED_TXT_COMPARE"
    echo       embedding vendor\txt_compare.exe
) else (
    echo       vendor\txt_compare.exe not found - building without it
)
"%WINDRES%" %EMBED% app.rc -O coff -o app_res.o
if errorlevel 1 goto :fail

echo [2/2] Compiling and linking...
"%GXX%" -std=c++17 -O2 -DUNICODE -D_UNICODE -finput-charset=UTF-8 ^
    gui_compare.cpp app_res.o -o gui_compare.exe ^
    -mwindows -static -static-libgcc -static-libstdc++ -pthread ^
    -lcomctl32 -lcomdlg32 -lshell32 -lole32
if errorlevel 1 goto :fail

echo.
echo Build OK: %~dp0gui_compare.exe
exit /b 0

:fail
echo.
echo [FAILED] See the messages above.
exit /b 1
