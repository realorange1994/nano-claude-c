# NanoClaude-C Test Script
# Usage: Provide your API key to test the actual AI interaction

param(
    [Parameter(Mandatory=$false)]
    [string]$ApiKey = $env:ANTHROPIC_API_KEY
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  NanoClaude-C Integration Test" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

if ([string]::IsNullOrEmpty($ApiKey)) {
    Write-Host "ERROR: No API key provided" -ForegroundColor Red
    Write-Host ""
    Write-Host "Please provide your Anthropic API key:" -ForegroundColor Yellow
    Write-Host "  - Set `$env:ANTHROPIC_API_KEY = 'your-key'" -ForegroundColor White
    Write-Host "  - Or pass as parameter: -ApiKey 'your-key'" -ForegroundColor White
    exit 1
}

Write-Host "API Key: $($ApiKey.Substring(0, [Math]::Min(15, $ApiKey.Length)))..." -ForegroundColor Gray
Write-Host ""

# Create a simple test using stdin redirect
$testInput = @"
Hello! Just say hi back.
quit
"@

$bytes = [System.Text.Encoding]::UTF8.GetBytes($testInput)
[System.IO.File]::WriteAllBytes("$env:TEMP\nanoclaude_test_input.txt", $bytes)

Write-Host "Running nanoclaude.exe with test input..." -ForegroundColor Yellow
Write-Host ""

$exePath = "E:\Git\nanoclaude-c\nanoclaude.exe"
$argString = "--api-key `"$ApiKey`""

# Start process
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exePath
$psi.Arguments = $argString
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8

$proc = [System.Diagnostics.Process]::Start($psi)

# Write input
$proc.StandardInput.Write($testInput)
$proc.StandardInput.Close()

# Wait with timeout
$timeout = 45
$elapsed = 0
while (!$proc.HasExited -and $elapsed -lt $timeout) {
    Start-Sleep -Milliseconds 500
    $elapsed += 0.5
}

$stdout = ""
$stderr = ""

if ($proc.HasExited) {
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
} else {
    Write-Host "Process timed out, killing..." -ForegroundColor Red
    $proc.Kill()
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
}

Write-Host "----------------------------------------" -ForegroundColor Cyan
Write-Host "STDOUT:" -ForegroundColor Green
Write-Host "----------------------------------------" -ForegroundColor Cyan
Write-Host $stdout

if ($stderr -and $stderr.Trim()) {
    Write-Host ""
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    Write-Host "STDERR:" -ForegroundColor Yellow
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    Write-Host $stderr
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Exit Code: $($proc.ExitCode)" -ForegroundColor Gray
Write-Host "========================================" -ForegroundColor Cyan

# Check for errors
if ($stderr -match "Error|error|ERROR|Exception|exception") {
    Write-Host ""
    Write-Host "ERRORS DETECTED IN STDERR!" -ForegroundColor Red
    exit 1
}

if ($stdout -match "Error:|error:|failed") {
    Write-Host ""
    Write-Host "ERRORS DETECTED IN OUTPUT!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Test completed successfully!" -ForegroundColor Green
exit 0
