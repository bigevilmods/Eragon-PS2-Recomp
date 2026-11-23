# In-level speed benchmark: boots into CaSpine with the scripted menu path (no movement),
# runs for $Seconds and reports vsyncs/s over the last 60 s (from the watchdog lines).
#   powershell -File tools\bench.ps1 [-Seconds 240] [-Env "PS2_GS_THREADS=3,PS2_VUINTERP=1"]
param([int]$Seconds = 240, [string]$Env = "")
Set-Location (Join-Path $PSScriptRoot "..")
$vars = @{ PS2_NOWINDOW = '1'; PS2_PADSCRIPT = 'start@4000:8,cross@4400:8,cross@4700:8,cross@5000:8,cross@5300:8' }
foreach ($kv in ($Env -split ',' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; $vars[$k] = $v }
foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
$p = Start-Process build\eragon.exe -ArgumentList '..\SLUS_213.22' -NoNewWindow -PassThru -RedirectStandardError bench.log
if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill() }
foreach ($k in $vars.Keys) { Remove-Item "env:$k" }
$w = Select-String bench.log -Pattern '^\[watch\] t=(\d+)s .*vsyncs=(\d+)' | ForEach-Object { [pscustomobject]@{ t = [int]$_.Matches[0].Groups[1].Value; v = [int]$_.Matches[0].Groups[2].Value } }
$last = $w[-1]; $ref = $w | Where-Object { $_.t -le $last.t - 60 } | Select-Object -Last 1
$rate = ($last.v - $ref.v) / ($last.t - $ref.t)
$respawn = (Select-String bench.log -Pattern 'Loading respawn' | Select-Object -First 1)
"{0}: {1:N1} vsyncs/s over t={2}..{3}s (vsync {4}..{5}){6}" -f ($(if ($Env) { $Env } else { 'default' })), $rate, $ref.t, $last.t, $ref.v, $last.v, $(if ($respawn) { '' } else { '  [level not reached]' })
