# Starts LmCoop and follows its run log, showing only problems. See LmCoop-Monitor.cmd.
$dir = $PSScriptRoot
$exe = Join-Path $dir 'LmCoop.exe'
$data = if (Test-Path (Join-Path $dir 'last_run.log')) { $dir } else { Join-Path $env:APPDATA "Luigi's Mansion Recompiled" }  # portable installs keep the log beside the exe
$log = Join-Path $data 'last_run.log'
$state = Join-Path $data 'last_run_state.ini'
$bad = 'warning|error|fail|exception|crash|fatal|hang|abort|0xC0|denied|could not|timed out'

$Host.UI.RawUI.WindowTitle = 'LmCoop monitor'
Write-Host "LmCoop monitor - showing warnings, errors and crashes from $log" -ForegroundColor Cyan
if (-not (Test-Path $exe)) { Write-Host "LmCoop.exe not found next to this file." -ForegroundColor Red; cmd /c pause; exit 1 }
$p = Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru

# the game may restart itself (updates), so "running" means any LmCoop from this folder
function Running { [bool](Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($dir, 'OrdinalIgnoreCase') -and $_.Name -match '^(LmCoop|LuigisMansionRecomp)$' }) }

$pos = 0L; $shown = 0
Start-Sleep -Milliseconds 800
while ($true) {
    $alive = Running
    if (Test-Path $log) {
        try {
            $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite,Delete')
            if ($fs.Length -lt $pos) { $pos = 0; Write-Host '--- new run ---' -ForegroundColor DarkGray }  # the log restarted
            $fs.Seek($pos, 'Begin') | Out-Null
            $rd = New-Object IO.StreamReader($fs)
            while ($null -ne ($line = $rd.ReadLine())) {
                if ($line -match $bad) { Write-Host $line -ForegroundColor $(if ($line -match 'crash|exception|fatal|0xC0') { 'Red' } else { 'Yellow' }); $shown++ }
            }
            $pos = $fs.Position; $rd.Dispose(); $fs.Dispose()
        } catch { }
    }
    if (-not $alive) { break }
    Start-Sleep -Milliseconds 400
}

$crashed = (Test-Path $state) -and ((Get-Content $state -Raw) -match '(?m)^state=crashed')
Write-Host ''
if ($crashed -or ($p -and $p.HasExited -and $p.ExitCode -ne 0 -and $p.ExitCode -ne $null)) {
    Write-Host '=== GAME CRASHED ===' -ForegroundColor Red
    if ($crashed) { (Get-Content $state | Where-Object { $_ -match '^(reason|phase|version)=' }) | ForEach-Object { Write-Host $_ -ForegroundColor Red } }
    Write-Host "Log: $log (the next start offers to send a crash report)" -ForegroundColor Red
    cmd /c pause
} else {
    Write-Host "Game closed normally. $shown problem line(s) were shown." -ForegroundColor Green
    if ($shown -gt 0) { Start-Sleep -Seconds 5 }
}
