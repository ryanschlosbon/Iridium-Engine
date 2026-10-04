# Helpers for Run-QualificationSweep.ps1 / Compare-QualificationSweep.ps1 (dot-sourced by
# M7RQualificationSweep.ps1). Harness tooling only.

# The checkout that owns out/m7r: the main working tree of this repository (a linked
# worktree's common git dir is <main>/.git).
function Get-M7RMainRoot {
    $common = (git -C (Get-M7RRepoRoot) rev-parse --path-format=absolute --git-common-dir).Trim()
    return (Resolve-Path (Join-Path $common '..')).Path
}

# Cooks the sweep-only inputs into <DataRoot>/out/m7r/ddc (cache hits are cheap) and
# resolves every model/environment the selected entries need to an absolute artifact.
function Initialize-M7RSweepInputs([string] $dataRoot, [string] $cook, $entries) {
    $needed = @{}
    foreach ($e in $entries) {
        if ($e.Model) { $needed[$e.Model] = 'model' }
        if ($e.Environment) { $needed[$e.Environment] = 'environment' }
    }
    $frozenMap = Get-Content (Join-Path $dataRoot 'out/m7r/ddc/artifacts.json') -Raw | ConvertFrom-Json
    $resolved = [ordered]@{}
    Push-Location $dataRoot
    try {
        foreach ($name in ($needed.Keys | Sort-Object)) {
            $frozen = $frozenMap.$name
            if ($frozen) {
                $path = Join-Path $dataRoot $frozen.artifact
                if (-not (Test-Path $path)) { throw "Frozen artifact for $name missing: $path" }
                $resolved[$name] = [ordered]@{ source = $frozen.source; artifact = $path; cookKey = $frozen.cookKey; artifactHash = $frozen.artifactHash; origin = 'frozen' }
                continue
            }
            $spec = $M7RSweepInputs[$name]
            if (-not $spec) { throw "Sweep input $name is neither frozen (artifacts.json) nor in `$M7RSweepInputs" }
            $source = Join-Path 'assets' $spec.Source
            if (-not (Test-Path $source)) { throw "Source for $name not found under $dataRoot\assets: $($spec.Source) (local-only content?)" }
            $metadata = if ($spec.Metadata) { " --metadata `"$($spec.Metadata)`"" } else { '' }
            $json = (cmd /c "`"$cook`" --source `"$source`"$metadata --ddc `"out/m7r/ddc`" 2>nul") -join "`n"
            if ($LASTEXITCODE -ne 0) { throw "Cook failed for $name ($source)" }
            $result = $json | ConvertFrom-Json
            if ($result.status -notin @('built', 'cache-hit')) { throw "Cook status $($result.status) for $source" }
            $key = $result.cookKey
            $path = Join-Path $dataRoot "out/m7r/ddc/$($key.Substring(0, 2))/$($key.Substring(2)).irartifact"
            if (-not (Test-Path $path)) { throw "Artifact not found at $path" }
            $resolved[$name] = [ordered]@{ source = $spec.Source; artifact = $path; cookKey = $key; artifactHash = $result.artifactHash; origin = $result.status }
            Write-Host ("cooked {0,-14} {1} {2}" -f $name, $result.status, $key.Substring(0, 16))
        }
    }
    finally { Pop-Location }
    return $resolved
}

function Get-M7RSweepArguments($entry, $inputs, [string] $entryDir) {
    $size = if ($entry.Size) { $entry.Size } else { '1280x720' }
    $transport = if ($entry.Transport) { $entry.Transport } else { 'sdr' }
    $a = @('--benchmark', $entry.Id, '--benchmark-manifest', $entry.Manifest)
    if ($entry.Model) { $a += @('--cooked-model-artifact', $inputs[$entry.Model].artifact) }
    if ($entry.Environment) { $a += @('--cooked-environment-artifact', $inputs[$entry.Environment].artifact) }
    $a += @('--window-size', $size, '--hidden-window', '--borderless-window', '--output-transport', $transport,
        '--warmup-frames', "$($entry.Warmup)", '--frame-limit', "$($entry.Frames)")
    $a += $(if ($entry.Validation) { '--validation' } else { '--no-validation' })
    if ($entry.Profile -ne $false) { $a += @('--profile-cpu-output', (Join-Path $entryDir 'profile.jsonl')) }
    if ($entry.Capture) {
        $a += @('--capture-frame', "$($entry.Capture.Frame)", '--capture-directory', (Join-Path $entryDir 'capture'),
            '--capture-point', $entry.Capture.Point)
        if ($entry.Capture.RequireSignal -ne $false) { $a += '--require-capture-signal' }
    }
    return @($a + $entry.Args)
}

function Format-M7RCommandLine([string] $exe, [string[]] $arguments) {
    $quoted = @($exe) + $arguments | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
    return ($quoted -join ' ')
}

# Like Invoke-M7REngine (stdout and stderr merged into $log) but bounded: a hung run is
# killed with its process tree and reported as exit -1.
function Invoke-M7RSweepProcess([string] $exe, [string[]] $arguments, [string] $log, [int] $timeoutSeconds) {
    $quoted = $arguments | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
    $command = "`"`"$exe`" $($quoted -join ' ') > `"$log`" 2>&1`""
    $process = Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', $command -NoNewWindow -PassThru
    $handle = $process.Handle   # keep the handle so ExitCode survives process exit
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
        cmd /c "taskkill /T /F /PID $($process.Id) >nul 2>&1"
        Add-Content -Path $log -Value "SWEEP: killed after $timeoutSeconds s timeout"
        return -1
    }
    $process.WaitForExit()
    return $process.ExitCode
}

function ConvertFrom-M7RJsonOrNull([string] $text) {
    try { return ($text | ConvertFrom-Json) } catch { return $null }
}

# Aggregates every per-frame counter of a --profile-cpu-output JSONL file (count, min,
# max, first, last, sum over retained frames) plus the run header's configuration
# blocks and the run summary's frame/memory blocks. Timing ranges are not recorded.
function Read-M7RProfileSummary([string] $path) {
    $counters = @{}
    $header = $null; $summary = $null; $frames = 0
    foreach ($line in [System.IO.File]::ReadLines($path)) {
        if (-not $line) { continue }
        $record = $line | ConvertFrom-Json
        switch ($record.type) {
            'run_header' { $header = $record }
            'run_summary' { $summary = $record }
            'frame' {
                $frames++
                foreach ($c in $record.counters) {
                    $v = [double]$c.value
                    $s = $counters[$c.name]
                    if (-not $s) {
                        $counters[$c.name] = @{ n = 1; min = $v; max = $v; first = $v; last = $v; sum = $v; unit = $c.unit; status = $c.status }
                    }
                    else {
                        $s.n++; $s.last = $v; $s.sum += $v
                        if ($v -lt $s.min) { $s.min = $v }
                        if ($v -gt $s.max) { $s.max = $v }
                        if ($c.status -ne $s.status) { $s.status = 'mixed' }
                    }
                }
            }
        }
    }
    $ordered = [ordered]@{}
    foreach ($name in ($counters.Keys | Sort-Object)) {
        $s = $counters[$name]
        $ordered[$name] = [ordered]@{ n = $s.n; min = $s.min; max = $s.max; first = $s.first; last = $s.last; sum = $s.sum; unit = $s.unit; status = $s.status }
    }
    $h = [ordered]@{}
    if ($header) {
        foreach ($k in @('validation_enabled', 'requested_window_size', 'render_extent', 'frame_limit', 'warmup_frames',
                'measured_frames', 'profiling', 'display', 'render_configuration', 'benchmark', 'model_input',
                'environment_input', 'startup')) {
            if ($null -ne $header.$k) { $h[$k] = $header.$k }
        }
    }
    $s = [ordered]@{}
    if ($summary) {
        foreach ($k in @('frames_retained', 'frames_completed', 'dropped_frames', 'aggregate_storage', 'memory_latest')) {
            if ($null -ne $summary.$k) { $s[$k] = $summary.$k }
        }
        $s['cpu_range_names'] = @($summary.cpu_ranges.PSObject.Properties | ForEach-Object { $_.Name } | Sort-Object)
        $s['gpu_range_names'] = @($summary.gpu_ranges.PSObject.Properties | ForEach-Object { $_.Name } | Sort-Object)
    }
    return [ordered]@{ frames = $frames; header = $h; summary = $s; counters = $ordered }
}

# Parses one entry's log, profile and capture directory into its sweep record and
# applies the entry's pass rules.
function Read-M7RSweepEntry($entry, [string] $entryDir, [string] $log, [int] $exit) {
    # ReadAllLines, not Get-Content: Get-Content strings carry PSPath/PSProvider notes
    # that ConvertTo-Json would serialize recursively.
    $lines = if (Test-Path $log) { [System.IO.File]::ReadAllLines($log) } else { @() }
    $iridium = @()
    $fatal = @()
    $validation = 0
    foreach ($line in $lines) {
        if ($line -match '^\[Validation\]:') { $validation++; continue }
        if ($line -match '^(IRIDIUM_[A-Z0-9_]+)\s*(.*)$') {
            $payload = $Matches[2]
            $json = if ($payload) { ConvertFrom-M7RJsonOrNull $payload } else { $null }
            $iridium += [pscustomobject][ordered]@{ tag = $Matches[1]; json = $json; raw = $(if ($null -eq $json) { $payload } else { $null }) }
            continue
        }
        if ($line -match 'Fatal Error:|SWEEP: killed') { $fatal += $line }
    }
    $problems = @()
    $profilePath = Join-Path $entryDir 'profile.jsonl'
    $profile = $null
    if (Test-Path $profilePath) {
        try { $profile = Read-M7RProfileSummary $profilePath } catch { $problems += "profile unreadable: $($_.Exception.Message)" }
    }
    $captures = @()
    $captureDir = Join-Path $entryDir 'capture'
    if (Test-Path $captureDir) {
        foreach ($sidecar in (Get-ChildItem $captureDir -Filter '*.json' | Sort-Object Name)) {
            $meta = Get-Content $sidecar.FullName -Raw | ConvertFrom-Json
            $captures += [pscustomobject][ordered]@{ stem = $sidecar.BaseName; point = $entry.Capture.Point; sha256 = $meta.image.sha256; metadata = $meta }
        }
    }

    # Pass rules: every expected tag present, no payload with passed=false, every
    # required counter nonzero, no counter whose name says mismatch is nonzero, the
    # requested capture exists, and no validation message or fatal error.
    foreach ($tag in @($entry.ExpectTags)) {
        if ($tag -and -not ($iridium | Where-Object { $_.tag -eq $tag })) { $problems += "missing $tag" }
    }
    foreach ($i in $iridium) {
        if ($i.json -and ($i.json.PSObject.Properties.Name -contains 'passed') -and $i.json.passed -ne $true) { $problems += "$($i.tag) passed=false" }
    }
    if ($entry.Profile -ne $false -and -not $profile) { $problems += 'no profile' }
    if ($profile) {
        foreach ($name in @($entry.RequireCounters)) {
            if (-not $name) { continue }
            $c = $profile.counters[$name]
            if (-not $c) { $problems += "counter $name absent" }
            elseif ($c.max -le 0) { $problems += "counter $name is zero" }
        }
        foreach ($name in $profile.counters.Keys) {
            if ($name -match 'mismatch' -and $profile.counters[$name].max -gt 0) { $problems += "counter $name = $($profile.counters[$name].max)" }
        }
    }
    if ($entry.Capture -and $captures.Count -eq 0) { $problems += 'no capture' }
    if ($validation -gt 0) { $problems += "$validation validation messages" }
    $problems += $fatal
    if ($exit -ne 0) { $problems += "exit $exit" }
    return [pscustomobject][ordered]@{
        key = $entry.Key; capability = $entry.Capability; fixture = $entry.Id; manifest = $entry.Manifest
        model = $entry.Model; environment = $entry.Environment; notes = $entry.Notes
        status = $(if ($problems.Count) { 'FAIL' } else { 'pass' }); problems = $problems
        exit = $exit; validationMessages = $validation; fatal = $fatal
        iridium = $iridium; profile = $profile; captures = $captures; derived = [pscustomobject]@{}
    }
}

function Write-M7RSweepSummary($report, [string] $path) {
    $md = @("# Qualification sweep $($report.label)", '',
        "Commit ``$($report.commit)`` (dirty tracked files: $($report.dirtyTrackedFiles)); exe SHA-256 ``$($report.executableSha256.Substring(0, 16))``; implicit layers disabled: $($report.implicitLayersDisabled).", '',
        '| Entry | Capability | Fixture | Status | Exit | Val msgs | IRIDIUM tags | Capture SHA-256 (16) | Seconds | Problems |',
        '|---|---|---|---|---:|---:|---|---|---:|---|')
    foreach ($r in $report.results) {
        $tags = (@($r.iridium | ForEach-Object { $_.tag -replace '^IRIDIUM_', '' } | Where-Object { $_ -notin @('TRANSPARENCY_EXECUTION', 'FRAME_TOPOLOGY_PREWARM', 'RUN_METRICS', 'CAPTURE') } | Select-Object -Unique) -join ', ')
        $sha = (@($r.captures | ForEach-Object { $_.sha256.Substring(0, 16) }) -join ' ')
        $shaCell = if ($sha) { '`' + $sha + '`' } else { '-' }
        $problemCell = ($r.problems -join '; ') -replace '\|', '/'
        $md += "| $($r.key) | ``$($r.capability)`` | ``$($r.fixture)`` | $($r.status) | $($r.exit) | $($r.validationMessages) | $tags | $shaCell | $($r.seconds) | $problemCell |"
    }
    $md | Set-Content -Encoding utf8 $path
}
