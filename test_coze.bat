@echo off
chcp 65001 >nul
set API_KEY=sk-WhOYYcJbcrWQaBnkN3eKhGoT6P0YMJSlY4KVpZlt2mgQJkOV
set BASE_URL=https://bx5b2c2ngm.coze.site
set MODEL=M2.7

echo ========================================
echo NanoClaude-C Test with Coze Proxy
echo ========================================
echo.

(echo Hello, say hi back.) | nanoclaude.exe --provider openai --base-url "%BASE_URL%" --model "%MODEL%" --api-key "%API_KEY%"

echo.
echo ========================================
echo Test complete
echo ========================================
