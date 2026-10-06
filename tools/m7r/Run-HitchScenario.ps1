# M7R R4c hitch scenario (design section 4, "Hitch scenario"): runs each hitch route
# as fresh native-4K Release processes with --qualification-scripted-changes, in the
# given side order, then summarizes with Analyze-Hitches.py. Harness tooling only.
#
# Routes: H-stress (F3-stress + assets/benchmarks/m7r/hitch-stress.v1.json),
# H-probe (F6-probecap + hitch-probe.v1.json) and, from M7R R4d, H-upload (F1-all +
# hitch-upload.v1.json: mid-run texture uploads; needs an executable with the
# add_textures action, so select routes with -Only for older baselines). Warmup and
# measured frame counts come from each scenario's "runner" block.
#
#   Baseline only (three runs of one build):
#   powershell -File tools/m7r/Run-HitchScenario.ps1 -Label r4c0-baseline -BaselineExe <exe> -Order A,A,A
#   Comparison (R4c.1+): baseline A against candidate B, A,B,B,A per route:
#   powershell -File tools/m7r/Run-HitchScenario.ps1 -Label r4c1 -BaselineExe <r4c0 exe> -CandidateExe <exe>
#   One build, two configurations (-BaselineArgs/-CandidateArgs go to one side only):
#   powershell -File tools/m7r/Run-HitchScenario.ps1 -Label r4d-upload -Only H-upload -BaselineExe <exe> -CandidateExe <exe> -BaselineArgs '--upload-queue legacy-blocking' -CandidateArgs '--upload-queue auto'
#
# Manifests and cooked artifacts come from -ArtifactRoot (default: this checkout),
# which must hold out/m7r/ddc (tools/m7r/Cook-FrozenModels.ps1). Scenario files come
# from this checkout. Results go to <OutRoot>/<Label> (default out/m7r/hitch).
param(
    [Parameter(Mandatory)] [string] $Label,
    [Parameter(Mandatory)] [string] $BaselineExe,
    [string] $CandidateExe = '',
    [string[]] $Order = @('A', 'B', 'B', 'A'),
    [string[]] $Only = @(),
    [string] $ArtifactRoot = '',
    [string] $OutRoot = '',
    [string[]] $ExtraArgs = @(),
    [string[]] $BaselineArgs = @(),
    [string[]] $CandidateArgs = @(),
    # 'off' (default) or a cache directory; see Get-M7RPipelineCacheArgs.
    [string] $PipelineCache = 'off'
)
$ErrorActionPreference = 'Stop'
# powershell -File passes comma lists as one string; accept both forms.
$Order = @($Order | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$ExtraArgs = @($ExtraArgs | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
$sideArgs = @{
    A = @($BaselineArgs | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
    B = @($CandidateArgs | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
}
. (Join-Path $PSScriptRoot 'M7RFixtures.ps1')
$root = Get-M7RRepoRoot
if (-not $ArtifactRoot) { $ArtifactRoot = $root }
$ArtifactRoot = (Resolve-Path $ArtifactRoot).Path
if (-not $OutRoot) { $OutRoot = Join-Path $root 'out/m7r/hitch' }

$exes = @{ A = (Resolve-Path $BaselineExe).Path }
if ($CandidateExe) { $exes.B = (Resolve-Path $CandidateExe).Path }
foreach ($side in $Order) {
    if (-not $exes.ContainsKey($side)) { throw "Side $side has no executable (use -BaselineExe for A, -CandidateExe for B)" }
}

$M7RHitchRoutes = @(
    @{ Key = 'H-stress'; Fixture = 'F3-stress';   Scenario = 'assets/benchmarks/m7r/hitch-stress.v1.json' }
    @{ Key = 'H-probe';  Fixture = 'F6-probecap'; Scenario = 'assets/benchmarks/m7r/hitch-probe.v1.json' }
    @{ Key = 'H-upload'; Fixture = 'F1-all';      Scenario = 'assets/benchmarks/m7r/hitch-upload.v1.json' }
)

$outDir = Join-Path $OutRoot $Label
if (Test-Path $outDir) { throw "Output directory already exists: $outDir" }
New-Item -ItemType Directory -Force $outDir | Out-Null

$runs = @()
foreach ($route in $M7RHitchRoutes) {
    if ($Only.Count -gt 0 -and $Only -notcontains $route.Key) { continue }
    $fixture = $M7RFrozenSet | Where-Object { $_.Key -eq $route.Fixture } | Select-Object -First 1
    if (-not $fixture) { throw "Unknown fixture $($route.Fixture)" }
    $scenarioPath = Join-Path $root $route.Scenario
    $scenario = Get-Content $scenarioPath -Raw | ConvertFrom-Json
    $warmup = [int] $scenario.runner.warmup_frames
    $frames = [int] $scenario.runner.measured_frames
    $artifact = Join-Path $ArtifactRoot (Get-M7RModelArtifact $ArtifactRoot $fixture.Model)
    $manifest = Join-Path $ArtifactRoot $fixture.Manifest
    $index = 0
    foreach ($side in $Order) {
        $index++
        $profile = Join-Path $outDir "$($route.Key)__$index-$side.jsonl"
        $log = Join-Path $outDir "$($route.Key)__$index-$side.log"
        $exe = $exes[$side]
        $arguments = @(
            '--benchmark', $fixture.Id, '--benchmark-manifest', $manifest,
            '--cooked-model-artifact', $artifact,
            '--window-size', '3840x2160', '--hidden-window', '--borderless-window',
            '--output-transport', 'sdr', '--no-validation',
            '--profile-cpu', '--profile-gpu', '--profile-cpu-output', $profile,
            '--cache-state', 'fresh-process-os-driver-cache-uncontrolled',
            '--warmup-frames', "$warmup", '--frame-limit', "$frames",
            '--qualification-scripted-changes', $scenarioPath
        ) + @(Get-M7REngineBaseArgs $exe $PipelineCache) + $fixture.Args + $ExtraArgs + $sideArgs[$side]
        if ($fixture.Environment) {
            $arguments += @('--cooked-environment-artifact', (Join-Path $ArtifactRoot (Get-M7RModelArtifact $ArtifactRoot $fixture.Environment)))
        }
        # Run from the executable's checkout (<root>/out/build/<preset>/bin).
        $exeRoot = (Resolve-Path (Join-Path (Split-Path $exe) '..\..\..\..')).Path
        $started = Get-Date
        Push-Location $exeRoot
        try { $exit = Invoke-M7REngine $exe $arguments $log } finally { Pop-Location }
        $seconds = ((Get-Date) - $started).TotalSeconds
        Write-Host ("{0,-9} run {1} side {2} exit {3} ({4:n0}s)" -f $route.Key, $index, $side, $exit, $seconds)
        if ($exit -ne 0) { throw "Hitch run failed: $log" }
        $runs += [pscustomobject]@{ route = $route.Key; fixture = $fixture.Key; index = $index; side = $side; exe = $exe; args = ($sideArgs[$side] -join ' '); scenario = $route.Scenario; profile = $profile }
    }
}
$runs | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 (Join-Path $outDir 'runs.json')
python (Join-Path $PSScriptRoot 'Analyze-Hitches.py') $outDir
if ($LASTEXITCODE -ne 0) { throw 'Analyze-Hitches.py failed' }
