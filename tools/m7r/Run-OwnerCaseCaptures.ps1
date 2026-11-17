# Captures the M7 completion owner-case fixtures ($M7COwnerCases in M7RFixtures.ps1;
# docs/milestones/M7-completion.md, M7.P1) at native 4K: one Release process per fixture
# and capture point, with --require-capture-signal. A case marked ConstantImage (camera
# turned away from every instance) captures the constant environment by design: it runs
# without the signal check and fails instead unless the image is constant. Writes
# hashes.json, summary.md and, with -Previews, a downscaled PNG of every final-sdr capture
# (Python + Pillow) under previews/ for inspection by eye. Harness tooling only; never a
# timing run.
#
#   powershell -File tools/m7r/Run-OwnerCaseCaptures.ps1 -Label m7c-p1-fixtures -Previews
#   ... -Label pc3-check -Only PC3-nolight,PC3-point-r10 -Points final-sdr -Previews
#   ... -Label pc1-validation -Only PC1-911-n16-all -Points final-sdr -Validation
param(
    [Parameter(Mandatory)] [string] $Label,
    [string] $Exe = 'out/build/x64-release/bin/IridiumEngine.exe',
    [switch] $Validation,
    [string[]] $Only = @(),
    [string[]] $Points = @('scene', 'final-sdr'),
    [string[]] $ExtraArgs = @(),
    [switch] $Previews,
    [string] $PreviewSize = '960x540',
    # 'off' (default) or a cache directory; see Get-M7RPipelineCacheArgs.
    [string] $PipelineCache = 'off'
)
$ErrorActionPreference = 'Stop'
# powershell -File passes comma lists as one string; accept both forms.
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$ExtraArgs = @($ExtraArgs | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
$Points = @($Points | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'M7RFixtures.ps1')
$root = Get-M7RRepoRoot
$artifactRoot = Get-M7RArtifactRoot $root
$unknown = @($Only | Where-Object { $k = $_; -not ($M7COwnerCases | Where-Object { $_.Key -eq $k }) })
if ($unknown.Count -gt 0) { throw "Unknown owner case(s): $($unknown -join ', ')" }
Push-Location $root
try {
    $exePath = (Resolve-Path $Exe).Path
    $baseArgs = @(Get-M7REngineBaseArgs $exePath $PipelineCache)
    $outDir = Join-Path $root "out/m7r/captures/$Label"
    if (Test-Path $outDir) { throw "Output directory already exists: $outDir (captures are never overwritten)" }
    New-Item -ItemType Directory -Force $outDir | Out-Null
    # Prints True when every pixel of a capture (PFM or TGA) is equal.
    $constantCheck = Join-Path $outDir 'check-constant.py'
    Set-Content -Encoding ascii $constantCheck @'
import sys
import numpy as np
from PIL import Image
path = sys.argv[1]
if path.endswith('.pfm'):
    data = open(path, 'rb').read()
    header = data.split(b'\n', 3)
    order = '<f4' if float(header[2]) < 0 else '>f4'
    pixels = np.frombuffer(header[3], dtype=order).reshape(-1, 3)
else:
    pixels = np.asarray(Image.open(path).convert('RGB')).reshape(-1, 3)
print(bool((pixels == pixels[0]).all()))
'@

    $results = @()
    foreach ($fixture in $M7COwnerCases) {
        if ($Only.Count -gt 0 -and $Only -notcontains $fixture.Key) { continue }
        foreach ($point in $Points) {
            $captureDir = Join-Path $outDir "$($fixture.Key)/$point"
            New-Item -ItemType Directory -Force $captureDir | Out-Null
            $log = Join-Path $outDir "$($fixture.Key)__$point.log"
            $arguments = @(
                '--benchmark', $fixture.Id, '--benchmark-manifest', (Join-Path $root $fixture.Manifest)
            ) + @(Get-M7RModelArtifactArgs $artifactRoot $fixture) + @(
                '--window-size', '3840x2160', '--hidden-window', '--borderless-window',
                '--output-transport', 'sdr',
                '--warmup-frames', '12', '--frame-limit', '6',
                '--capture-frame', '4', '--capture-directory', $captureDir,
                '--capture-point', $point,
                $(if ($Validation) { '--validation' } else { '--no-validation' })
            ) + $baseArgs + $fixture.Args + $ExtraArgs
            if (-not $fixture.ConstantImage) { $arguments += '--require-capture-signal' }
            $started = Get-Date
            $exit = Invoke-M7REngine $exePath $arguments $log
            $seconds = ((Get-Date) - $started).TotalSeconds
            $validationLines = @(Select-String -Path $log -Pattern '^\[Validation\]:' -ErrorAction SilentlyContinue).Count
            $sidecar = Get-ChildItem $captureDir -Filter '*.json' | Select-Object -First 1
            $sha = $null; $image = $null; $constant = $null
            if ($sidecar) {
                $meta = Get-Content $sidecar.FullName -Raw | ConvertFrom-Json
                $sha = $meta.image.sha256
                $image = Get-ChildItem $captureDir -File | Where-Object { $_.Extension -in '.tga', '.pfm' } | Select-Object -First 1
            }
            if ($fixture.ConstantImage -and $image) {
                $constant = [string](python $constantCheck $image.FullName) -eq 'True'
                if (-not $constant) { $sha = $null }
            }
            $results += [pscustomobject]@{
                key = $fixture.Key; fixture = $fixture.Id; point = $point; sha256 = $sha
                exit = $exit; validationMessages = $validationLines; seconds = [math]::Round($seconds, 1)
                image = $(if ($image) { $image.FullName.Substring($root.Length + 1) } else { $null })
                constantImageExpected = [bool]$fixture.ConstantImage; constantImage = $constant
            }
            $status = if ($exit -eq 0 -and $sha) { 'ok' } else { 'FAILED' }
            Write-Host ("{0,-26} {1,-9} {2} {3} validation={4} {5:n1}s" -f $fixture.Key, $point, $status, $(if ($sha) { $sha.Substring(0, 16) } else { '-' }), $validationLines, $seconds)
        }
    }

    if ($Previews) {
        $previewDir = Join-Path $outDir 'previews'
        New-Item -ItemType Directory -Force $previewDir | Out-Null
        $pairs = @($results | Where-Object { $_.point -eq 'final-sdr' -and $_.image -and $_.image.EndsWith('.tga') } |
            ForEach-Object { "$(Join-Path $root $_.image)|$(Join-Path $previewDir "$($_.key).png")" })
        if ($pairs.Count -gt 0) {
            $size = $PreviewSize -split 'x'
            $script = @'
import sys
from PIL import Image
width, height = int(sys.argv[1]), int(sys.argv[2])
for pair in sys.argv[3:]:
    source, target = pair.split('|')
    Image.open(source).convert('RGB').resize((width, height), Image.LANCZOS).save(target)
'@
            $scriptPath = Join-Path $previewDir 'make-previews.py'
            Set-Content -Encoding utf8 $scriptPath $script
            python $scriptPath $size[0] $size[1] @pairs
            if ($LASTEXITCODE -ne 0) { Write-Warning 'Preview conversion failed (Python with Pillow is required).' }
            Remove-Item $scriptPath
        }
    }

    Remove-Item $constantCheck
    $git = (git rev-parse HEAD).Trim()
    $dirty = [bool](git status --porcelain --untracked-files=no | Where-Object { $_ -notmatch 'imgui\.ini$' })
    $exeSha = (Get-FileHash $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $report = [pscustomobject]@{
        label = $Label; commit = $git; dirtyTrackedFiles = $dirty; executable = $Exe; executableSha256 = $exeSha
        validation = [bool]$Validation; extraArgs = $ExtraArgs
        artifactRoot = $artifactRoot; localAssetRoot = (Get-IridiumLocalAssetRoot $root)
        engineBaseArgs = $baseArgs; captured = (Get-Date).ToString('o'); results = $results
    }
    $report | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $outDir 'hashes.json')
    $md = @("| Case | Fixture | Point | SHA-256 (16) | Exit | Validation msgs | Signal |", "|---|---|---|---|---:|---:|---|")
    foreach ($r in $results) {
        $signal = if ($r.constantImageExpected) { "constant by design ($(if ($r.constantImage) { 'verified' } else { 'NOT constant' }))" } else { 'required' }
        $md += "| $($r.key) | ``$($r.fixture)`` | $($r.point) | ``$(if ($r.sha256) { $r.sha256.Substring(0, 16) } else { '-' })`` | $($r.exit) | $($r.validationMessages) | $signal |"
    }
    $md | Set-Content -Encoding utf8 (Join-Path $outDir 'summary.md')
    $failed = @($results | Where-Object { $_.exit -ne 0 -or -not $_.sha256 }).Count
    Write-Host "Wrote $outDir (commit $git, failures $failed)"
    if ($failed -gt 0) { exit 1 }
}
finally { Pop-Location }
