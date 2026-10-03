# Records the indirect command-stream digest (--qualification-indirect-stream-digest,
# M7R R3a.0) on the digest fixtures and compares two recorded labels. Harness tooling only.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/m7r/Run-IndirectDigest.ps1 -Label r3a0
#   ... -Label r3a2 -Exe <worktree>\out\build\x64-release\bin\IridiumEngine.exe -Compare r3a0
#   ... -Label r3a2 -Compare r3a0 -NoRun          (compare two existing labels)
#   ... -Label r3a0-ext -Extended                 (adds the shadow/probe LOD routes)
#
# Each fixture runs exactly like the frozen capture route (native 3840x2160, SDR, 12 warm-up
# + 6 measured frames) without the capture itself. Output goes to <DataRoot>/out/m7r/digests/
# <Label>: one engine log per fixture plus digest.json (every IRIDIUM_INDIRECT_STREAM_DIGEST
# payload). -DataRoot is the checkout that owns out/m7r (cooked DDC); it defaults to the main
# checkout of this repository, so a worktree build reuses its cooked artifacts. A label is
# never overwritten.
param(
    [Parameter(Mandatory)] [string] $Label,
    [string] $Exe = 'out/build/x64-release/bin/IridiumEngine.exe',
    [string] $Compare = '',
    [switch] $NoRun,
    [string[]] $Only = @(),
    [switch] $Extended,
    [string] $DataRoot = ''
)
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'M7RFixtures.ps1')
$repo = Get-M7RRepoRoot
if (-not $DataRoot) {
    $common = (git -C $repo rev-parse --path-format=absolute --git-common-dir).Trim()
    $DataRoot = (Resolve-Path (Join-Path $common '..')).Path
}
$DataRoot = (Resolve-Path $DataRoot).Path
$digestRoot = Join-Path $DataRoot 'out/m7r/digests'

# The R3a.0 design's digest fixtures, keyed as in $M7RFrozenSet.
$M7RDigestFixtures = @('F1-all', 'F3-stress', 'F5-hetero', 'F5-point', 'F6-probecap', 'F7-hiz', 'F7-lod')
# -Extended: the shadow and probe resident-LOD routes, which no frozen fixture enables.
$M7RDigestExtended = @(
    @{ Key = 'X-shadow-lod'; Id = 'm7_directional_shadow_lod_near_mid_far_v1'; Manifest = 'assets/m7-directional-shadow-lod-manifest.v1.json'
       Model = 'alfa-lod'; Args = @('--experimental-shadow-lod-error-texels', '2', '--shadow-lod-max-level', '15') }
    @{ Key = 'X-probe-lod'; Id = 'm7_probe_lod_reflection_motion_v1'; Manifest = 'assets/m7-probe-lod-admission-manifest.v1.json'
       Model = 'alfa-lod'; Environment = 'belfast-env'; Args = @('--validate-reflection-probes', '--experimental-probe-lod-error-pixels', '8') }
)
$digestSet = @($M7RFrozenSet | Where-Object { $M7RDigestFixtures -contains $_.Key })
if ($Extended) { $digestSet += $M7RDigestExtended }

function Read-DigestLabel([string] $label) {
    $path = Join-Path $digestRoot "$label/digest.json"
    if (-not (Test-Path $path)) { throw "No digest recorded for label ${label}: $path" }
    return Get-Content $path -Raw | ConvertFrom-Json
}

if (-not $NoRun) {
    $outDir = Join-Path $digestRoot $Label
    if (Test-Path $outDir) { throw "Digest label already exists: $outDir (digests are never overwritten)" }
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
    Push-Location $DataRoot
    try {
        $exePath = (Resolve-Path (Join-Path $repo $Exe) -ErrorAction SilentlyContinue)
        if (-not $exePath) { $exePath = Resolve-Path $Exe }
        $exePath = $exePath.Path
        $fixtures = @()
        foreach ($fixture in $digestSet) {
            if ($Only.Count -gt 0 -and $Only -notcontains $fixture.Key) { continue }
            $log = Join-Path $outDir "$($fixture.Key).log"
            $arguments = @(
                '--benchmark', $fixture.Id, '--benchmark-manifest', (Join-Path $DataRoot $fixture.Manifest),
                '--cooked-model-artifact', (Join-Path $DataRoot (Get-M7RModelArtifact $DataRoot $fixture.Model)),
                '--window-size', '3840x2160', '--hidden-window', '--borderless-window',
                '--output-transport', 'sdr', '--warmup-frames', '12', '--frame-limit', '6',
                '--no-validation', '--qualification-indirect-stream-digest'
            ) + $fixture.Args
            if ($fixture.Environment) {
                $arguments += @('--cooked-environment-artifact', (Join-Path $DataRoot (Get-M7RModelArtifact $DataRoot $fixture.Environment)))
            }
            $exit = Invoke-M7REngine $exePath $arguments $log
            $streams = @(); $aggregates = @()
            foreach ($line in Get-Content $log) {
                if ($line -notmatch '^IRIDIUM_INDIRECT_STREAM_DIGEST (\{.*\})$') { continue }
                $payload = $Matches[1] | ConvertFrom-Json
                if ($payload.aggregate) { $aggregates += $payload } else { $streams += $payload }
            }
            $all = $aggregates | Where-Object { $_.view -eq 'all' } | Select-Object -First 1
            $fixtures += [pscustomobject]@{ key = $fixture.Key; exit = $exit; streams = $streams; aggregates = $aggregates }
            $status = if ($exit -eq 0 -and $all) { 'ok' } else { 'FAILED' }
            Write-Host ("{0,-12} {1} streams={2} digest={3} orphans={4} unretired={5}" -f $fixture.Key, $status,
                $(if ($all) { $all.streams } else { '-' }), $(if ($all) { $all.digest } else { '-' }),
                $(if ($all) { $all.orphan_events } else { '-' }), $(if ($all) { $all.unretired } else { '-' }))
        }
        $git = (git -C $repo rev-parse HEAD).Trim()
        $exeSha = (Get-FileHash $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
        [pscustomobject]@{ label = $Label; commit = $git; executable = $exePath; executableSha256 = $exeSha
            recorded = (Get-Date).ToString('o'); fixtures = $fixtures } |
            ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $outDir 'digest.json')
        Write-Host "Wrote $outDir (commit $git)"
        $failed = @($fixtures | Where-Object { $_.exit -ne 0 }).Count
        if ($failed -gt 0) { Write-Host "$failed fixture run(s) failed"; exit 1 }
    }
    finally { Pop-Location }
}

if ($Compare) {
    $baseline = Read-DigestLabel $Compare
    $candidate = Read-DigestLabel $Label
    $differences = 0
    foreach ($base in $baseline.fixtures) {
        if ($Only.Count -gt 0 -and $Only -notcontains $base.key) { continue }
        $cand = $candidate.fixtures | Where-Object { $_.key -eq $base.key } | Select-Object -First 1
        if (-not $cand) { Write-Host ("{0,-12} MISSING in {1}" -f $base.key, $Label); ++$differences; continue }
        $baseStreams = @($base.streams); $candStreams = @($cand.streams)
        $mismatch = $null
        $count = [math]::Max($baseStreams.Count, $candStreams.Count)
        for ($i = 0; $i -lt $count; ++$i) {
            $b = if ($i -lt $baseStreams.Count) { $baseStreams[$i] } else { $null }
            $c = if ($i -lt $candStreams.Count) { $candStreams[$i] } else { $null }
            if (-not $b -or -not $c -or $b.view -ne $c.view -or $b.sequence -ne $c.sequence -or $b.digest -ne $c.digest) {
                $parts = @()
                if ($b -and $c) {
                    foreach ($field in 'slot', 'dispatches', 'draws', 'log', 'host', 'device') {
                        if ($b.$field -ne $c.$field) { $parts += $field }
                    }
                }
                $mismatch = "stream #$i ($(if ($b) { "$($b.view)/$($b.sequence)" } else { '-' }) vs $(if ($c) { "$($c.view)/$($c.sequence)" } else { '-' })) differs in: $($parts -join ',')"
                break
            }
        }
        $baseAll = $base.aggregates | Where-Object { $_.view -eq 'all' } | Select-Object -First 1
        $candAll = $cand.aggregates | Where-Object { $_.view -eq 'all' } | Select-Object -First 1
        if (-not $mismatch -and ($baseAll.digest -ne $candAll.digest -or $candAll.orphan_events -ne 0 -or $candAll.unretired -ne 0)) {
            $mismatch = "aggregate differs ($($baseAll.digest) vs $($candAll.digest), orphans $($candAll.orphan_events), unretired $($candAll.unretired))"
        }
        if ($mismatch) { ++$differences; Write-Host ("{0,-12} DIFFERENT {1}" -f $base.key, $mismatch) }
        else { Write-Host ("{0,-12} identical ({1} streams, {2})" -f $base.key, $baseStreams.Count, $baseAll.digest) }
    }
    if ($differences -gt 0) { Write-Host "$differences fixture(s) differ from $Compare"; exit 1 }
    Write-Host "All fixtures identical to $Compare"
}
