@echo off
REM SneekPeek build script — picks MSVC (cl) if present, else MinGW (g++).
REM Usage: build.bat        (Release exe -> .\SneekPeek.exe)
setlocal

set OUT=SneekPeek.exe
set SRC=src\main.cpp src\app_index.cpp
set RES_OBJS=

where cl >nul 2>nul
if %ERRORLEVEL%==0 (
    echo [build] Using MSVC (cl)
    where rc >nul 2>nul
    if not errorlevel 1 (
        del src\app.res 2>nul
        pushd src
        rc /fo app.res app.rc
        popd
        if exist src\app.res set RES_OBJS=src\app.res
    )
    cl /std:c++17 /O1 /Os /EHsc /utf-8 /DUNICODE /D_UNICODE /MT %SRC% %RES_OBJS% ^
       /link user32.lib shell32.lib gdi32.lib advapi32.lib shlwapi.lib comctl32.lib comdlg32.lib powrprof.lib ole32.lib ^
       /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:%OUT%
    if %ERRORLEVEL%==0 echo [build] OK -^> %OUT%
    exit /b %ERRORLEVEL%
)

where g++ >nul 2>nul
if %ERRORLEVEL%==0 (
    echo [build] Using MinGW (g++)
    where windres >nul 2>nul
    if not errorlevel 1 (
        del src\app.o 2>nul
        pushd src
        windres app.rc -o app.o
        popd
        if exist src\app.o set RES_OBJS=src\app.o
    )
    g++ -std=c++17 -Os -s -DUNICODE -D_UNICODE -finput-charset=UTF-8 %SRC% %RES_OBJS% -o %OUT% ^
        -mwindows -municode -luser32 -lshell32 -lgdi32 -ladvapi32 -lshlwapi -lcomctl32 -lcomdlg32 -lpowrprof -lole32 -static
    if %ERRORLEVEL%==0 echo [build] OK -^> %OUT%
    exit /b %ERRORLEVEL%
)

echo [build] No C++ compiler found in PATH.
echo [build] Install one of:
echo [build]   winget install BrechtSanders.WinLibs.POSIX.UCRT   (MinGW, lightweight)
echo [build]   winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools"
echo [build] Then re-run build.bat from a terminal with the compiler on PATH.
exit /b 1
