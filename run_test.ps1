$env:NANOCLAUDE_TEST = "1"
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "E:\Git\nanoclaude-c\nanoclaude.exe"
$psi.Arguments = "--provider openai --base-url `"https://bx5b2c2ngm.coze.site`" --model M2.7 --api-key `"sk-WhOYYcJbcrWQaBnkN3eKhGoT6P0YMJSlY4KVpZlt2mgQJkOV`""
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8

$proc = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Seconds 1

# Send test message
$proc.StandardInput.WriteLine("Hi")
$proc.StandardInput.Close()

# Wait for completion
$timeout = 45
$proc.WaitForExit($timeout * 1000)

$stdout = $proc.StandardOutput.ReadToEnd()
$stderr = $proc.StandardError.ReadToEnd()

Write-Host "========================================"
Write-Host "STDOUT:" -ForegroundColor Green
Write-Host "========================================"
Write-Host $stdout
if ($stderr -and $stderr.Trim()) {
    Write-Host ""
    Write-Host "========================================"
    Write-Host "STDERR:" -ForegroundColor Yellow
    Write-Host "========================================"
    Write-Host $stderr
}
Write-Host ""
Write-Host "Exit Code:" $proc.ExitCode
