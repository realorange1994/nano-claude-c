# Interactive test script for NanoClaude-C
param(
    [Parameter(Mandatory=$false)]
    [string]$ApiKey = $env:ANTHROPIC_API_KEY
)

$ErrorActionPreference = "Continue"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "NanoClaude-C Interactive Test" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

if ([string]::IsNullOrEmpty($ApiKey)) {
    Write-Host "Error: No API key provided" -ForegroundColor Red
    Write-Host ""
    Write-Host "Usage:" -ForegroundColor Yellow
    Write-Host "  powershell -File test_chat.ps1 -ApiKey 'your-api-key'" -ForegroundColor White
    Write-Host "  Or set environment variable: `$env:ANTHROPIC_API_KEY = 'your-api-key'" -ForegroundColor White
    exit 1
}

Write-Host "API Key: $($ApiKey.Substring(0, [Math]::Min(10, $ApiKey.Length)))..." -ForegroundColor Gray
Write-Host ""

# Run nanoclaude.exe with interactive input
$pinfo = New-Object System.Diagnostics.ProcessStartInfo
$pinfo.FileName = "E:\Git\nanoclaude-c\nanoclaude.exe"
$pinfo.Arguments = "--api-key `"$ApiKey`""
$pinfo.RedirectStandardInput = $true
$pinfo.RedirectStandardOutput = $true
$pinfo.RedirectStandardError = $true
$pinfo.UseShellExecute = $false
$pinfo.CreateNoWindow = $true
$pinfo.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$pinfo.StandardErrorEncoding = [System.Text.Encoding]::UTF8

$p = New-Object System.Diagnostics.Process
$p.StartInfo = $pinfo
$p.Start() | Out-Null

# Send test message
$testMessage = "Hello! Please respond with a brief greeting."
Write-Host "[Sending] $testMessage" -ForegroundColor Yellow
$p.StandardInput.WriteLine($testMessage)

# Wait a bit for streaming response
Start-Sleep -Seconds 15

# Send quit command
if (!$p.HasExited) {
    Write-Host ""
    Write-Host "[Sending] /quit" -ForegroundColor Yellow
    $p.StandardInput.WriteLine("/quit")
}

$stdout = $p.StandardOutput.ReadToEnd()
$stderr = $p.StandardError.ReadToEnd()

# Wait for process to finish
if (!$p.HasExited) {
    $p.WaitForExit(30000)
    if (!$p.HasExited) {
        $p.Kill()
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Response:" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host $stdout

if ($stderr -match "Error|error|FAIL|fail") {
    Write-Host ""
    Write-Host "Errors/Warnings:" -ForegroundColor Red
    Write-Host $stderr
}

Write-Host ""
Write-Host "Exit code: $($p.ExitCode)" -ForegroundColor Gray
