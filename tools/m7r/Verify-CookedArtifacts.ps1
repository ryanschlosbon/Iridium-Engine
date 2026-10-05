# Re-cooks every M7R frozen-set model (M7RFixtures.ps1 $M7RModels) with a given
# IridiumCookAsset into an empty derived-data cache and compares the cook key and
# artifact hash with the recorded out/m7r/ddc/artifacts.json. A cold cook runs every
# importer path (glTF texture views, mips, BC compression, environment products), so
# identical hashes show that a change to how cooks are scheduled (M7R R5b.2) did not
# change the bytes. Harness tooling only; sources come from -DataRoot (local-only
# third-party models included).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/m7r/Verify-CookedArtifacts.ps1 `
#       -Cook <worktree>\out\build\x64-release\bin\IridiumCookAsset.exe -WorkDir C:\r5bb\ddc-verify
param(
    [Parameter(Mandatory)] [string] $Cook,
    [Parameter(Mandatory)] [string] $WorkDir,
    [string] $DataRoot = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'M7RFixtures.ps1')
. (Join-Path $PSScriptRoot 'M7RSweepCommon.ps1')
if (-not $DataRoot) { $DataRoot = Get-M7RMainRoot }
$DataRoot = (Resolve-Path $DataRoot).Path
$cookPath = (Resolve-Path $Cook).Path
$recorded = Get-Content (Join-Path $DataRoot 'out/m7r/ddc/artifacts.json') -Raw | ConvertFrom-Json
if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$ddc = (Resolve-Path $WorkDir).Path
$failures = 0
Push-Location $DataRoot
try {
    foreach ($name in $M7RModels.Keys) {
        $model = $M7RModels[$name]
        $metadata = if ($model.Metadata) { " --metadata `"$($model.Metadata)`"" } else { '' }
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $json = (cmd /c "`"$cookPath`" --source `"assets/$($model.Source)`"$metadata --ddc `"$ddc`" 2>nul") -join "`n"
        $exit = $LASTEXITCODE
        $watch.Stop()
        $expected = $recorded.$name
        if ($exit -ne 0) { Write-Host ("FAIL      {0,-12} cook exit {1}" -f $name, $exit); $failures++; continue }
        $result = $json | ConvertFrom-Json
        $same = $expected -and $result.cookKey -eq $expected.cookKey -and $result.artifactHash -eq $expected.artifactHash
        if (-not $same) { $failures++ }
        Write-Host ("{0,-9} {1,-12} {2,-6} {3:n1}s key {4} hash {5}" -f ($(if ($same) { 'identical' } else { 'CHANGED' })),
            $name, $result.status, $watch.Elapsed.TotalSeconds, $result.cookKey.Substring(0, 16), $result.artifactHash.Substring(0, 16))
    }
}
finally { Pop-Location }
Remove-Item -Recurse -Force $WorkDir
Write-Host "changed or failed: $failures"
if ($failures -ne 0) { exit 1 }
