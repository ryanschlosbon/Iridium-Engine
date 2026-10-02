# Captures the M7R frozen set (scene-linear PFM + final-SDR TGA per fixture) and
# writes hashes.json plus a markdown summary into -OutDir. Harness tooling only.
#
#   powershell -File tools/m7r/Run-FrozenCaptures.ps1 -Label r0-release
#   powershell -File tools/m7r/Run-FrozenCaptures.ps1 -Label r1 -Validation -Only F4-ord2,F5-hetero
param(
    [Parameter(Mandatory)] [string] $Label,
    [string] $Exe = 'out/build/x64-release/bin/IridiumEngine.exe',
    [switch] $Validation,
    [string[]] $Only = @(),
    [string[]] $Points = @('scene', 'final-sdr'),
    [string[]] $ExtraArgs = @()
)
$ErrorActionPreference = 'Stop'
# powershell -File passes comma lists as one string; accept both forms.
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$ExtraArgs = @($ExtraArgs | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
$Points = @($Points | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'M7RFixtures.ps1')
$root = Get-M7RRepoRoot
Push-Location $root
try {
    $exePath = (Resolve-Path $Exe).Path
    $outDir = Join-Path $root "out/m7r/captures/$Label"
    if (Test-Path $outDir) { throw "Output directory already exists: $outDir (captures are never overwritten)" }
    New-Item -ItemType Directory -Force $outDir | Out-Null

    $results = @()
    foreach ($fixture in $M7RFrozenSet) {
        if ($Only.Count -gt 0 -and $Only -notcontains $fixture.Key) { continue }
        foreach ($point in $Points) {
            $captureDir = Join-Path $outDir "$($fixture.Key)/$point"
            New-Item -ItemType Directory -Force $captureDir | Out-Null
            $log = Join-Path $outDir "$($fixture.Key)__$point.log"
            $artifact = Get-M7RModelArtifact $root $fixture.Model
            $arguments = @(
                '--benchmark', $fixture.Id, '--benchmark-manifest', $fixture.Manifest,
                '--cooked-model-artifact', $artifact,
                '--window-size', '3840x2160', '--hidden-window', '--borderless-window',
                '--output-transport', 'sdr',
                '--warmup-frames', '12', '--frame-limit', '6',
                '--capture-frame', '4', '--capture-directory', $captureDir,
                '--capture-point', $point, '--require-capture-signal',
                $(if ($Validation) { '--validation' } else { '--no-validation' })
            ) + $fixture.Args + $ExtraArgs
            $started = Get-Date
            $exit = Invoke-M7REngine $exePath $arguments $log
            $seconds = ((Get-Date) - $started).TotalSeconds
            $validationLines = @(Select-String -Path $log -Pattern '^\[Validation\]:' -ErrorAction SilentlyContinue).Count
            $sidecar = Get-ChildItem $captureDir -Filter '*.json' | Select-Object -First 1
            $sha = $null; $stem = $null
            if ($sidecar) {
                $meta = Get-Content $sidecar.FullName -Raw | ConvertFrom-Json
                $sha = $meta.image.sha256
                $stem = $sidecar.BaseName
            }
            $results += [pscustomobject]@{
                key = $fixture.Key; fixture = $fixture.Id; point = $point; sha256 = $sha
                exit = $exit; validationMessages = $validationLines; seconds = [math]::Round($seconds, 1)
                stem = $stem; args = ($fixture.Args -join ' ')
            }
            $status = if ($exit -eq 0 -and $sha) { 'ok' } else { 'FAILED' }
            Write-Host ("{0,-10} {1,-9} {2} {3} validation={4}" -f $fixture.Key, $point, $status, $(if ($sha) { $sha.Substring(0, 16) } else { '-' }), $validationLines)
        }
    }

    $git = (git rev-parse HEAD).Trim()
    $dirty = [bool](git status --porcelain --untracked-files=no | Where-Object { $_ -notmatch 'imgui\.ini$' })
    $exeSha = (Get-FileHash $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $report = [pscustomobject]@{
        label = $Label; commit = $git; dirtyTrackedFiles = $dirty; executable = $Exe; executableSha256 = $exeSha
        validation = [bool]$Validation; extraArgs = $ExtraArgs; captured = (Get-Date).ToString('o'); results = $results
    }
    $report | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $outDir 'hashes.json')

    $md = @("| Fixture | Point | SHA-256 (16) | Exit | Validation msgs |", "|---|---|---|---:|---:|")
    foreach ($r in $results) {
        $md += "| $($r.key) ``$($r.fixture)`` $($r.args) | $($r.point) | ``$(if ($r.sha256) { $r.sha256.Substring(0, 16) } else { '-' })`` | $($r.exit) | $($r.validationMessages) |"
    }
    $md | Set-Content -Encoding utf8 (Join-Path $outDir 'summary.md')
    $failed = @($results | Where-Object { $_.exit -ne 0 -or -not $_.sha256 }).Count
    Write-Host "Wrote $outDir (commit $git, failures $failed)"
    if ($failed -gt 0) { exit 1 }
}
finally { Pop-Location }
