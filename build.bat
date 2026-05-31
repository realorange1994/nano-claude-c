@echo off
setlocal enabledelayedexpansion
cd /d %~dp0
echo Building nanoclaude.exe...

set GCC=E:\Git\mingw64\bin\gcc.exe
set SRC_FILES=
for %%f in (src\*.c) do set SRC_FILES=!SRC_FILES! %%f

%GCC% -Wall -Wextra -std=c11 -O2 -Isrc -Ideps\cJSON deps\cJSON\cJSON.c !SRC_FILES! -lwinhttp -lws2_32 -lshlwapi -o nanoclaude.exe

if %ERRORLEVEL% == 0 (
    echo Done! nanoclaude.exe built successfully.
) else (
    echo Build failed!
    exit /b 1
)
