@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "ROOT=%CD%"
set "BUILD=%ROOT%\build"
set "DEPS=%ROOT%\.deps"
set "PROTO_VER=3.21.12"
set "PROTO_SRC=%DEPS%\protobuf-%PROTO_VER%"
set "PROTO_BUILD=%DEPS%\protobuf-build"
set "PROTO_DIR=%DEPS%\protos"
set "PB_OUT=%ROOT%\src\Resource\Protobufs"
set "PROTO_SHA=a742e214618bfb0d4668e1b7a022b8db4d0cbe08"
set "PF86=%ProgramFiles(x86)%"
if not defined PF86 set "PF86=%ProgramFiles%"

rem --- offsets dumper -------------------------------------------------------
set "DUMPER=C:\Users\Unkn0Wn\Desktop\dota2-offset-dumper-main"
if defined ANCHOUSE_DUMPER set "DUMPER=%ANCHOUSE_DUMPER%"
if not defined ANCHOUSE_DUMPER if defined OVR_DUMPER set "DUMPER=%OVR_DUMPER%"
if not exist "%DUMPER%\dota2_dumper.exe" if exist "%ROOT%\..\dota2-offset-dumper-main\dota2_dumper.exe" set "DUMPER=%ROOT%\..\dota2-offset-dumper-main"
set "DOTA_DIR=C:\Steam\steamapps\common\dota 2 beta"
if defined ANCHOUSE_DOTA_DIR set "DOTA_DIR=%ANCHOUSE_DOTA_DIR%"
if not defined ANCHOUSE_DOTA_DIR if defined OVR_DOTA_DIR set "DOTA_DIR=%OVR_DOTA_DIR%"

set "PAUSE=1"
set "FORCE_DUMP=0"
set "SKIP_OFFSETS=0"
if /i "%~1"=="nopause" set "PAUSE=0"
if /i "%~2"=="nopause" set "PAUSE=0"
if /i "%~1"=="nooffsets" set "SKIP_OFFSETS=1"
if /i "%~2"=="nooffsets" set "SKIP_OFFSETS=1"
if /i "%~1"=="offsets" set "FORCE_DUMP=1"

if /i "%~1"=="refresh" goto :refresh
if /i "%~1"=="clean" goto :clean
if /i "%~1"=="rebuild" goto :rebuild
if /i "%~1"=="distclean" goto :distclean
if /i "%~1"=="help" goto :help
if /i "%~1"=="--help" goto :help
if /i "%~1"=="-h" goto :help

goto :build

:refresh
set "FORCE_DUMP=1"
call :update_offsets
if "%PAUSE%"=="1" pause
exit /b 0

:build
echo ============================================================
echo  anchousechanger build
echo  Root: %ROOT%
echo ============================================================

call :find_vs
if errorlevel 1 goto :fail
set "VSLANG=1033"
call :find_cmake
if errorlevel 1 goto :fail

call :update_offsets

if not exist "%PROTO_BUILD%\protoc.exe" goto :protobuf_needed
if not exist "%PROTO_BUILD%\libprotobuf.lib" goto :protobuf_needed
goto :protobuf_ready

:protobuf_needed
call :build_protobuf
if errorlevel 1 goto :fail

:protobuf_ready
if exist "%PB_OUT%\base_gcmessages.pb.cc" goto :protos_ready
echo [2/4] Preparing protobuf sources...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\prepare_protos.ps1" -OutDir "%PB_OUT%" -ProtoDir "%PROTO_DIR%" -Protoc "%PROTO_BUILD%\protoc.exe" -ProtobufSrc "%PROTO_SRC%\src" -Sha "%PROTO_SHA%"
if errorlevel 1 (
    echo ERROR: could not generate protobuf sources.
    goto :fail
)

:protos_ready
if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%BUILD%\obj" mkdir "%BUILD%\obj"

echo [3/4] Compiling anchousechanger.dll...
(
    for /r "%ROOT%\src" %%f in (*.cpp) do echo "%%f"
    for /r "%ROOT%\src" %%f in (*.cc) do echo "%%f"
    for /r "%ROOT%\src\Resource\Minhook" %%f in (*.c) do echo "%%f"
) > "%BUILD%\sources.rsp"

cl.exe /nologo /LD /std:c++20 /EHsc /MT /O2 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WINSOCKAPI_ /utf-8 /I"%ROOT%\src" /I"%ROOT%\src\Resource\Minhook\include" /I"%PB_OUT%" /I"%PROTO_SRC%\src" /Fo"%BUILD%\obj\\" @"%BUILD%\sources.rsp" /link /OUT:"%BUILD%\anchousechanger.dll" /LIBPATH:"%PROTO_BUILD%" libprotobuf.lib user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib psapi.lib version.lib winmm.lib ws2_32.lib /MACHINE:X64 > "%BUILD%\compile.log" 2>&1
if errorlevel 1 (
    echo ERROR: anchousechanger.dll build failed.
    call :tail "%BUILD%\compile.log"
    goto :fail
)
if not exist "%BUILD%\anchousechanger.dll" (
    echo ERROR: compiler reported success, but the DLL is missing.
    goto :fail
)

echo [4/4] Compiling loader.exe...
cl.exe /nologo /std:c++20 /EHsc /MT /O2 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /utf-8 /Fo"%BUILD%\obj\loader.obj" "%ROOT%\loader\loader.cpp" /Fe:"%BUILD%\loader.exe" /link /SUBSYSTEM:CONSOLE /MACHINE:X64 shell32.lib advapi32.lib user32.lib > "%BUILD%\loader.log" 2>&1
if errorlevel 1 (
    echo ERROR: loader.exe build failed.
    call :tail "%BUILD%\loader.log"
    goto :fail
)
if not exist "%BUILD%\loader.exe" (
    echo ERROR: compiler reported success, but loader.exe is missing.
    goto :fail
)

echo.
echo ============================================================
echo  Build complete
echo ============================================================
echo   %BUILD%\anchousechanger.dll
echo   %BUILD%\loader.exe
echo.
echo  Usage:
echo    1. Start Dota 2 and wait for the main menu.
echo    2. Run "%BUILD%\loader.exe"
echo       or "%BUILD%\loader.exe" --launch
echo.
if "%PAUSE%"=="1" pause
exit /b 0

:find_vs
if not exist "%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" (
    echo ERROR: vswhere.exe was not found.
    echo Install Visual Studio 2022 or newer with the "Desktop development with C++" workload.
    exit /b 1
)

set "VSPATH="
for /f "usebackq delims=" %%i in (`"%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo ERROR: Visual Studio with the x64 C++ tools was not found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: could not initialize the Visual Studio x64 environment.
    exit /b 1
)

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe is not available after vcvars64.bat.
    exit /b 1
)
exit /b 0

:find_cmake
set "CMAKE="
where cmake.exe >nul 2>&1
if not errorlevel 1 set "CMAKE=cmake.exe"
if not defined CMAKE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE=%ProgramFiles%\CMake\bin\cmake.exe"
if not defined CMAKE if exist "%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKE (
    echo ERROR: CMake was not found.
    echo Install CMake from https://cmake.org/download/ and run this script again.
    exit /b 1
)
exit /b 0

:build_protobuf
echo [1/4] Preparing protobuf %PROTO_VER%...
if not exist "%DEPS%" mkdir "%DEPS%"

if exist "%PROTO_SRC%\src\google\protobuf\message.h" goto :bp_configure

if not exist "%DEPS%\protobuf-%PROTO_VER%.zip" (
    echo   downloading protobuf %PROTO_VER%...
    where curl.exe >nul 2>&1
    if errorlevel 1 (
        powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri 'https://github.com/protocolbuffers/protobuf/archive/refs/tags/v%PROTO_VER%.zip' -OutFile '%DEPS%\protobuf-%PROTO_VER%.zip' -UseBasicParsing"
    ) else (
        curl.exe -L --fail --retry 3 --retry-delay 2 -o "%DEPS%\protobuf-%PROTO_VER%.zip" "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v%PROTO_VER%.zip"
    )
    if errorlevel 1 (
        echo ERROR: could not download protobuf.
        exit /b 1
    )
)

echo   extracting protobuf...
where tar.exe >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath '%DEPS%\protobuf-%PROTO_VER%.zip' -DestinationPath '%DEPS%' -Force"
) else (
    tar.exe -xf "%DEPS%\protobuf-%PROTO_VER%.zip" -C "%DEPS%"
)
if errorlevel 1 (
    echo ERROR: could not extract protobuf.
    exit /b 1
)
if not exist "%PROTO_SRC%\src\google\protobuf\message.h" (
    echo ERROR: protobuf was extracted, but the source tree is missing.
    exit /b 1
)

:bp_configure
echo   configuring protobuf (first build can take several minutes)...
"%CMAKE%" -S "%PROTO_SRC%" -B "%PROTO_BUILD%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF -Dprotobuf_MSVC_STATIC_RUNTIME=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "%DEPS%\protobuf-configure.log" 2>&1
if errorlevel 1 (
    echo ERROR: protobuf configure failed.
    call :tail "%DEPS%\protobuf-configure.log"
    exit /b 1
)

echo   building protobuf (this can take 5-10 minutes)...
"%CMAKE%" --build "%PROTO_BUILD%" > "%DEPS%\protobuf-build.log" 2>&1
if errorlevel 1 (
    echo ERROR: protobuf build failed.
    call :tail "%DEPS%\protobuf-build.log"
    exit /b 1
)

if not exist "%PROTO_BUILD%\protoc.exe" (
    echo ERROR: protoc.exe was not produced.
    exit /b 1
)
if not exist "%PROTO_BUILD%\libprotobuf.lib" (
    echo ERROR: libprotobuf.lib was not produced.
    exit /b 1
)
echo   protobuf is ready.
exit /b 0

:tail
if "%~1"=="" exit /b 0
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Content -LiteralPath '%~1' -Tail 40" 2>nul
exit /b 0

:update_offsets
if "%SKIP_OFFSETS%"=="1" goto :offsets_skipped
if not exist "%DUMPER%\dota2_dumper.exe" goto :offsets_missing_dumper

set "DUMP=%DUMPER%\dota2_dump"
set "DUMP_JSON=%DUMP%\offsets.json"
set "GAME_CLIENT=%DOTA_DIR%\game\dota\bin\win64\client.dll"
set "NEED_DUMP=0"
if not exist "%DUMP_JSON%" set "NEED_DUMP=1"
if "%FORCE_DUMP%"=="1" set "NEED_DUMP=1"
if "%NEED_DUMP%"=="1" goto :offsets_maybe_dump
call :dump_stale
if errorlevel 1 set "NEED_DUMP=1"

:offsets_maybe_dump
if "%NEED_DUMP%"=="0" goto :offsets_apply
tasklist /fi "imagename eq dota2.exe" 2>nul | find /i "dota2.exe" >nul
if errorlevel 1 goto :offsets_not_running
echo [0/4] Offsets: reading a fresh dump from the running game...
call :run_dumper
if not exist "%DUMP_JSON%" (
    echo   WARNING: the dumper did not produce "%DUMP_JSON%".
    exit /b 0
)
goto :offsets_apply

:offsets_not_running
echo [0/4] Offsets: Dota 2 is not running, the existing dump is used.
echo        Start the game and run "build.bat refresh" to update the offsets.
goto :offsets_apply

:offsets_apply
if not exist "%DUMP_JSON%" goto :offsets_no_dump
where py >nul 2>&1
if errorlevel 1 goto :offsets_no_python
if not exist "%BUILD%" mkdir "%BUILD%"
py -3 "%ROOT%\scripts\update_offsets.py" "%DUMP%" "%ROOT%\src\Misc\Offsets.h" > "%BUILD%\offsets.log" 2>&1
if errorlevel 1 goto :offsets_failed
type "%BUILD%\offsets.log"
exit /b 0

:offsets_failed
echo   WARNING: could not apply the dump, keeping the current offsets.
call :tail "%BUILD%\offsets.log"
exit /b 0

:offsets_no_dump
echo [0/4] Offsets: no dump in "%DUMP%", keeping the current offsets.
echo        Launch Dota 2 and run "build.bat refresh" to create one.
exit /b 0

:offsets_no_python
echo [0/4] Offsets: Python launcher "py" not found, keeping the current offsets.
exit /b 0

:offsets_skipped
echo [0/4] Offsets: skipped (nooffsets).
exit /b 0

:offsets_missing_dumper
echo [0/4] Offsets: dumper not found in "%DUMPER%", keeping the current offsets.
exit /b 0

:dump_stale
if not exist "%GAME_CLIENT%" exit /b 0
if not exist "%DUMP_JSON%" exit /b 1
set "DUMP_STATE=fresh"
for /f "usebackq delims=" %%r in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "if ((Get-Item -LiteralPath $env:DUMP_JSON).LastWriteTimeUtc -lt (Get-Item -LiteralPath $env:GAME_CLIENT).LastWriteTimeUtc) { 'stale' } else { 'fresh' }"`) do set "DUMP_STATE=%%r"
if /i "%DUMP_STATE%"=="stale" exit /b 1
exit /b 0

:run_dumper
pushd "%DUMPER%"
net session >nul 2>&1
if errorlevel 1 goto :run_dumper_elevated
"%DUMPER%\dota2_dumper.exe"
goto :run_dumper_done

:run_dumper_elevated
powershell -NoProfile -ExecutionPolicy Bypass -Command "$p = Start-Process -FilePath '%DUMPER%\dota2_dumper.exe' -WorkingDirectory '%DUMPER%' -Verb RunAs -PassThru; if ($p) { try { $p | Wait-Process -Timeout 180 -ErrorAction Stop } catch { }; if (-not $p.HasExited) { try { $p | Stop-Process -Force } catch { } } }"

:run_dumper_done
popd
exit /b 0

:clean
if exist "%BUILD%" (
    rmdir /s /q "%BUILD%"
    if exist "%BUILD%" (
        echo ERROR: could not remove "%BUILD%". Close programs that use the DLL and try again.
        exit /b 1
    )
)
echo Cleaned "%BUILD%".
exit /b 0

:rebuild
if exist "%BUILD%" rmdir /s /q "%BUILD%"
if exist "%PB_OUT%" rmdir /s /q "%PB_OUT%"
goto :build

:distclean
if exist "%BUILD%" rmdir /s /q "%BUILD%"
if exist "%PB_OUT%" rmdir /s /q "%PB_OUT%"
if exist "%DEPS%" rmdir /s /q "%DEPS%"
echo Removed build, generated protobufs and .deps.
exit /b 0

:help
echo Usage: build.bat [command] [nopause] [nooffsets]
echo.
echo Commands:
echo   (none)      Build the DLL and loader (refreshes offsets when possible).
echo   offsets     Force an offsets dump, apply it, then build.
echo   refresh     Force an offsets dump and apply it (no build).
echo   rebuild     Remove build output and generated protobufs, then build.
echo   clean       Remove the build directory only.
echo   distclean   Remove build, generated protobufs and downloaded dependencies.
echo   nooffsets   Build without touching src\Misc\Offsets.h.
echo   nopause     Build without waiting for a key at the end.
echo   help        Show this help.
echo.
echo Offsets:
echo   Dumper: %DUMPER%
echo   Dota:   %DOTA_DIR%
echo   Override with the ANCHOUSE_DUMPER / ANCHOUSE_DOTA_DIR environment variables
echo   (OVR_DUMPER / OVR_DOTA_DIR are still accepted for compatibility).
echo   A dump is taken from the running game before the build when the current
echo   dump is missing or older than client.dll; otherwise it is reused.
echo.
echo Requirements:
echo   - Visual Studio 2022/2026 with "Desktop development with C++"
echo   - CMake
echo   - Python 3 (py launcher) for the automatic offsets refresh
echo   - Internet access on the first build (protobuf and Dota 2 protos)
exit /b 0

:fail
echo.
echo Build failed. Check the messages and logs above.
if "%PAUSE%"=="1" pause
exit /b 1
