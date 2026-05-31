@echo off
chcp 65001 >nul
echo ========================================
echo NanoClaude-C Interactive Test
echo ========================================
echo.

set API_KEY=%ANTHROPIC_API_KEY%
if "%API_KEY%"=="" (
    echo Error: ANTHROPIC_API_KEY environment variable not set
    echo Please set it with: set ANTHROPIC_API_KEY=your_api_key
    exit /b 1
)

echo Testing with API key: %API_KEY:~0,10%...
echo.

echo hello | nanoclaude.exe --api-key "%API_KEY%" 2>&1
