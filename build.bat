@echo off
cd /d %~dp0
echo Building nanoclaude.exe...

E:\Git\mingw64\bin\gcc.exe -Wall -Wextra -std=c11 -O2 -Isrc -Ideps/cJSON deps/cJSON/cJSON.c src/*.c -lwinhttp -lws2_32 -lshlwapi -o nanoclaude.exe

if %ERRORLEVEL% == 0 (
    echo Done! nanoclaude.exe built successfully.
) else (
    echo Build failed!
)
