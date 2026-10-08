@echo off
REM ============================================================
REM  build_msvc.bat - Build CPlayer with Visual Studio 2026 (MSVC)
REM
REM  Pipeline:
REM    1. Locate VS (C++ tools) via vswhere
REM    2. Call vcvars64.bat to set up cl.exe / rc.exe / INCLUDE / LIB
REM    3. rc.exe compiles app.rc  -> app.res (version info only)
REM    4. cl.exe compiles + links -> cplayer.exe (single EXE, dynamic CRT)
REM
REM  No manifest glue needed: MSVC embeds the manifest via the linker.
REM ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

REM --- 1. Locate Visual Studio ---------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found at "%VSWHERE%"
    exit /b 1
)
set VSDIR=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo ERROR: No Visual Studio installation with C++ x86/x64 tools found.
    exit /b 1
)
echo Using Visual Studio: %VSDIR%

REM --- 2. Set up the build environment -------------------------
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo ERROR: vcvars64.bat failed & exit /b 1 )

REM --- 3. Compile resources (manifest + version info) ----------
REM Intermediate outputs go to build\ so the source dir stays clean.
REM NOTE: keep this file ASCII-only - cmd.exe parses .bat with the ANSI
REM codepage, and non-ASCII bytes (e.g. UTF-8 Chinese) corrupt parsing.
if not exist build mkdir build
rc.exe /nologo /fo build\app.res app.rc
if errorlevel 1 ( echo FAILED: rc.exe & exit /b 1 )

REM --- 4. Compile + link ---------------------------------------
REM  Flags:
REM    /std:c11  - C11 (MSVC's C99+ mode; the code uses mid-block decls)
REM    /utf-8    - source & execution charset are UTF-8 (Chinese literals)
REM    /O1       - optimize for SIZE (favours small code over speed)
REM    /GS-      - no buffer-security checks (smaller; trusted local app)
REM    /GL       - whole-program optimization (paired with /LTCG)
REM    /Gy       - function-level linking (enables /OPT:ICF COMDAT folding)
REM    /Gw       - whole-program global optimization (pairs with /GL)
REM    /MD       - dynamic CRT -> small EXE (~178KB). Depends on the VC++
REM                redistributable (vcruntime140.dll, present on any machine
REM                with VS / the redist installed) + ucrtbase.dll (ships with
REM                Win10/11). For a truly self-contained zero-DLL EXE, switch
REM                /MD -> /MT (costs ~+175KB).
REM    /EHa-     - no C++ EH (pure C)
REM  Link:
REM    /SUBSYSTEM:WINDOWS  - GUI app, no console
REM    /OPT:REF /OPT:ICF   - drop unreferenced code, fold identical COMDATs
REM    /LTCG               - link-time code generation (smaller + faster)
REM    /MERGE:.rdata=.text - fold read-only data into the code section
REM                          (saves a section header + alignment padding)
cl.exe /nologo /std:c11 /utf-8 /O1 /GS- /GL /Gy /Gw /MD /EHa- ^
    /DNDEBUG /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0600 ^
    /Fo:build\ /Fe:cplayer.exe ^
    main.c win32_ui.c playlist.c audio_decode.c audio_waveout.c tag_reader.c tag_cache.c ^
    build\app.res ^
    /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG /MERGE:.rdata=.text ^
    /MANIFEST:EMBED /MANIFESTINPUT:app.manifest ^
    user32.lib gdi32.lib msimg32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib winmm.lib

if errorlevel 1 ( echo FAILED: cl.exe & exit /b 1 )

echo.
echo Build OK: cplayer.exe
for %%I in (cplayer.exe) do echo Size: %%~zI bytes
echo (dynamic CRT; manifest embedded via /MANIFEST:EMBED. Use /MT for zero-DLL.)
endlocal
exit /b 0
