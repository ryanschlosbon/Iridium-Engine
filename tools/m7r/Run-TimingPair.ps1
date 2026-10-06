# Matched native-4K Release timing pair for the M7R refactor evidence tier: for each
# timing route, runs baseline (A) and candidate (B) in A,B,B,A order as fresh
# processes, then summarizes with Summarize-Profiles.py. Each root is a checkout with
# its own out/build/x64-release and assets/shaders (use a git worktree for the
# baseline commit). Manifests and cooked artifacts come from -ArtifactRoot (default:
# this repository), which must hold out/m7r/ddc; a worktree passes the main checkout.
# Results go to <OutRoot>/<Label> (default: out/m7r/timing of this repository).
#
#   powershell -File tools/m7r/Run-TimingPair.ps1 -Label r1 -BaselineRoot out/m7r/worktrees/r0
#   ... -Label x -BaselineRoot <main> -ArtifactRoot <main> -Only T-F5-hetero,T-F6-probecap
param(
    [Parameter(Mandatory)] [string] $Label,
    [Parameter(Mandatory)] [string] $BaselineRoot,
    [string] $CandidateRoot = '',
    [int] $Warmup = 500,
    [int] $Frames = 10000,
    [string[]] $Only = @(),
    [string[]] $ExtraArgs = @(),
    [string] $ArtifactRoot = '',
    [string] $OutRoot = '',
    # 'off' (default) or a cache directory; see Get-M7RPipelineCacheArgs.
    [string] $PipelineCache = 'off'
)
$ErrorActionPreference = 'Stop'
# powershell -File passes comma lists as one string; accept both forms.
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$ExtraArgs = @($ExtraArgs | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'M7RFixtures.ps1')
$root = Get-M7RRepoRoot
if (-not $CandidateRoot) { $CandidateRoot = $root }
if (-not $ArtifactRoot) { $ArtifactRoot = $root }
$ArtifactRoot = (Resolve-Path $ArtifactRoot).Path
if (-not $OutRoot) { $OutRoot = Join-Path $root 'out/m7r/timing' }
$roots = @{ A = (Resolve-Path $BaselineRoot).Path; B = (Resolve-Path $CandidateRoot).Path }
$outDir = Join-Path $OutRoot $Label
if (Test-Path $outDir) { throw "Output directory already exists: $outDir" }
New-Item -ItemType Directory -Force $outDir | Out-Null

$runs = @()
foreach ($route in $M7RTimingRoutes) {
    if ($Only.Count -gt 0 -and $Only -notcontains $route.Key) { continue }
    $artifact = Join-Path $ArtifactRoot (Get-M7RModelArtifact $ArtifactRoot $route.Model)
    $manifest = Join-Path $ArtifactRoot $route.Manifest
    $index = 0
    foreach ($side in @('A', 'B', 'B', 'A')) {
        $index++
        $profile = Join-Path $outDir "$($route.Key)__$index-$side.jsonl"
        $log = Join-Path $outDir "$($route.Key)__$index-$side.log"
        $exe = Join-Path $roots[$side] 'out/build/x64-release/bin/IridiumEngine.exe'
        $arguments = @(
            '--benchmark', $route.Id, '--benchmark-manifest', $manifest,
            '--cooked-model-artifact', $artifact,
            '--window-size', '3840x2160', '--hidden-window', '--borderless-window',
            '--output-transport', 'sdr', '--no-validation',
            '--profile-cpu', '--profile-gpu', '--profile-cpu-output', $profile,
            '--cache-state', 'fresh-process-os-driver-cache-uncontrolled',
            '--warmup-frames', "$Warmup", '--frame-limit', "$Frames"
        ) + @(Get-M7REngineBaseArgs $exe $PipelineCache) + $route.Args + $ExtraArgs
        if ($route.Environment) {
            $arguments += @('--cooked-environment-artifact', (Join-Path $ArtifactRoot (Get-M7RModelArtifact $ArtifactRoot $route.Environment)))
        }
        Push-Location $roots[$side]
        try { $exit = Invoke-M7REngine $exe $arguments $log } finally { Pop-Location }
        Write-Host ("{0,-11} run {1} side {2} exit {3}" -f $route.Key, $index, $side, $exit)
        if ($exit -ne 0) { throw "Timing run failed: $log" }
        $runs += [pscustomobject]@{ route = $route.Key; index = $index; side = $side; root = $roots[$side]; profile = $profile }
    }
}
$runs | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 (Join-Path $outDir 'runs.json')
python (Join-Path $PSScriptRoot 'Summarize-Profiles.py') $outDir
if ($LASTEXITCODE -ne 0) { throw 'Summarize-Profiles.py failed' }
