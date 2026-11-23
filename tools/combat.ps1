# Combat test: boots into CaSpine, attacks and jumps (square/triangle/circle), and saves a frame every
# -Every vsyncs from -From to -To into frames/, then a contact sheet to -Out.
#   powershell -File tools\combat.ps1 -Out sheet.png [-Env "PS2_VUFLAGS=1"] [-From 6100] [-To 6300] [-Every 10]
param([string]$Out = "combat.png", [string]$Env = "", [int]$From = 6100, [int]$To = 6300, [int]$Every = 10)
Set-Location (Join-Path $PSScriptRoot "..")
Remove-Item frames\*.bmp -ErrorAction SilentlyContinue
$vars = @{ PS2_NOWINDOW = '1'; PS2_SHOT = "$Every";
           PS2_PADSCRIPT = 'start@4000:8,cross@4400:8,cross@4700:8,cross@5000:8,cross@5300:8,square@6000:6,square@6040:6,square@6080:6,triangle@6140:6,triangle@6180:6,circle@6240:6' }
foreach ($kv in ($Env -split ',' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; $vars[$k] = $v }
foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
$p = Start-Process build\eragon.exe -ArgumentList '..\SLUS_213.22' -NoNewWindow -PassThru -RedirectStandardError combat.log
$last = "frames\f{0:d5}.bmp" -f ([int]([math]::Floor($To / $Every) * $Every))
$t0 = Get-Date
while (-not (Test-Path $last) -and ((Get-Date) - $t0).TotalSeconds -lt 900) { Start-Sleep 5 }
if (-not $p.HasExited) { $p.Kill() }
foreach ($k in $vars.Keys) { Remove-Item "env:$k" }
$f = Get-ChildItem frames\*.bmp | Where-Object { $n = [int]($_.BaseName.Substring(1)); $n -ge $From -and $n -le $To } | Sort-Object { [int]($_.BaseName.Substring(1)) }
python -I tools\frames2png.py $Out @($f.FullName)
"$($f.Count) frames -> $Out"
