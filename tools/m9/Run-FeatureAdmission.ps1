# M9 G6a feature-admission timing runner (docs/milestones/M9-temporal-and-post.md,
# docs/performance/FRAME_BUDGET.md "Evidence tiers"). For each timing route, runs five
# fresh native-4K Release processes per side, interleaved A,B,B,A,A,B,B,A,A,B, records
# machine state before (and GPU state after) every process, then summarizes with
# Summarize-Admission.py. Run-TimingPair.ps1 (the refactor tier) is unchanged.
#
# Sides: each side is an engine executable plus the checkout it runs in (the engine
# resolves assets/shaders against its working directory). -RootA/-RootB name checkouts
# (exe = <root>/out/build/x64-release/bin/IridiumEngine.exe); -ExeA/-ExeB name
# executables directly (working directory = the checkout above out/build/<preset>/bin,
# or -RootX when given). Both default to this checkout. -ArgsA/-ArgsB add engine flags
# to one side only (a feature toggle); -ExtraArgs adds flags to both.
#
# Manifests and cooked artifacts (out/m7r/ddc, from tools/m7r/Cook-FrozenModels.ps1)
# come from -RepoDataRoot, which defaults to the main checkout of this repository (the
# parent of the git common directory), so a git worktree uses the main checkout's
# cooked data and local-only assets. Results go to <OutRoot>/<Label> (default:
# out/m9/timing of this checkout); an existing label is never overwritten.
#
#   powershell -ExecutionPolicy Bypass -File tools/m9/Run-FeatureAdmission.ps1 -Label taa-on `
#       -ArgsB '--anti-aliasing taa' [-Routes T-F1-all,T-F5-hetero] [-RequireQuiet]
#   ... -Label smoke -Routes T-F1-all -Warmup 5 -Frames 50
param(
    [Parameter(Mandatory)] [string] $Label,
    [string[]] $Routes = @(),
    [string] $RootA = '',
    [string] $RootB = '',
    [string] $ExeA = '',
    [string] $ExeB = '',
    [string[]] $ArgsA = @(),
    [string[]] $ArgsB = @(),
    [string[]] $ExtraArgs = @(),
    [int] $Warmup = 500,
    [int] $Frames = 10000,
    [string] $RepoDataRoot = '',
    [string] $OutRoot = '',
    # 'off' (default) or a cache directory; see Get-M7RPipelineCacheArgs.
    [string] $PipelineCache = 'off',
    # Abort (instead of warn) when the pre-run quiet check fails.
    [switch] $RequireQuiet,
    [double] $QuietThresholdPercent = 5.0,
    [int] $QuietSampleSeconds = 3,
    # CPU sample window for the per-process top-CPU record (0 records cumulative CPU only).
    [int] $StateSampleSeconds = 1,
    [switch] $SkipSummary
)
$ErrorActionPreference = 'Stop'
# powershell -File passes comma lists as one string; accept both forms.
$Routes = @($Routes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$ArgsA = @($ArgsA | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
$ArgsB = @($ArgsB | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
$ExtraArgs = @($ExtraArgs | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot '..\m7r\M7RFixtures.ps1')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Order = @('A', 'B', 'B', 'A', 'A', 'B', 'B', 'A', 'A', 'B')
$EngineProcessName = 'IridiumEngine'

function Get-MainCheckout([string] $checkout) {
    $common = @(git -C $checkout rev-parse --path-format=absolute --git-common-dir 2>$null)[0]
    if ($common -and (Test-Path $common)) { return (Resolve-Path (Join-Path $common '..')).Path }
    return $checkout
}

function Resolve-Side([string] $name, [string] $sideRoot, [string] $exe) {
    if ($sideRoot) { $sideRoot = (Resolve-Path $sideRoot).Path }
    if (-not $exe) {
        if (-not $sideRoot) { $sideRoot = $root }
        $exe = Join-Path $sideRoot 'out/build/x64-release/bin/IridiumEngine.exe'
    }
    if (-not (Test-Path $exe)) { throw "Side $name executable not found: $exe" }
    $exe = (Resolve-Path $exe).Path
    if (-not $sideRoot) {
        # <checkout>/out/build/<preset>/bin/IridiumEngine.exe -> <checkout>
        $bin = Split-Path $exe -Parent
        $candidate = Split-Path (Split-Path (Split-Path (Split-Path $bin -Parent) -Parent) -Parent) -Parent
        if ($candidate -and (Test-Path (Join-Path $candidate 'assets/shaders'))) { $sideRoot = $candidate }
        else { throw "Cannot derive the checkout for $exe; pass -Root$name." }
    }
    return [pscustomobject]@{ name = $name; root = $sideRoot; exe = $exe }
}

# --- machine state ---------------------------------------------------------------
$NvidiaSmi = (Get-Command nvidia-smi -ErrorAction SilentlyContinue | Select-Object -First 1)
$GpuFields = @('name', 'driver_version', 'pstate', 'clocks.gr', 'clocks.sm', 'clocks.mem', 'clocks.max.gr',
    'power.draw', 'power.limit', 'temperature.gpu', 'utilization.gpu', 'utilization.memory', 'memory.used',
    'fan.speed', 'clocks_event_reasons.active')
function Get-GpuState {
    if (-not $NvidiaSmi) { return $null }
    $fields = $GpuFields
    # Capture all output: Select-Object -First would stop the pipeline and fail the exit code.
    $line = @(& $NvidiaSmi.Source "--query-gpu=$($fields -join ',')" '--format=csv,noheader,nounits' 2>$null)[0]
    if ($LASTEXITCODE -ne 0 -or -not $line) {
        # Older drivers lack clocks_event_reasons; retry without it.
        $fields = $GpuFields | Where-Object { $_ -ne 'clocks_event_reasons.active' }
        $line = @(& $NvidiaSmi.Source "--query-gpu=$($fields -join ',')" '--format=csv,noheader,nounits' 2>$null)[0]
        if ($LASTEXITCODE -ne 0 -or -not $line) { return $null }
    }
    $values = $line -split ',\s*'
    $state = [ordered]@{}
    for ($i = 0; $i -lt $fields.Count; $i++) {
        $raw = if ($i -lt $values.Count) { $values[$i].Trim() } else { '' }
        $number = 0.0
        if ($fields[$i] -ne 'driver_version' -and $raw -match '^-?\d+(\.\d+)?$' -and [double]::TryParse($raw, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref] $number)) {
            $state[$fields[$i]] = $number
        } else { $state[$fields[$i]] = $raw }
    }
    return $state
}

$LogicalProcessors = [int](Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
# Top CPU consumers. With $seconds > 0, percent is the share of the whole machine (all
# logical processors) used over the window; cumulative CPU seconds are kept as well.
function Get-TopProcesses([int] $seconds, [int] $count = 10) {
    $first = @{}
    foreach ($p in Get-Process) { try { $first[$p.Id] = $p.TotalProcessorTime.TotalSeconds } catch {} }
    if ($seconds -gt 0) { Start-Sleep -Seconds $seconds }
    $rows = foreach ($p in Get-Process) {
        if ($p.Id -eq 0) { continue }   # Idle
        try { $cpu = $p.TotalProcessorTime.TotalSeconds } catch { continue }
        $percent = $null
        if ($seconds -gt 0 -and $first.ContainsKey($p.Id)) {
            $percent = [math]::Round(100.0 * ($cpu - $first[$p.Id]) / ($seconds * $LogicalProcessors), 2)
        }
        [pscustomobject]@{ name = $p.ProcessName; id = $p.Id; cpu_percent = $percent; cpu_seconds_total = [math]::Round($cpu, 2) }
    }
    if ($seconds -gt 0) { return @($rows | Sort-Object cpu_percent -Descending | Select-Object -First $count) }
    return @($rows | Sort-Object cpu_seconds_total -Descending | Select-Object -First $count)
}

function Get-PowerPlan {
    $text = @(powercfg /getactivescheme 2>$null)[0]
    if ($text) { return "$text".Trim() }
    return $null
}

function Get-MachineState([int] $sampleSeconds) {
    # CPU sample first: the GPU reading is then taken immediately before launch and
    # after a short settle, rather than over the previous process's teardown.
    $top = Get-TopProcesses $sampleSeconds
    $gpu = Get-GpuState
    return [ordered]@{
        timestamp = (Get-Date).ToString('o')
        gpu = $gpu
        top_processes = $top
        top_processes_sample_seconds = $sampleSeconds
        power_plan = Get-PowerPlan
    }
}

# Quiet check: GPU utilization (mean of samples across the window) and per-process CPU
# share over the window, excluding engine processes and this script's own process.
function Test-Quiet {
    $gpuSamples = @()
    $first = @{}
    foreach ($p in Get-Process) { try { $first[$p.Id] = $p.TotalProcessorTime.TotalSeconds } catch {} }
    for ($i = 0; $i -lt $QuietSampleSeconds; $i++) {
        $g = Get-GpuState
        if ($g -and $g['utilization.gpu'] -is [double]) { $gpuSamples += $g['utilization.gpu'] }
        Start-Sleep -Seconds 1
    }
    $busy = @()
    foreach ($p in Get-Process) {
        if ($p.Id -eq 0 -or $p.Id -eq $PID -or $p.ProcessName -eq $EngineProcessName) { continue }
        if (-not $first.ContainsKey($p.Id)) { continue }
        try { $cpu = $p.TotalProcessorTime.TotalSeconds } catch { continue }
        $percent = 100.0 * ($cpu - $first[$p.Id]) / ($QuietSampleSeconds * $LogicalProcessors)
        if ($percent -gt $QuietThresholdPercent) {
            $busy += [pscustomobject]@{ name = $p.ProcessName; id = $p.Id; cpu_percent = [math]::Round($percent, 2) }
        }
    }
    $gpuMean = $null
    if ($gpuSamples.Count -gt 0) { $gpuMean = ($gpuSamples | Measure-Object -Average).Average }
    $problems = @()
    if ($null -ne $gpuMean -and $gpuMean -gt $QuietThresholdPercent) { $problems += ("GPU utilization {0:n1}% > {1}%" -f $gpuMean, $QuietThresholdPercent) }
    foreach ($b in $busy) { $problems += ("{0} (pid {1}) uses {2}% CPU > {3}%" -f $b.name, $b.id, $b.cpu_percent, $QuietThresholdPercent) }
    if (-not $NvidiaSmi) { $problems += 'nvidia-smi not found: GPU utilization not checked' }
    return [ordered]@{
        timestamp = (Get-Date).ToString('o')
        sample_seconds = $QuietSampleSeconds
        threshold_percent = $QuietThresholdPercent
        logical_processors = $LogicalProcessors
        gpu_utilization_samples = $gpuSamples
        gpu_utilization_mean = $gpuMean
        busy_processes = $busy
        quiet = ($problems.Count -eq 0)
        problems = $problems
    }
}

# --- setup -----------------------------------------------------------------------
$sides = @{ A = (Resolve-Side 'A' $RootA $ExeA); B = (Resolve-Side 'B' $RootB $ExeB) }
$sideArgs = @{ A = $ArgsA; B = $ArgsB }
if (-not $RepoDataRoot) { $RepoDataRoot = Get-MainCheckout $root }
$RepoDataRoot = (Resolve-Path $RepoDataRoot).Path
if (-not (Test-Path (Join-Path $RepoDataRoot 'out/m7r/ddc/artifacts.json'))) {
    throw "No cooked artifacts under $RepoDataRoot (out/m7r/ddc/artifacts.json); run tools/m7r/Cook-FrozenModels.ps1 there or pass -RepoDataRoot."
}
if (-not $OutRoot) { $OutRoot = Join-Path $root 'out/m9/timing' }
$outDir = Join-Path $OutRoot $Label
if (Test-Path $outDir) { throw "Output directory already exists: $outDir" }

$selected = @($M7RTimingRoutes | Where-Object { $Routes.Count -eq 0 -or $Routes -contains $_.Key })
$unknown = @($Routes | Where-Object { $r = $_; -not ($M7RTimingRoutes | Where-Object { $_.Key -eq $r }) })
if ($unknown.Count -gt 0) { throw "Unknown route(s): $($unknown -join ', '). Known: $(($M7RTimingRoutes | ForEach-Object { $_.Key }) -join ', ')" }
if ($selected.Count -eq 0) { throw 'No routes selected.' }

Write-Host "feature admission '$Label': $($selected.Count) route(s) x $($Order.Count) processes, warm-up $Warmup, measured $Frames"
Write-Host "  A: $($sides.A.exe) $($ArgsA -join ' ')"
Write-Host "  B: $($sides.B.exe) $($ArgsB -join ' ')"
Write-Host "  data: $RepoDataRoot"

# The quiet check runs before the label directory exists, so an aborted attempt can be
# retried under the same label.
$quiet = Test-Quiet
if (-not $quiet.quiet) {
    foreach ($p in $quiet.problems) { Write-Warning "machine not quiet: $p" }
    if ($RequireQuiet) { throw 'Machine is not quiet (-RequireQuiet); nothing was run.' }
}
New-Item -ItemType Directory -Force $outDir | Out-Null
$machine = [ordered]@{
    label = $Label
    host = $env:COMPUTERNAME
    logical_processors = $LogicalProcessors
    nvidia_smi = $(if ($NvidiaSmi) { $NvidiaSmi.Source } else { $null })
    preflight = $quiet
    runs = @()
}
$config = [ordered]@{
    label = $Label
    created = (Get-Date).ToString('o')
    order = $Order
    warmup_frames = $Warmup
    measured_frames = $Frames
    pipeline_cache = $PipelineCache
    repo_data_root = $RepoDataRoot
    sides = [ordered]@{
        A = [ordered]@{ exe = $sides.A.exe; root = $sides.A.root; args = $ArgsA }
        B = [ordered]@{ exe = $sides.B.exe; root = $sides.B.root; args = $ArgsB }
    }
    extra_args = $ExtraArgs
    require_quiet = [bool]$RequireQuiet
}
function Save-Records($runs) {
    [ordered]@{ config = $config; runs = @($runs) } | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $outDir 'runs.json')
    $machine | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $outDir 'machine-state.json')
}
# --- runs ------------------------------------------------------------------------
$runs = @()
try {
    foreach ($route in $selected) {
        $artifact = Join-Path $RepoDataRoot (Get-M7RModelArtifact $RepoDataRoot $route.Model)
        $manifest = Join-Path $RepoDataRoot $route.Manifest
        $index = 0
        foreach ($side in $Order) {
            $index++
            $s = $sides[$side]
            $stem = "$($route.Key)__$index-$side"
            $profile = Join-Path $outDir "$stem.jsonl"
            $log = Join-Path $outDir "$stem.log"
            $arguments = @(
                '--benchmark', $route.Id, '--benchmark-manifest', $manifest,
                '--cooked-model-artifact', $artifact,
                '--window-size', '3840x2160', '--hidden-window', '--borderless-window',
                '--output-transport', 'sdr', '--no-validation',
                '--profile-cpu', '--profile-gpu', '--profile-cpu-output', $profile,
                '--cache-state', 'fresh-process-os-driver-cache-uncontrolled',
                '--warmup-frames', "$Warmup", '--frame-limit', "$Frames"
            ) + @(Get-M7REngineBaseArgs $s.exe $PipelineCache) + $route.Args + $ExtraArgs + $sideArgs[$side]
            if ($route.Environment) {
                $arguments += @('--cooked-environment-artifact', (Join-Path $RepoDataRoot (Get-M7RModelArtifact $RepoDataRoot $route.Environment)))
            }
            $before = Get-MachineState $StateSampleSeconds
            $watch = [System.Diagnostics.Stopwatch]::StartNew()
            Push-Location $s.root
            try { $exit = Invoke-M7REngine $s.exe $arguments $log } finally { Pop-Location }
            $watch.Stop()
            $after = Get-GpuState
            $machine.runs += [ordered]@{ route = $route.Key; index = $index; side = $side; before = $before; gpu_after = $after }
            $runs += [pscustomobject]@{
                route = $route.Key; index = $index; side = $side; root = $s.root; exe = $s.exe
                profile = $profile; log = $log; exit_code = $exit
                wall_seconds = [math]::Round($watch.Elapsed.TotalSeconds, 2); arguments = $arguments
            }
            $g = $before.gpu
            $gpuText = if ($g) { "{0} MHz {1} W {2} C util {3}%" -f $g['clocks.gr'], $g['power.draw'], $g['temperature.gpu'], $g['utilization.gpu'] } else { 'gpu state n/a' }
            Write-Host ("{0,-14} run {1,2} side {2} exit {3} ({4:n1}s; before: {5})" -f $route.Key, $index, $side, $exit, $watch.Elapsed.TotalSeconds, $gpuText)
            if ($exit -ne 0) { throw "Timing run failed: $log" }
        }
    }
} finally {
    Save-Records $runs
}

if ($SkipSummary) { return }
python (Join-Path $PSScriptRoot 'Summarize-Admission.py') $outDir
if ($LASTEXITCODE -ne 0) { throw 'Summarize-Admission.py failed' }
