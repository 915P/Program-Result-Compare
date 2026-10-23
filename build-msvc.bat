@echo off
setlocal
cd /d "%~dp0"

rem ============================================================
rem  Build gui_compare.exe with MSVC
rem  vswhere is used to locate Visual Studio and vcvars64.bat.
rem ============================================================
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Is Visual Studio installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"

if not defined VSDIR (
    echo [ERROR] No MSVC toolset found.
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed.
    exit /b 1
)

echo [1/2] Compiling resources...
set "EMBED="
if exist "vendor\txt_compare.exe" (
    set "EMBED=/d EMBED_TXT_COMPARE"
    echo       embedding vendor\txt_compare.exe
) else (
    echo       vendor\txt_compare.exe not found - building without it
)
rc /nologo /fo app_res.res %EMBED% app.rc
if errorlevel 1 goto :fail

echo [2/2] Compiling and linking...
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE gui_compare.cpp app_res.res ^
   /Fe:gui_compare.exe /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib
if errorlevel 1 goto :fail

del /q *.obj >nul 2>nul
echo.
echo Build OK: %~dp0gui_compare.exe
exit /b 0

:fail
echo.
echo [FAILED] See the messages above.
exit /b 1
