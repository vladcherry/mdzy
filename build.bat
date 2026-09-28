@echo off
rem Builds build\mdzy.exe with MSVC. Usage: build.bat [dev]
setlocal
cd /d "%~dp0"

where cl >nul 2>nul && goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :nomsvc
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :nomsvc
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul || goto :nomsvc

:build
set DEFS=/DNDEBUG
if /i "%~1"=="dev" set DEFS=/DMDZY_DEV
if not exist build mkdir build
rc /nologo /i src /fo build\mdzy.res src\mdzy.rc || exit /b 1
cl /nologo /std:c11 /O1 /GS- /Gy /MT /W3 %DEFS% src\mdzy.c build\mdzy.res /Fo:build\ /Fe:build\mdzy.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:NO /INCREMENTAL:NO || exit /b 1
echo Built build\mdzy.exe
exit /b 0

:nomsvc
echo MSVC not found. Install "Build Tools for Visual Studio" with the "Desktop development with C++" workload.
exit /b 1
