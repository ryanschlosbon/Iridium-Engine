# M9 G6c temporal capture runner (docs/milestones/M9-temporal-and-post.md). Harness tooling only.
#
# Sequence mode (default): for each fixture of $M9TemporalSet (tools/m9/M9Fixtures.ps1), and with
# -IncludeFrozen also the M7R frozen set, runs one native-4K hidden process per capture point with
# --capture-frames <Frames> and writes one artifact per measured frame to
#   out/m9/captures/<Label>/<fixture>/<point>/
# Measured frame m is benchmark (application) frame warmup + m; the warmup is the fixture's own
# (manifest warmup_frames) unless -Warmup is given. The frame limit is LAST + 1.
#
# Reference mode (-Reference): an accumulation reference per fixture. One process renders with
#   --benchmark-hold-frame <HoldFrame> --temporal-jitter on --temporal-jitter-sequence <Samples>
# and captures <Samples> consecutive scene-linear frames from the first measured frame at or after
# HoldFrame + Settle, which covers every Halton phase once over a frozen scene. The frames are then
# averaged into one PFM by the G6d metrics tool:
#   IridiumTemporalMetrics.exe accumulate --frames F1.pfm ... --out <stem>__ref<Samples>.pfm
# into out/m9/captures/<Label>/<fixture>/reference/. The per-frame PFMs are deleted afterwards; their
# sidecars and frames.json (SHA-256 and jitter per frame) are kept. If the metrics tool is not built
# (it is the G6d lane's target), the frames are kept and the accumulation is skipped with a message.
#
# A label is never overwritten. Each run records its arguments, exit code, wall time, peak process
# memory and per-frame capture metadata in runs.json; machine.json records commit, executable hash
# and GPU state.
#
#   powershell -ExecutionPolicy Bypass -File tools/m9/Run-TemporalCaptures.ps1 -Label pan-jitter `
#       -Only TF-pan -Frames 57:62 -Points scene -ExtraArgs '--temporal-jitter on'
#   powershell -ExecutionPolicy Bypass -File tools/m9/Run-TemporalCaptures.ps1 -Label ref64 `
#       -Reference -Only TF-static -HoldFrame 130 -Samples 64
param(
    [Parameter(Mandatory)] [string] $Label,
    [string] $Exe = 'out/build/x64-release/bin/IridiumEngine.exe',
    [string[]] $Only = @(),
    # FIRST:LAST[:STEP] measured frames (sequence mode).
    [string] $Frames = '0:7',
    [string[]] $Points = @('scene', 'final-sdr'),
    [string[]] $ExtraArgs = @(),
    # Warmup frames; -1 keeps each fixture's manifest warmup.
    [int] $Warmup = -1,
    [switch] $IncludeFrozen,
    [switch] $Validation,
    [switch] $Reference,
    # Reference mode: benchmark frame to hold, jitter phases (= frames averaged), and frames to
    # skip after the hold before the first sample.
    [int] $HoldFrame = -1,
    [int] $Samples = 64,
    [int] $Settle = 0,
    [string] $MetricsExe = 'out/build/x64-release/bin/IridiumTemporalMetrics.exe',
    # Where the frozen-set manifests and cooked artifacts (out/m7r/ddc) come from. Default: the main
    # checkout (parent of the git common directory), so a worktree uses the main checkout's data
    # (third-party content comes from the local asset library).
    [string] $RepoDataRoot = '',
    # Where the M9 cooked artifacts (out/m9/ddc, from Cook-TemporalModels.ps1) come from. Default:
    # this checkout when it has them, else the main checkout.
    [string] $M9DataRoot = '',
    [string] $OutRoot = '',
    [string] $PipelineCache = 'off'
)
$ErrorActionPreference = 'Stop'
# powershell -File passes comma lists as one string; accept both forms.
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$ExtraArgs = @($ExtraArgs | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
$Points = @($Points | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'M9Fixtures.ps1')
. (Join-Path $PSScriptRoot '..\m7r\M7RFixtures.ps1')
$root = Get-M9RepoRoot

function Get-MainCheckout([string] $checkout) {
    $common = @(git -C $checkout rev-parse --path-format=absolute --git-common-dir 2>$null)[0]
    if ($common -and (Test-Path $common)) { return (Resolve-Path (Join-Path $common '..')).Path }
    return $checkout
}

function ConvertTo-FrameRange([string] $text) {
    if ($text -notmatch '^(\d+):(\d+)(?::(\d+))?$') { throw "-Frames must be FIRST:LAST[:STEP], got '$text'" }
    $first = [int64]$Matches[1]; $last = [int64]$Matches[2]
    $step = if ($Matches[3]) { [int64]$Matches[3] } else { 1 }
    if ($first -gt $last -or $step -lt 1) { throw "-Frames needs FIRST <= LAST and STEP >= 1, got '$text'" }
    $frames = @(); for ($f = $first; $f -le $last; $f += $step) { $frames += $f }
    return [pscustomobject]@{ first = $first; last = $last; step = $step; frames = $frames; text = $text }
}

function Get-GpuSummary {
    $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $smi) { return $null }
    $line = @(& $smi.Source '--query-gpu=name,driver_version,pstate,clocks.gr,clocks.mem,power.draw,temperature.gpu,memory.used' '--format=csv,noheader,nounits' 2>$null)[0]
    if (-not $line) { return $null }
    $v = $line -split ',\s*'
    return [ordered]@{ name = $v[0]; driver = $v[1]; pstate = $v[2]; clock_gr_mhz = $v[3]; clock_mem_mhz = $v[4]
        power_w = $v[5]; temperature_c = $v[6]; memory_used_mib = $v[7] }
}

# Runs the engine with stdout in $log and stderr in "$log.err", sampling the process's peak working
# set and peak private bytes until it exits. Returns exit code, wall seconds and the peaks.
function Invoke-MeasuredEngine([string] $exe, [string[]] $arguments, [string] $log, [string] $workingDirectory) {
    $quoted = $arguments | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
    $started = Get-Date
    $process = Start-Process -FilePath $exe -ArgumentList ($quoted -join ' ') -WorkingDirectory $workingDirectory `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" -NoNewWindow -PassThru
    $handle = $process.Handle   # keeps the exit code readable after exit
    $peakWorkingSet = 0L; $peakPrivate = 0L
    while (-not $process.HasExited) {
        try {
            $process.Refresh()
            $peakWorkingSet = [math]::Max($peakWorkingSet, $process.PeakWorkingSet64)
            $peakPrivate = [math]::Max($peakPrivate, $process.PeakPagedMemorySize64)
        } catch {}
        Start-Sleep -Milliseconds 100
    }
    $process.WaitForExit()
    return [pscustomobject]@{
        exit = $process.ExitCode; seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
        peakWorkingSetBytes = $peakWorkingSet; peakPrivateBytes = $peakPrivate
    }
}

function Get-Fixtures {
    $all = @($M9TemporalSet | ForEach-Object { $_ + @{ Set = 'm9' } })
    if ($IncludeFrozen) { $all += @($M7RFrozenSet | ForEach-Object { $_ + @{ Set = 'm7r' } }) }
    if ($Only.Count -gt 0) {
        foreach ($key in $Only) { if (-not ($all | Where-Object { $_.Key -eq $key })) { throw "Unknown fixture key: $key" } }
        $all = @($all | Where-Object { $Only -contains $_.Key })
    }
    return $all
}

function Get-FixtureDataRoot($fixture) {
    if ($fixture.Set -eq 'm9') { return $M9DataRoot }
    return $RepoDataRoot
}

function Get-FixtureWarmup($fixture) {
    if ($Warmup -ge 0) { return $Warmup }
    $manifest = Get-Content (Join-Path (Get-FixtureDataRoot $fixture) $fixture.Manifest) -Raw | ConvertFrom-Json
    $entry = $manifest.fixtures | Where-Object { $_.id -eq $fixture.Id } | Select-Object -First 1
    if (-not $entry) { throw "Fixture $($fixture.Id) not in $($fixture.Manifest)" }
    return [int]$entry.warmup_frames
}

function Get-FixtureArguments($fixture) {
    $data = Get-FixtureDataRoot $fixture
    if ($fixture.Set -eq 'm9') {
        $artifact = Join-Path $data (Get-M9ModelArtifact $data $fixture.Model)
    } else {
        $artifact = Join-Path $data (Get-M7RModelArtifact $data $fixture.Model)
    }
    $arguments = @('--benchmark', $fixture.Id, '--benchmark-manifest', (Join-Path $data $fixture.Manifest),
        '--cooked-model-artifact', $artifact)
    if ($fixture.Environment) {
        $arguments += @('--cooked-environment-artifact', (Join-Path $RepoDataRoot (Get-M7RModelArtifact $RepoDataRoot $fixture.Environment)))
    }
    return $arguments + $fixture.Args
}

# Per-frame capture records from the sidecars in $dir, in measured-frame order.
function Get-CaptureRecords([string] $dir) {
    $records = foreach ($sidecar in Get-ChildItem $dir -Filter '*.json' -ErrorAction SilentlyContinue) {
        $meta = Get-Content $sidecar.FullName -Raw | ConvertFrom-Json
        $jitter = $meta.render_configuration.temporal_jitter
        [pscustomobject]@{
            measuredFrame = [int64]$meta.run.measured_frame_index
            applicationFrame = [int64]$meta.run.application_frame_index
            benchmarkStateFrame = [int64]$meta.run.benchmark_state_frame_index
            image = $meta.image.path; sha256 = $meta.image.sha256
            jitter = $jitter
        }
    }
    return @($records | Sort-Object measuredFrame)
}

Push-Location $root
try {
    if (-not $RepoDataRoot) { $RepoDataRoot = Get-MainCheckout $root }
    $RepoDataRoot = (Resolve-Path $RepoDataRoot).Path
    if (-not $M9DataRoot) {
        $M9DataRoot = if (Test-Path (Join-Path $root 'out/m9/ddc/artifacts.json')) { $root } else { $RepoDataRoot }
    }
    $M9DataRoot = (Resolve-Path $M9DataRoot).Path
    $exePath = (Resolve-Path $Exe).Path
    $cacheArgs = @(Get-M7REngineBaseArgs $exePath $PipelineCache)
    if (-not $OutRoot) { $OutRoot = Join-Path $root 'out/m9/captures' }
    $outDir = Join-Path $OutRoot $Label
    if (Test-Path $outDir) { throw "Output directory already exists: $outDir (captures are never overwritten)" }
    if ($Reference) {
        if ($HoldFrame -lt 0) { throw '-Reference requires -HoldFrame (a benchmark frame; warmup frames count).' }
        if ($Samples -lt 1 -or $Samples -gt 4096) { throw '-Samples must be 1..4096 (the jitter sequence length).' }
        $Points = @('scene')
    }
    $range = if ($Reference) { $null } else { ConvertTo-FrameRange $Frames }
    $fixtures = @(Get-Fixtures)
    New-Item -ItemType Directory -Force $outDir | Out-Null

    $git = (git rev-parse HEAD).Trim()
    $dirty = [bool](git status --porcelain --untracked-files=no | Where-Object { $_ -notmatch 'imgui\.ini$' })
    $machine = [ordered]@{
        label = $Label; mode = $(if ($Reference) { 'reference' } else { 'sequence' }); commit = $git
        dirtyTrackedFiles = $dirty; executable = $exePath
        executableSha256 = (Get-FileHash $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
        repoDataRoot = $RepoDataRoot; m9DataRoot = $M9DataRoot; computer = $env:COMPUTERNAME
        cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
        gpuBefore = Get-GpuSummary; started = (Get-Date).ToString('o')
    }
    $runs = @()
    $metricsPath = if ([IO.Path]::IsPathRooted($MetricsExe)) { $MetricsExe } else { Join-Path $root $MetricsExe }
    foreach ($fixture in $fixtures) {
        $warmup = Get-FixtureWarmup $fixture
        if ($Reference) {
            # First measured frame whose benchmark frame is at or after HoldFrame + Settle.
            $firstMeasured = [math]::Max(0, $HoldFrame + $Settle - $warmup)
            $fixtureRange = ConvertTo-FrameRange "$($firstMeasured):$($firstMeasured + $Samples - 1)"
            $engineExtra = @('--benchmark-hold-frame', "$HoldFrame", '--temporal-jitter', 'on',
                '--temporal-jitter-sequence', "$Samples")
        } else {
            $fixtureRange = $range
            $engineExtra = @()
        }
        foreach ($point in $Points) {
            $pointName = if ($Reference) { 'reference' } else { $point }
            $fixtureDir = Join-Path $outDir "$($fixture.Key)/$pointName"
            $captureDir = if ($Reference) { Join-Path $fixtureDir 'frames' } else { $fixtureDir }
            New-Item -ItemType Directory -Force $captureDir | Out-Null
            $log = Join-Path $outDir "$($fixture.Key)__$pointName.log"
            $arguments = @(Get-FixtureArguments $fixture) + @(
                '--window-size', '3840x2160', '--hidden-window', '--borderless-window',
                '--output-transport', 'sdr',
                '--warmup-frames', "$warmup", '--frame-limit', "$($fixtureRange.last + 1)",
                '--capture-frames', $fixtureRange.text, '--capture-directory', $captureDir,
                '--capture-point', $point, '--require-capture-signal',
                $(if ($Validation) { '--validation' } else { '--no-validation' })
            ) + $cacheArgs + $engineExtra + $ExtraArgs
            $result = Invoke-MeasuredEngine $exePath $arguments $log $root
            $validationLines = @(Select-String -Path $log -Pattern '^\[Validation\]:' -ErrorAction SilentlyContinue).Count
            $records = @(Get-CaptureRecords $captureDir)
            $expected = @($fixtureRange.frames)
            $complete = $result.exit -eq 0 -and $records.Count -eq $expected.Count -and
                -not (Compare-Object @($records | ForEach-Object { $_.measuredFrame }) $expected -SyncWindow 0)
            $run = [ordered]@{
                key = $fixture.Key; fixture = $fixture.Id; point = $point; warmup = $warmup
                frames = $fixtureRange.text; arguments = $arguments; exit = $result.exit; seconds = $result.seconds
                peakWorkingSetBytes = $result.peakWorkingSetBytes; peakPrivateBytes = $result.peakPrivateBytes
                validationMessages = $validationLines; complete = $complete; captures = $records
            }
            if ($Reference -and $complete) {
                $images = @($records | ForEach-Object { $_.image })
                $stem = [IO.Path]::GetFileNameWithoutExtension($images[0]) -replace '__mf\d+$', ''
                $referencePath = Join-Path $fixtureDir "$($stem)__hold$($HoldFrame)__ref$($Samples).pfm"
                $records | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $fixtureDir 'frames.json')
                if (Test-Path $metricsPath) {
                    # Relative frame names from the frames directory keep the command line short.
                    # Windows PowerShell turns native stderr into errors; the tool's summary
                    # line goes to stderr, so judge it by its exit code only.
                    Push-Location $captureDir
                    $savedPreference = $ErrorActionPreference
                    $ErrorActionPreference = 'Continue'
                    try {
                        $reportPath = Join-Path $fixtureDir 'accumulate.json'
                        & $metricsPath accumulate --frames @images --out $referencePath --report $reportPath 2>&1 |
                            ForEach-Object { "$_" } | Set-Content -Encoding utf8 (Join-Path $outDir "$($fixture.Key)__accumulate.log")
                        $accumulateExit = $LASTEXITCODE
                    } finally {
                        $ErrorActionPreference = $savedPreference
                        Pop-Location
                    }
                    $run.accumulateExit = $accumulateExit
                    if ($accumulateExit -eq 0 -and (Test-Path $referencePath)) {
                        $run.reference = [ordered]@{ path = $referencePath
                            sha256 = (Get-FileHash $referencePath -Algorithm SHA256).Hash.ToLowerInvariant(); samples = $Samples }
                        foreach ($image in $images) { Remove-Item -LiteralPath (Join-Path $captureDir $image) }
                    } else {
                        $run.complete = $false
                        Write-Warning "$($fixture.Key): accumulate failed (exit $accumulateExit); per-frame PFMs kept in $captureDir"
                    }
                } else {
                    $run.reference = $null
                    Write-Warning ("$($fixture.Key): $MetricsExe not found (the G6d temporal metrics tool). " +
                        "Per-frame PFMs kept in $captureDir; accumulate them later with " +
                        "'IridiumTemporalMetrics accumulate --frames <frames>/*.pfm --out $referencePath'.")
                }
            }
            $runs += [pscustomobject]$run
            $status = if ($run.complete) { 'ok' } else { 'FAILED' }
            Write-Host ("{0,-12} {1,-10} {2} frames={3} captures={4} exit={5} peakWS={6:n0}MiB validation={7}" -f `
                $fixture.Key, $pointName, $status, $fixtureRange.text, $records.Count, $result.exit,
                ($result.peakWorkingSetBytes / 1MB), $validationLines)
            $machine.finished = (Get-Date).ToString('o')
            $machine | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $outDir 'machine.json')
            $runs | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $outDir 'runs.json')
        }
    }
    $machine.gpuAfter = Get-GpuSummary
    $machine | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $outDir 'machine.json')
    $failed = @($runs | Where-Object { -not $_.complete }).Count
    Write-Host "Wrote $outDir (commit $git, failures $failed)"
    if ($failed -gt 0) { exit 1 }
}
finally { Pop-Location }
