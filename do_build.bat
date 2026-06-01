@echo off
cd /d "E:\Git\nanoclaude-c"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

echo === STEP 1: Compiling ===
cl /O2 /Isrc /Ideps\cJSON /c deps\cJSON\cJSON.c src\*.c 2>build_err.txt
if %ERRORLEVEL% neq 0 (
    echo COMPILE FAILED!
    type build_err.txt
    exit /b 1
)

echo === STEP 2: Linking ===
link cJSON.obj main.obj config.obj provider.obj tool.obj repl.obj history.obj http.obj tool_impl.obj buffer.obj rgrep.obj glob.obj jsonrpc.obj calc.obj winhttp.lib ws2_32.lib shlwapi.lib /out:nanoclaude.exe 2>>build_err.txt
if %ERRORLEVEL% neq 0 (
    echo LINK FAILED!
    type build_err.txt
    exit /b 1
)

echo === STEP 3: Cleanup ===
del /q *.obj *.exp *.lib *.o 2>nul
if exist nanoclaude.exe (
    echo === BUILD SUCCESSFUL ===
    dir nanoclaude.exe
) else (
    echo BUILD FAILED: nanoclaude.exe not found
    exit /b 1
)
