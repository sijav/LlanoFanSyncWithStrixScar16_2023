@echo off
setlocal
rem Builds build\LlanoFanSync.exe with the Microsoft C++ compiler.
rem Needs Visual Studio 2019 or newer (any edition, or the free Build Tools) with "Desktop development with C++".
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer not found.
    echo Install Visual Studio or "Build Tools for Visual Studio" with the "Desktop development with C++" workload.
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No Visual Studio installation with the C++ x64 tools was found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1

if not exist build mkdir build
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /utf-8 src\LlanoFanSync.cpp ^
   /Fo:build\ /Fe:build\LlanoFanSync.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /MANIFEST:EMBED || exit /b 1
del /q build\*.obj 2>nul

echo.
echo Built: %CD%\build\LlanoFanSync.exe
