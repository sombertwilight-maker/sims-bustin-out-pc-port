@echo off
rem ======================================================================
rem  The Sims Bustin' Out - rebuild the native Windows port from YOUR disc.
rem
rem  Needs: Python 3.10+ (python.org), CMake 3.20+, and either
rem         Visual Studio 2022 (Desktop C++ workload)  or  MinGW-w64 (gcc).
rem  Usage: build_windows.bat "C:\path\to\Sims, The - Bustin' Out (USA).iso"
rem
rem  Steps: extract default.xbe from the image -> analyse it -> translate all
rem  ~26,000 functions to C (about 2 million lines) -> compile the .exe.
rem  Takes 10-30 minutes depending on the machine.
rem ======================================================================
setlocal EnableDelayedExpansion
set "HERE=%~dp0"
set "ISO=%~1"
if "%ISO%"=="" (
  echo Usage: %~nx0 "path\to\your\Sims Bustin Out.iso"
  exit /b 1
)

rem --- locate the xboxrecomp toolkit ---
set "TK="
if exist "%HERE%..\..\xboxrecomp-0.17.0\tools" set "TK=%HERE%..\..\xboxrecomp-0.17.0"
if exist "%HERE%..\xboxrecomp\tools" set "TK=%HERE%..\xboxrecomp"
if not "%XBOXRECOMP_DIR%"=="" set "TK=%XBOXRECOMP_DIR%"
if "%TK%"=="" (
  echo Cannot find the xboxrecomp toolkit. Set XBOXRECOMP_DIR to its folder.
  exit /b 1
)
for %%I in ("%TK%") do set "TK=%%~fI"
echo Toolkit: %TK%

where python >nul 2>nul || (echo Python not found on PATH. & exit /b 1)
where cmake  >nul 2>nul || (echo CMake not found on PATH. & exit /b 1)
python -m pip install --quiet capstone || exit /b 1

rem --- 1. the XBE from the player's own disc ---
set "WORK=%TK%\game_files"
if not exist "%WORK%" mkdir "%WORK%"
pushd "%TK%"
python -m tools.xiso get "%ISO%" default.xbe -o "%WORK%\" || (popd & exit /b 1)

rem --- 2. analysis (seed lists come from this project) ---
copy /y "%HERE%seeds\*.json" "%WORK%\" >nul
python -m tools.xbe_parser "%WORK%\default.xbe" --json "%WORK%\default_analysis.json" --quiet || (popd & exit /b 1)
set "SEEDS="
for %%S in ("%WORK%\seeds_*.json") do set SEEDS=!SEEDS! --seed-functions "%%~fS"
python -m tools.disasm "%WORK%\default.xbe" --force !SEEDS! || (popd & exit /b 1)
python -m tools.func_id "%WORK%\default.xbe" || (popd & exit /b 1)
python -m tools.abi_analysis "%WORK%\default.xbe" || (popd & exit /b 1)

rem --- 3. lift to C into this project ---
python -m tools.recomp "%WORK%\default.xbe" --all --split 250 --game-name "The Sims Bustin' Out" ^
  --gen-dir "%HERE%src\recomp\gen" --exclude-manual "%HERE%src\recomp_manual.c" || (popd & exit /b 1)
popd
python "%HERE%tools\postprocess_gen.py" "%HERE%src\recomp\gen" || exit /b 1

rem --- 4. compile ---
where cl >nul 2>nul
if %errorlevel%==0 (
  cmake -S "%HERE%." -B "%HERE%build" -DXBOXRECOMP_DIR="%TK%" || exit /b 1
  cmake --build "%HERE%build" --config Release --parallel || exit /b 1
  set "OUT=%HERE%build\Release\SimsBustinOut.exe"
) else (
  echo NOTE: MinGW gcc works but is about 2x slower at run time than Visual Studio
  echo       ^(gcc emulates thread-local storage^). Prefer the VS 2022 prompt.
  where gcc >nul 2>nul || (echo No compiler found: run this from a "x64 Native Tools Command Prompt for VS 2022", or install MinGW-w64. & exit /b 1)
  cmake -S "%HERE%." -B "%HERE%build-mingw" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DXBOXRECOMP_DIR="%TK%" || exit /b 1
  cmake --build "%HERE%build-mingw" --parallel || exit /b 1
  set "OUT=%HERE%build-mingw\SimsBustinOut.exe"
)
echo.
echo Built: !OUT!
echo Copy it (with settings.ini) into a folder of its own and run it.
endlocal
