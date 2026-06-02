@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d "E:\Git\nanoclaude-c"

echo === Build with /O1 ===
del /q *.obj *.exp *.lib 2>nul
cl /nologo /O1 /MT /Gy /Isrc /Ideps\cJSON /c deps\cJSON\cJSON.c src\*.c 2>build_err.txt
if %ERRORLEVEL% neq 0 ( echo COMPILE FAILED & type build_err.txt & exit /b 1 )
link /nologo /OPT:REF /OPT:ICF cJSON.obj main.obj config.obj provider.obj tool.obj repl.obj history.obj http.obj tool_impl.obj buffer.obj rgrep.obj glob.obj jsonrpc.obj calc.obj system_prompt.obj winhttp.lib ws2_32.lib shlwapi.lib /out:nanoclaude.exe 2>>build_err.txt
if %ERRORLEVEL% neq 0 ( echo LINK FAILED & type build_err.txt & exit /b 1 )

del /q *.obj *.exp *.lib *.o 2>nul
echo BUILD SUCCESSFUL
dir nanoclaude.exe
