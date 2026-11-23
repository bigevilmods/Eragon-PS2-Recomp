# Movement test: boots into CaSpine, holds the left stick up for a while, and reports how much the upper
# half of the screen (background) changed between frames while running. Near 0 = the player did not move.
#   powershell -File tools\movetest.ps1 [-Env "PS2_VUFLAGS=1"]
param([string]$Env = "", [int]$Seconds = 215)
Set-Location (Join-Path $PSScriptRoot "..")
Remove-Item frames\*.bmp -ErrorAction SilentlyContinue
$vars = @{ PS2_NOWINDOW = '1'; PS2_SHOT = '150';
           PS2_PADSCRIPT = 'start@4000:8,cross@4400:8,cross@4700:8,cross@5000:8,cross@5300:8,lup@6200:1200' }
foreach ($kv in ($Env -split ',' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; $vars[$k] = $v }
foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
$p = Start-Process build\eragon.exe -ArgumentList '..\SLUS_213.22' -NoNewWindow -PassThru -RedirectStandardError movetest.log
if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill() }
foreach ($k in $vars.Keys) { Remove-Item "env:$k" }
$f = Get-ChildItem frames\*.bmp | Where-Object { [int]($_.BaseName.Substring(1)) -ge 6300 } | Sort-Object { [int]($_.BaseName.Substring(1)) }
if ($f.Count -lt 2) { "not enough frames"; exit }
python -I tools\framediff.py @($f.FullName)
