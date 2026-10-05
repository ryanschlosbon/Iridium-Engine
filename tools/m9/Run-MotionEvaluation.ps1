# M9 temporal motion evaluation: captures for Evaluate-TemporalMotion.py. Harness tooling only.
#
# The held-scene evaluation (Evaluate-Temporal.py) scores a converged frame of a frozen scene.
# This one scores frames of the fixtures' own motion (camera pans and cuts, moving occluders,
# teleports, moving glass) against a per-frame accumulation reference:
#   - references: one Run-TemporalCaptures -Reference process per frame, holding the benchmark
#     at that frame (64 Halton phases), under out/m9/motion/ref<Samples>/<fixture>-h<frame>
#     (reused when present, so several candidates share them);
#   - candidate: one Run-TemporalCaptures sequence process per frame range, under
#     out/m9/motion/<Label>/<fixture>-<first>-<last>.
# Existing references and candidate ranges are kept and skipped, so an interrupted run resumes.
# Held benchmark frame H shows the same scene as measured frame H - warmup (verified byte for
# byte on TF-disocclude). The frame plan below pairs each scored frame with its predecessor, so
# the scorer's trail mask (pixels whose reference changed since the previous frame) is defined.
#
#   powershell -ExecutionPolicy Bypass -File tools/m9/Run-MotionEvaluation.ps1 -Label taa-def `
#       -ExtraArgs '--anti-aliasing taa' -Point scene-resolved
#   powershell -ExecutionPolicy Bypass -File tools/m9/Run-MotionEvaluation.ps1 -Label noaa -Point scene
#   python tools/m9/Evaluate-TemporalMotion.py --candidate taa-def --baseline noaa
param(
    [Parameter(Mandatory)] [string] $Label,
    [string[]] $ExtraArgs = @(),
    [string] $Point = 'scene-resolved',
    [string[]] $Only = @(),
    [int] $Samples = 64,
    [switch] $ReferencesOnly
)
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$ExtraArgs = @($ExtraArgs | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$capture = Join-Path $PSScriptRoot 'Run-TemporalCaptures.ps1'
$warmup = 120   # every M9 temporal fixture's manifest warmup

# Measured-frame ranges per fixture (benchmark frame = 120 + measured).
$plan = [ordered]@{
    # Static segment, the cut at benchmark 330 into a moving pan, recovery, steady pan.
    'TF-pan'        = @('209:218', '241:242', '273:274')
    # Occluder sweeping at 7 m per 120 frames: trailing-edge disocclusion.
    'TF-disocclude' = @('79:80', '109:110')
    # Cube teleport at benchmark 301 (keyframe 61 of the 240-frame period), recovery.
    'TF-teleport'   = @('180:188', '195:196')
    'TF-glass'      = @('79:80')
    'TF-thin'       = @('79:80')
    'TF-foliage'    = @('79:80')
    'TF-emissive'   = @('79:80')
    'TF-specular'   = @('79:80')
    'TF-hdr'        = @('79:80')
    # M9.3: tinted glass and two particle cards (sorted, WeightedOIT) over a still backdrop.
    'TF-reactive'   = @('79:82', '139:140')
}

function Invoke-Capture([string[]] $arguments) {
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $capture @arguments 2>&1
    $last = @($output | ForEach-Object { "$_" })[-1]
    if ($LASTEXITCODE -ne 0 -or $last -notmatch 'failures 0') {
        $output | ForEach-Object { Write-Host "  $_" }
        throw "Run-TemporalCaptures failed: $($arguments -join ' ')"
    }
    Write-Host $last
}

$refRoot = Join-Path $root "out/m9/motion/ref$Samples"
# A git worktree also reuses the main checkout's references (new ones are written here).
$common = @(git -C $root rev-parse --path-format=absolute --git-common-dir 2>$null)[0]
$mainRefRoot = if ($common -and (Test-Path $common)) {
    Join-Path (Resolve-Path (Join-Path $common '..')).Path "out/m9/motion/ref$Samples" } else { $refRoot }
$candidateRoot = Join-Path $root "out/m9/motion/$Label"
foreach ($key in $plan.Keys) {
    if ($Only.Count -gt 0 -and $Only -notcontains $key) { continue }
    foreach ($range in $plan[$key]) {
        $parts = $range -split ':'
        $first = [int]$parts[0]; $last = [int]$parts[1]
        for ($m = $first; $m -le $last; ++$m) {
            $hold = $warmup + $m
            $refLabel = "$key-h$hold"
            if ((Test-Path (Join-Path $refRoot $refLabel)) -or (Test-Path (Join-Path $mainRefRoot $refLabel))) { continue }
            Invoke-Capture @('-Label', $refLabel, '-OutRoot', $refRoot, '-Only', $key, '-Reference',
                '-HoldFrame', "$hold", '-Samples', "$Samples")
        }
        if ($ReferencesOnly) { continue }
        # Resumable: a captured range is never overwritten, so a rerun continues the plan.
        if (Test-Path (Join-Path $candidateRoot "$key-$first-$last")) { continue }
        Invoke-Capture (@('-Label', "$key-$first-$last", '-OutRoot', $candidateRoot, '-Only', $key,
            '-Frames', $range, '-Points', $Point) +
            $(if ($ExtraArgs.Count -gt 0) { @('-ExtraArgs', ($ExtraArgs -join ' ')) } else { @() }))
    }
}
if (-not $ReferencesOnly) {
    @{ label = $Label; point = $Point; extraArgs = $ExtraArgs; samples = $Samples; plan = $plan } |
        ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $candidateRoot 'motion.json')
}
Write-Host "Motion evaluation captures done: $Label"
