# Cooks the M9 temporal fixture models (M9Fixtures.ps1 $M9Models) into out/m9/ddc and writes
# out/m9/ddc/artifacts.json (model name -> source, artifact path, cook key, artifact hash).
# Re-running is a cache hit. Sources are engine-authored (tools/m9/Generate-TemporalAssets.py).
#
#   powershell -File tools/m9/Cook-TemporalModels.ps1
param([string] $Cook = 'out/build/x64-release/bin/IridiumCookAsset.exe')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'M9Fixtures.ps1')
$root = Get-M9RepoRoot
Push-Location $root
try {
    $ddc = 'out/m9/ddc'
    New-Item -ItemType Directory -Force $ddc | Out-Null
    $map = [ordered]@{}
    foreach ($name in $M9Models.Keys) {
        $model = $M9Models[$name]
        $source = $model.Source
        $metadata = if ($model.Metadata) { " --metadata `"$($model.Metadata)`"" } else { '' }
        $json = (cmd /c "`"$Cook`" --source `"assets/$source`"$metadata --ddc `"$ddc`" 2>nul") -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "Cook failed for ${name}:`n$json" }
        $result = $json | ConvertFrom-Json
        if ($result.status -notin @('built', 'cache-hit')) { throw "Cook status $($result.status) for $source" }
        $key = $result.cookKey
        $path = "$ddc/$($key.Substring(0, 2))/$($key.Substring(2)).irartifact"
        if (-not (Test-Path $path)) { throw "Artifact not found at $path" }
        $map[$name] = [ordered]@{ source = $source; metadata = $model.Metadata; artifact = $path; cookKey = $key; artifactHash = $result.artifactHash; sourceHash = $result.sourceHash }
        Write-Host ("{0,-14} {1,-42} {2} {3}" -f $name, $source, $result.status, $key.Substring(0, 16))
    }
    $map | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$ddc/artifacts.json"
}
finally { Pop-Location }
