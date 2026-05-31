$ErrorActionPreference = "Stop"

$config = @{
    api_key = "sk-WhOYYcJbcrWQaBnkN3eKhGoT6P0YMJSlY4KVpZlt2mgQJkOV"
    base_url = "https://bx5b2c2ngm.coze.site"
    model = "M2.7"
    provider = "openai"
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "NanoClaude-C Interactive Test" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Config:" -ForegroundColor Yellow
Write-Host "  Provider: $($config.provider)"
Write-Host "  Model: $($config.model)"
Write-Host "  Base URL: $($config.base_url)"
Write-Host ""

# Create process with interactive input
$pinfo = New-Object System.Diagnostics.ProcessStartInfo
$pinfo.FileName = "E:\Git\nanoclaude-c\nanoclaude.exe"
$pinfo.Arguments = "--provider openai --base-url `"$($config.base_url)`" --model `"$($config.model)`" --api-key `"$($config.api_key)`""
$pinfo.RedirectStandardInput = $false
$pinfo.RedirectStandardOutput = $true
$pinfo.RedirectStandardOutput = $true
$pinfo.UseShellExecute = $false
$pinfo.CreateNoWindow = $false

# Actually use interactive mode
$pinfo.RedirectStandardInput = $false

$p = New-Object System.Diagnostics.Process
$p.StartInfo = $pinfo
$p.Start() | Out-Null

# Send input after a short delay
Start-Sleep -Seconds 2

if (!$p.HasExited) {
    $p.StandardInput.WriteLine("Hello, say hi back briefly.")
    Start-Sleep -Seconds 5
}

if (!$p.HasExited) {
    $p.StandardInput.WriteLine("/quit")
}

$timeout = 60
$p.WaitForExit($timeout * 1000) | Out-Null

if (!$p.HasExited) {
    Write-Host "Process timed out, killing..." -ForegroundColor Red
    $p.Kill()
}

$stdout = $p.StandardOutput.ReadToEnd()

Write-Host ""
Write-Host "----------------------------------------" -ForegroundColor Cyan
Write-Host "Output:" -ForegroundColor Green
Write-Host "----------------------------------------" -ForegroundColor Cyan
Write-Host $stdout
Write-Host ""
Write-Host "Exit Code: $($p.ExitCode)" -ForegroundColor Gray
