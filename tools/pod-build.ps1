<# Podslicer target build. Uses the same bounded build policy as pod build. #>
[CmdletBinding()]
param(
    [Parameter(Position=0)] [string] $Target = 'OrcaSlicer',
    [string] $Config = 'Release',
    [string] $BuildDir = '',
    [ValidateRange(1,4)] [int] $Jobs = 2,
    [switch] $Quiet,
    [switch] $DryRun
)
$buildTargets = if ($Target -eq 'OrcaSlicer') { @('OrcaSlicer', 'OrcaSlicer_app_gui') } else { @($Target) }
& (Join-Path $PSScriptRoot 'petkos-dev-build.ps1') -Targets $buildTargets -Config $Config -BuildDir $BuildDir -Jobs $Jobs -DryRun:$DryRun
