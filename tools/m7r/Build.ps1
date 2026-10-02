# Configures (when needed) and builds a preset inside the MSVC developer environment,
# optionally running ctest. Prints only diagnostics and the tail on failure.
#
#   powershell -ExecutionPolicy Bypass -File tools/m7r/Build.ps1 -Preset x64-release [-Configure] [-Test] [-Target IridiumEngine]
param(
    [string] $Preset = 'x64-release',
    [switch] $Configure,
    [switch] $Test,
    [string] $Target = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevShell.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Push-Location $repo
try {
    $ErrorActionPreference = 'Continue'
    $buildDir = "out/build/$Preset"
    if ($Configure -or -not (Test-Path "$buildDir/CMakeCache.txt")) {
        $out = cmake --preset $Preset 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0) { $out | Select-Object -Last 40; exit 1 }
    }
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $buildArgs = @('--build', $buildDir)
    if ($Target) { $buildArgs += @('--target', $Target) }
    $out = cmake @buildArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $watch.Stop()
    $out | Where-Object { $_ -match '(error|warning) [A-Z]+\d+|CMake Error|FAILED:|LINK : fatal' } | Select-Object -First 60
    if ($code -ne 0) { $out | Select-Object -Last 15; Write-Host "BUILD FAILED ($Preset)"; exit 1 }
    Write-Host ("build ok ({0}) in {1:n1}s" -f $Preset, $watch.Elapsed.TotalSeconds)
    if ($Test) {
        $out = ctest --test-dir $buildDir -j 8 --output-on-failure 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        $out | Where-Object { $_ -match 'Failed|\*\*\*|tests passed|tests failed' } | Select-Object -First 40
        if ($code -ne 0) { Write-Host "TESTS FAILED ($Preset)"; exit 1 }
    }
}
finally { Pop-Location }
