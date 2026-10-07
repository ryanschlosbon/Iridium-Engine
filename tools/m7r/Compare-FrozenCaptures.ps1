# Compares two Run-FrozenCaptures results. 'exact' fixtures must be byte-identical
# (image SHA-256); fixtures with a tolerance class (M7RFixtures.ps1) that differ are
# checked with Diff-Images.py against that class's envelope. Exits non-zero on any
# failure or missing entry.
#
#   powershell -File tools/m7r/Compare-FrozenCaptures.ps1 -Baseline r0 -Candidate r1 [-PruneIdentical]
param(
    [Parameter(Mandatory)] [string] $Baseline,
    [Parameter(Mandatory)] [string] $Candidate,
    # Delete candidate images that are byte-identical to the baseline (the hash is
    # recorded in hashes.json); keeps disk use bounded across many 4K runs.
    [switch] $PruneIdentical
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'M7RFixtures.ps1')
$root = Get-M7RRepoRoot
$tolerance = @{}; foreach ($f in $M7RFrozenSet) { $tolerance[$f.Key] = if ($f.Tolerance) { $f.Tolerance } else { 'exact' } }

# A label's capture directory: this checkout's, else the main checkout's (a worktree
# compares against baselines captured there).
function Get-CaptureDir([string] $label) {
    $own = Join-Path $root "out/m7r/captures/$label"
    if (Test-Path (Join-Path $own 'hashes.json')) { return $own }
    $main = Get-IridiumMainCheckout $root
    if ($main) {
        $shared = Join-Path $main "out/m7r/captures/$label"
        if (Test-Path (Join-Path $shared 'hashes.json')) { return $shared }
    }
    return $own
}
function Read-Results([string] $label) {
    $path = Join-Path (Get-CaptureDir $label) 'hashes.json'
    $map = @{}
    foreach ($r in (Get-Content $path -Raw | ConvertFrom-Json).results) { $map["$($r.key)|$($r.point)"] = $r }
    return $map
}
function Image-Path([string] $label, $r) {
    $extension = if ($r.point -eq 'scene') { 'pfm' } else { 'tga' }
    Join-Path (Get-CaptureDir $label) "$($r.key)/$($r.point)/$($r.stem).$extension"
}

$base = Read-Results $Baseline
$cand = Read-Results $Candidate
$failures = 0
foreach ($k in ($base.Keys + $cand.Keys | Sort-Object -Unique)) {
    if (-not $base.ContainsKey($k)) { Write-Host "NEW       $k"; continue }
    if (-not $cand.ContainsKey($k)) { Write-Host "MISSING   $k"; $failures++; continue }
    $b = $base[$k]; $c = $cand[$k]
    if ($b.sha256 -and $b.sha256 -eq $c.sha256) {
        Write-Host "identical $k"
        if ($PruneIdentical) { Remove-Item -ErrorAction SilentlyContinue (Image-Path $Candidate $c) }
        continue
    }
    $class = $tolerance[$b.key]
    if (-not $class -or $class -eq 'exact' -or -not $c.sha256) {
        Write-Host "MISMATCH  $k  $($b.sha256) -> $($c.sha256)"; $failures++; continue
    }
    $limits = $M7RTolerances[$class][$b.point]
    $report = python (Join-Path $PSScriptRoot 'Diff-Images.py') (Image-Path $Baseline $b) (Image-Path $Candidate $c) @limits
    if ($LASTEXITCODE -eq 0) { Write-Host "within    $k  [$class] $(($report | Select-Object -First 1))" }
    else { Write-Host "OUTSIDE   $k  [$class]"; $report | ForEach-Object { Write-Host "          $_" }; $failures++ }
}
Write-Host "failures: $failures"
if ($failures -gt 0) { exit 1 }
