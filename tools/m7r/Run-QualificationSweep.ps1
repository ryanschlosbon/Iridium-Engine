# Runs every qualification capability of the engine once on its fixture (the table in
# M7RQualificationSweep.ps1) and records structured output, so the R2 harness move can
# be diffed with Compare-QualificationSweep.ps1. Harness tooling only.
#
# Per entry: exact command line, exit code, elapsed time, `[Validation]:` count, every
# `IRIDIUM_*` stdout line (JSON payload parsed), CPU-profile counter aggregates, and
# capture SHA-256 plus sidecar metadata. Output goes to <DataRoot>/out/m7r/sweeps/<Label>
# (sweep.json, summary.md, one directory per entry); an existing label is never
# overwritten.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/m7r/Run-QualificationSweep.ps1 -Label r2-baseline
#   ... -Label r2-x -Only V04-ord2-resize,O01-shadow-oracle
#
# -DataRoot is the checkout that owns out/m7r (cooked DDC, sweeps); it defaults to the
# main checkout of this repository, so a worktree build reuses its cooked artifacts.
# The engine runs with its own checkout as the working directory (shaders, manifests).
# Alfa entries (R04/R05, O02-O04, X01) also need the local-only model sources under the
# run root's assets/models/alfa_romeo (manifest content hashes). In a worktree use real
# files or hard links: a directory junction resolves outside assets/ and the manifest
# loader rejects it ("content path escapes manifest directory"). Never commit them.
param(
    [Parameter(Mandatory)] [string] $Label,
    [string] $Exe = 'out/build/x64-release/bin/IridiumEngine.exe',
    [string] $Cook = 'out/build/x64-release/bin/IridiumCookAsset.exe',
    [string] $DataRoot = '',
    [string[]] $Only = @(),
    [int] $TimeoutSeconds = 600,
    [switch] $AllowImplicitLayers,
    # 'off' (default) or a cache directory; see Get-M7RPipelineCacheArgs.
    [string] $PipelineCache = 'off'
)
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'M7RQualificationSweep.ps1')
$root = Get-M7RRepoRoot
if (-not $DataRoot) { $DataRoot = Get-M7RMainRoot }
$DataRoot = (Resolve-Path $DataRoot).Path

Push-Location $root
try {
    $exePath = (Resolve-Path $Exe).Path
    $cookPath = (Resolve-Path $Cook).Path
    $cacheArgs = @(Get-M7RPipelineCacheArgs $exePath $PipelineCache)
    $outDir = Join-Path $DataRoot "out/m7r/sweeps/$Label"
    if (Test-Path $outDir) { throw "Sweep label already exists: $outDir (sweeps are never overwritten)" }
    New-Item -ItemType Directory -Force $outDir | Out-Null

    # Implicit loader layers (overlays, ReShade, capture tools) inject into the device and
    # can emit loader messages; disable them unless asked, as the vulkan ctest label does.
    if (-not $AllowImplicitLayers) { $env:VK_LOADER_LAYERS_DISABLE = '~implicit~' }

    $selected = @($M7RQualificationSweep | Where-Object { $Only.Count -eq 0 -or $Only -contains $_.Key })
    if ($selected.Count -eq 0) { throw "No sweep entries match -Only $($Only -join ',')" }
    $inputs = Initialize-M7RSweepInputs $DataRoot $cookPath $selected

    $results = @()
    foreach ($entry in $selected) {
        $entryDir = Join-Path $outDir $entry.Key
        New-Item -ItemType Directory -Force $entryDir | Out-Null
        $arguments = @(Get-M7RSweepArguments $entry $inputs $entryDir) + $cacheArgs
        $log = Join-Path $entryDir 'engine.log'
        $commandLine = Format-M7RCommandLine $exePath $arguments
        Set-Content -Encoding utf8 (Join-Path $entryDir 'command.txt') $commandLine
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        $exit = Invoke-M7RSweepProcess $exePath $arguments $log $TimeoutSeconds
        $watch.Stop()
        $record = Read-M7RSweepEntry $entry $entryDir $log $exit
        $record | Add-Member seconds ([math]::Round($watch.Elapsed.TotalSeconds, 1))
        $record | Add-Member commandLine $commandLine
        $record | Add-Member arguments @($arguments)
        $results += $record
        Write-Host ("{0,-24} {1,-5} exit={2} validation={3} iridium={4} captures={5} {6:n1}s{7}" -f $entry.Key,
            $record.status, $exit, $record.validationMessages, @($record.iridium).Count, @($record.captures).Count,
            $watch.Elapsed.TotalSeconds, $(if ($record.problems.Count) { '  ' + ($record.problems -join '; ') } else { '' }))
    }

    # Cross-entry relations (reference-direct routes must reproduce the automatic capture).
    foreach ($r in $results) {
        $entry = $selected | Where-Object { $_.Key -eq $r.key }
        if (-not $entry.MatchCapture) { continue }
        $other = $results | Where-Object { $_.key -eq $entry.MatchCapture }
        $mine = @($r.captures | ForEach-Object { $_.sha256 })
        $theirs = @(if ($other) { $other.captures | ForEach-Object { $_.sha256 } })
        $match = ($mine.Count -gt 0) -and ($mine.Count -eq $theirs.Count) -and (-not (Compare-Object $mine $theirs))
        $relation = [ordered]@{ entry = $entry.MatchCapture; identical = [bool]$match }
        $within = $match
        if (-not $match -and $mine.Count -eq 1 -and $theirs.Count -eq 1) {
            # Pixel statistics for the record; MatchTolerance (Diff-Images.py limits) may accept.
            $ext = if ($entry.Capture.Point -eq 'scene') { 'pfm' } else { 'tga' }
            $ia = Join-Path $outDir "$($entry.MatchCapture)/capture/$($other.captures[0].stem).$ext"
            $ib = Join-Path $outDir "$($r.key)/capture/$($r.captures[0].stem).$ext"
            $limits = @($entry.MatchTolerance)
            $report = @(python (Join-Path $PSScriptRoot 'Diff-Images.py') $ia $ib @limits)
            $relation['diff'] = @($report | ForEach-Object { [string]$_ })
            if ($limits.Count -gt 0) { $within = ($LASTEXITCODE -eq 0); $relation['withinTolerance'] = [bool]$within; $relation['tolerance'] = $limits }
        }
        $r.derived | Add-Member captureMatches $relation
        if (-not $within -and $entry.MatchRequired -ne $false) { $r.problems += "capture differs from $($entry.MatchCapture)"; $r.status = 'FAIL' }
    }

    $git = (git rev-parse HEAD).Trim()
    $dirty = [bool](git status --porcelain --untracked-files=no | Where-Object { $_ -notmatch 'imgui\.ini$' })
    $report = [pscustomobject][ordered]@{
        schema = 'iridium.m7r.qualification_sweep'; schemaVersion = 1
        label = $Label; commit = $git; dirtyTrackedFiles = $dirty; runRoot = $root; dataRoot = $DataRoot
        executable = $exePath; executableSha256 = (Get-FileHash $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
        implicitLayersDisabled = (-not $AllowImplicitLayers); started = (Get-Date).ToString('o')
        inputs = $inputs; results = $results
    }
    $report | ConvertTo-Json -Depth 40 | Set-Content -Encoding utf8 (Join-Path $outDir 'sweep.json')
    Write-M7RSweepSummary $report (Join-Path $outDir 'summary.md')
    $failed = @($results | Where-Object { $_.status -ne 'pass' }).Count
    Write-Host "Wrote $outDir (commit $git, entries $($results.Count), not passing $failed)"
    if ($failed -gt 0) { exit 1 }
}
finally { Pop-Location }
