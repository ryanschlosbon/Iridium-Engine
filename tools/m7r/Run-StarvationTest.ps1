# M7R R5b.3 cook-while-render starvation test (ADR-0015, R5 design section 4.2).
# Harness tooling only; run it on a quiet machine.
#
# For each timing route (T-F1-all, T-F7-stack), four fresh native-4K Release processes
# in A,B,B,A order:
#   A  --qualification-frame-task-probe                       (frame work only)
#   B  --qualification-frame-task-probe
#      --qualification-background-cook <CookSource>           (cooking in a loop)
# The probe joins a fixed frame-critical parallelFor every frame (as R5c's parallel
# extraction will) on both sides; the load re-cooks the source with the DDC bypassed,
# one full cook after another, as Background tasks.
#
# Cook throughput: with -BaselineCookExe (an IridiumCookAsset built before R5b, whose
# glTF texture views cook on a dedicated fork-join), both cook tools cook the source
# into an empty DDC in A,B,B,A order with no rendering; the candidate must reach 90 %
# of the baseline's cooks per minute.
#
# Analyze-Starvation.py then checks the pass criteria:
#   - non-wait CPU median and p99 (B against A) within the A/A-B/B noise band + 3 %;
#   - frame-task worker start latency p99 <= 50 us under cooking;
#   - no frame over 2x the median caused by the frame task waiting;
#   - cook throughput >= 90 % of the pre-R5b dedicated-thread build;
# and that every background cook produced the same artifact bytes.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/m7r/Run-StarvationTest.ps1 `
#       -Label r5b3 -BaselineCookExe <pre-R5b checkout>\out\build\x64-release\bin\IridiumCookAsset.exe
#   ... -Label r5b3-smoke -Warmup 50 -Frames 600 -Only T-F7-stack -CookRepeats 1   (functional check)
param(
    [Parameter(Mandatory)] [string] $Label,
    [string] $Exe = 'out/build/x64-release/bin/IridiumEngine.exe',
    [string] $Cook = 'out/build/x64-release/bin/IridiumCookAsset.exe',
    [string] $BaselineCookExe = '',
    [string] $CookSource = 'models/alfa_romeo/alfa_romeo.gltf',
    [int] $CookRepeats = 2,
    [int] $Warmup = 500,
    [int] $Frames = 10000,
    [string[]] $Only = @(),
    [string] $DataRoot = '',
    [string] $CookWorkDir = 'C:/r5b3-cook',
    [switch] $SkipFrames,
    # 'off' (default) or a cache directory; see Get-M7RPipelineCacheArgs.
    [string] $PipelineCache = 'off'
)
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'M7RFixtures.ps1')
. (Join-Path $PSScriptRoot 'M7RSweepCommon.ps1')
$root = Get-M7RRepoRoot
if (-not $DataRoot) { $DataRoot = Get-M7RMainRoot }
$DataRoot = (Resolve-Path $DataRoot).Path
$exePath = (Resolve-Path (Join-Path $root $Exe)).Path
$outDir = Join-Path $DataRoot "out/m7r/starvation/$Label"
if (Test-Path $outDir) { throw "Output directory already exists: $outDir" }
New-Item -ItemType Directory -Force $outDir | Out-Null

$runs = @()
if (-not $SkipFrames) {
    foreach ($route in $M7RTimingRoutes) {
        if ($Only.Count -gt 0 -and $Only -notcontains $route.Key) { continue }
        $artifact = Join-Path $DataRoot (Get-M7RModelArtifact $DataRoot $route.Model)
        $manifest = Join-Path $DataRoot $route.Manifest
        $index = 0
        foreach ($side in @('A', 'B', 'B', 'A')) {
            $index++
            $profile = Join-Path $outDir "$($route.Key)__$index-$side.jsonl"
            $log = Join-Path $outDir "$($route.Key)__$index-$side.log"
            $arguments = @(
                '--benchmark', $route.Id, '--benchmark-manifest', $manifest,
                '--cooked-model-artifact', $artifact,
                '--window-size', '3840x2160', '--hidden-window', '--borderless-window',
                '--output-transport', 'sdr', '--no-validation',
                '--profile-cpu', '--profile-gpu', '--profile-cpu-output', $profile,
                '--cache-state', 'fresh-process-os-driver-cache-uncontrolled',
                '--warmup-frames', "$Warmup", '--frame-limit', "$Frames",
                '--qualification-frame-task-probe'
            ) + @(Get-M7RPipelineCacheArgs $exePath $PipelineCache) + $route.Args
            if ($side -eq 'B') { $arguments += @('--qualification-background-cook', $CookSource) }
            # The engine runs from its own checkout (shaders); manifests, artifacts
            # and the cook source come from absolute paths or its assets/ root.
            Push-Location $root
            try { $exit = Invoke-M7REngine $exePath $arguments $log } finally { Pop-Location }
            Write-Host ("{0,-11} run {1} side {2} exit {3}" -f $route.Key, $index, $side, $exit)
            if ($exit -ne 0) { throw "Starvation run failed: $log" }
            $runs += [pscustomobject]@{ kind = 'frames'; route = $route.Key; index = $index; side = $side; profile = $profile; log = $log }
        }
    }
}

# Cook throughput without rendering: baseline (pre-R5b) against candidate cook tool.
$cookPath = (Resolve-Path (Join-Path $root $Cook)).Path
$tools = [ordered]@{ B = $cookPath }
if ($BaselineCookExe) { $tools['A'] = (Resolve-Path $BaselineCookExe).Path }
$order = if ($BaselineCookExe) { @('A', 'B', 'B', 'A') } else { @('B') }
$source = Join-Path $root "assets/$CookSource"
for ($repeat = 0; $repeat -lt $CookRepeats; $repeat++) {
    foreach ($side in $order) {
        if (Test-Path $CookWorkDir) { Remove-Item -Recurse -Force $CookWorkDir }
        New-Item -ItemType Directory -Force $CookWorkDir | Out-Null
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $json = (cmd /c "`"$($tools[$side])`" --source `"$source`" --ddc `"$CookWorkDir`" 2>nul") -join "`n"
        $exit = $LASTEXITCODE
        $watch.Stop()
        if ($exit -ne 0) { throw "Cook run failed ($side, exit $exit)" }
        $result = $json | ConvertFrom-Json
        Write-Host ("cook side {0} {1:n2}s {2} hash {3}" -f $side, $watch.Elapsed.TotalSeconds, $result.status, $result.artifactHash.Substring(0, 16))
        $runs += [pscustomobject]@{ kind = 'cook'; side = $side; seconds = $watch.Elapsed.TotalSeconds; status = $result.status; artifactHash = $result.artifactHash }
    }
}
if (Test-Path $CookWorkDir) { Remove-Item -Recurse -Force $CookWorkDir }

$runs | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 (Join-Path $outDir 'runs.json')
python (Join-Path $PSScriptRoot 'Analyze-Starvation.py') $outDir
exit $LASTEXITCODE
