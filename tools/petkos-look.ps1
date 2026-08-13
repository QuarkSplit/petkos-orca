<#
Petko's Orca: look at the running application, without taking the screen off whoever is using it.

This exists because the probes cannot answer the only question that finally matters. On
2026-08-13 every span said the plate board was working - the model built 36 rows, 37 items and
2913 px of content, and BoardRowLayout fired seventeen times a run - while the user was looking
at blank space, because the window was hidden. A probe proves a function ran. A picture proves a
person can see the result.

    pwsh -Command "& tools/petkos-look.ps1"                       # 36 plates
    pwsh -Command "& tools/petkos-look.ps1 -Plates 6 -Out six.png"

Two things it does deliberately:

  PW_RENDERFULLCONTENT (the 2) is what makes PrintWindow capture composited content rather than
  returning a blank client area - and PrintWindow captures WITHOUT bringing the window forward,
  so this never steals focus from what is being typed somewhere else.

  It drives the app through PetkosPerfDriver with quit=0, and asks for plate SWITCHES even when
  the picture does not need them: the driver builds plates straight on PartPlateList, which skips
  the notification the real "add plate" path sends, so without a switch the board is still
  describing one plate and the picture would libel the application.
#>
param([int]$Plates = 36, [string]$Out = 'board-check.png')

$ErrorActionPreference = 'Stop'
$repo = 'D:\Dev\petkos-orca'
$exe  = Join-Path $repo 'build\src\Release\orca-slicer.exe'
$dd   = Join-Path $repo 'datadir'

if (Get-Process -Name 'orca-slicer' -ErrorAction SilentlyContinue) { throw 'already running' }

$env:PETKOS_PERF        = '1'
$env:PETKOS_PERF_OUT    = Join-Path $repo 'perf-runs\look'
# switch=2: the driver builds plates straight on PartPlateList, which skips the sidebar
# notification the real "add plate" path sends. One plate switch makes the board reload,
# which is what the app does for a user within a second of adding a plate.
$env:PETKOS_PERF_SCRIPT = "plates=$Plates,warmup=20,orbit=40,switch=2,assign=0,board=0,drag=0,quit=0"

$p = Start-Process -FilePath $exe -ArgumentList @('--datadir', $dd) -PassThru
Write-Host "launched pid $($p.Id), waiting for it to settle..."

$deadline = (Get-Date).AddSeconds(90)
$hwnd = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    $p.Refresh()
    if ($p.HasExited) { throw 'app exited early' }
    if ($p.MainWindowHandle -ne 0) { $hwnd = $p.MainWindowHandle }
    if ($hwnd -ne [IntPtr]::Zero -and (Get-Date) -gt $deadline.AddSeconds(-60)) { break }
}
if ($hwnd -eq [IntPtr]::Zero) { $p.Kill(); throw 'no main window appeared' }
Start-Sleep -Seconds 8

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class Cap {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
}
"@
[void][Cap]::SetProcessDPIAware()
$r = New-Object Cap+RECT
[void][Cap]::GetWindowRect($hwnd, [ref]$r)
$w = $r.R - $r.L; $h = $r.B - $r.T
Write-Host "window ${w}x${h}"
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$ok = [Cap]::PrintWindow($hwnd, $dc, 2)
$g.ReleaseHdc($dc); $g.Dispose()
$path = Join-Path $repo $Out
$bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "PrintWindow=$ok -> $path"

$p.CloseMainWindow() | Out-Null
if (-not $p.WaitForExit(20000)) { $p.Kill() }
Remove-Item Env:PETKOS_PERF, Env:PETKOS_PERF_OUT, Env:PETKOS_PERF_SCRIPT -ErrorAction SilentlyContinue
Write-Host 'closed'
