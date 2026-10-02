# Measures clean configure/build and incremental rebuild times for one preset in a
# checkout (use a dedicated worktree: the clean step deletes its build directory).
# Incremental cases touch a translation unit, a common RHI header, a shader include,
# and one compute shader, then rebuild. Writes JSON to out/m7r/build-times/<Label>.
#
#   powershell -File tools/m7r/Measure-BuildTimes.ps1 -Label r0-release -Root out/m7r/worktrees/r0 -Preset x64-release
param(
    [Parameter(Mandatory)] [string] $Label,
    [Parameter(Mandatory)] [string] $Root,
    [string] $Preset = 'x64-release',
    [switch] $SkipClean
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevShell.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$checkout = (Resolve-Path $Root).Path
$outDir = Join-Path $repo "out/m7r/build-times"
New-Item -ItemType Directory -Force $outDir | Out-Null
$buildDir = Join-Path $checkout "out/build/$Preset"

function Measure-Step([string] $name, [scriptblock] $action) {
    # Native stderr (CMake warnings) must not terminate under Windows PowerShell 5.1.
    $ErrorActionPreference = 'Continue'
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $output = & $action 2>&1 | ForEach-Object { "$_" }
    $watch.Stop()
    $ErrorActionPreference = 'Stop'
    $steps = ($output | Where-Object { $_ -match '^\[(\d+)/(\d+)\]' } | Select-Object -Last 1)
    $total = if ($steps -match '^\[\d+/(\d+)\]') { [int]$Matches[1] } else { 0 }
    if ($LASTEXITCODE -ne 0) { $output | Select-Object -Last 20 | Write-Host; throw "$name failed" }
    Write-Host ("{0,-28} {1,8:n1} s  {2} ninja steps" -f $name, $watch.Elapsed.TotalSeconds, $total)
    [pscustomobject]@{ step = $name; seconds = [math]::Round($watch.Elapsed.TotalSeconds, 2); ninjaSteps = $total }
}

Push-Location $checkout
try {
    $results = @()
    if (-not $SkipClean) {
        if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
        $results += Measure-Step 'configure (clean)' { cmake --preset $Preset }
        $results += Measure-Step 'build (clean)' { cmake --build $buildDir }
    }
    $results += Measure-Step 'no-op build' { cmake --build $buildDir }
    $touches = [ordered]@{
        'touch Application.cpp'      = @('src/app/Application.cpp', 'src/core/Application.cpp')
        'touch rhi/Mesh.h'           = @('src/renderer/rhi/Mesh.h')
        'touch shadow_filter.glsl'   = @('assets/shaders/include/shadow_filter.glsl')
        'touch cluster_count.comp'   = @('assets/shaders/cluster_count.comp')
    }
    foreach ($name in $touches.Keys) {
        # First existing candidate, so the same script measures pre- and post-R1 layouts.
        $file = $touches[$name] | Where-Object { Test-Path $_ } | Select-Object -First 1
        (Get-Item $file).LastWriteTime = Get-Date
        $results += Measure-Step $name { cmake --build $buildDir }
    }
    $report = [pscustomobject]@{
        label = $Label; preset = $Preset; checkout = $checkout
        commit = (git rev-parse HEAD).Trim(); measured = (Get-Date).ToString('o')
        cpu = (Get-CimInstance Win32_Processor).Name; results = $results
    }
    $report | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $outDir "$Label.json")
}
finally { Pop-Location }
