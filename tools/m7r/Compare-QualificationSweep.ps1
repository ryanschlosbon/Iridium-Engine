# Diffs two Run-QualificationSweep results entry by entry: status, exit code,
# validation-message count, every IRIDIUM_* payload field, profile counter aggregates and
# configuration blocks, and capture hashes plus sidecar metadata. Volatile fields
# (timestamps, durations, paths, process/run ids, commit/dirty, wall-clock) are ignored
# by $M7RSweepVolatileFields; entries may add their own patterns (Volatile) or a capture
# tolerance class (Tolerance, checked with Diff-Images.py against $M7RTolerances).
# Prints identical/changed per entry and exits non-zero on any non-volatile difference.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/m7r/Compare-QualificationSweep.ps1 -Baseline r2-baseline -Candidate r2-baseline-repeat
#
# -Baseline/-Candidate take a label under <DataRoot>/out/m7r/sweeps or a path to a
# sweep.json. -Ignore adds regexes on flattened field paths for a deliberate change
# (say why in the evidence log); -ShowAll prints every differing path, not the first 25;
# -ListVolatile lists the volatile paths that actually differed.
param(
    [Parameter(Mandatory)] [string] $Baseline,
    [Parameter(Mandatory)] [string] $Candidate,
    [string] $DataRoot = '',
    [string[]] $Ignore = @(),
    [switch] $ShowAll,
    [switch] $ListVolatile
)
$ErrorActionPreference = 'Stop'
$Ignore = @($Ignore | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'M7RQualificationSweep.ps1')
if (-not $DataRoot) { $DataRoot = Get-M7RMainRoot }

function Resolve-Sweep([string] $name) {
    $path = if (Test-Path $name -PathType Leaf) { $name } else { Join-Path $DataRoot "out/m7r/sweeps/$name/sweep.json" }
    if (-not (Test-Path $path)) { throw "Sweep not found: $path" }
    $json = Get-Content $path -Raw | ConvertFrom-Json
    return [pscustomobject]@{ path = (Resolve-Path $path).Path; dir = (Split-Path (Resolve-Path $path).Path); json = $json }
}

# Flattens a parsed JSON value into path -> canonical string. IRIDIUM lines are keyed by
# tag and occurrence so an inserted line of another tag does not shift every index.
function Add-Flat($map, [string] $prefix, $value) {
    if ($null -eq $value) { $map[$prefix] = 'null'; return }
    if ($value -is [System.Management.Automation.PSCustomObject]) {
        $props = @($value.PSObject.Properties)
        if ($props.Count -eq 0) { $map[$prefix] = '{}' }
        foreach ($p in $props) { Add-Flat $map "$prefix.$($p.Name)" $p.Value }
        return
    }
    if ($value -is [System.Collections.IDictionary]) {
        foreach ($k in $value.Keys) { Add-Flat $map "$prefix.$k" $value[$k] }
        return
    }
    if ($value -is [array]) {
        if ($value.Count -eq 0) { $map[$prefix] = '[]' }
        for ($i = 0; $i -lt $value.Count; $i++) { Add-Flat $map "$prefix[$i]" $value[$i] }
        return
    }
    if ($value -is [bool]) { $map[$prefix] = $value.ToString().ToLowerInvariant(); return }
    if ($value -is [double] -or $value -is [single] -or $value -is [decimal]) {
        $map[$prefix] = ([double]$value).ToString('R', [System.Globalization.CultureInfo]::InvariantCulture); return
    }
    $map[$prefix] = [string]$value
}

function Get-FlatEntry($r) {
    $map = [ordered]@{}
    $map['status'] = [string]$r.status
    $map['exit'] = [string]$r.exit
    $map['validationMessages'] = [string]$r.validationMessages
    Add-Flat $map 'fatal' @($r.fatal)
    $seen = @{}
    foreach ($line in @($r.iridium)) {
        $n = if ($seen.ContainsKey($line.tag)) { $seen[$line.tag] + 1 } else { 0 }
        $seen[$line.tag] = $n
        if ($null -ne $line.json) { Add-Flat $map "iridium.$($line.tag)#$n" $line.json }
        else { $map["iridium.$($line.tag)#$n"] = [string]$line.raw }
    }
    foreach ($tag in $seen.Keys) { $map["iridium.$tag.count"] = [string]($seen[$tag] + 1) }
    if ($r.profile) { Add-Flat $map 'profile' $r.profile }
    $captures = @($r.captures)
    $map['captures.count'] = [string]$captures.Count
    for ($i = 0; $i -lt $captures.Count; $i++) {
        $map["captures[$i].sha256"] = [string]$captures[$i].sha256
        Add-Flat $map "captures[$i].metadata" $captures[$i].metadata
    }
    Add-Flat $map 'derived' $r.derived
    return $map
}

function Test-Volatile([string] $path, [string[]] $patterns) {
    foreach ($p in $patterns) { if ($path -match $p) { return $true } }
    return $false
}

$base = Resolve-Sweep $Baseline
$cand = Resolve-Sweep $Candidate
$table = @{}; foreach ($e in $M7RQualificationSweep) { $table[$e.Key] = $e }
$bMap = @{}; foreach ($r in $base.json.results) { $bMap[$r.key] = $r }
$cMap = @{}; foreach ($r in $cand.json.results) { $cMap[$r.key] = $r }
Write-Host "baseline  $($base.json.label) $($base.json.commit.Substring(0, 12)) dirty=$($base.json.dirtyTrackedFiles)"
Write-Host "candidate $($cand.json.label) $($cand.json.commit.Substring(0, 12)) dirty=$($cand.json.dirtyTrackedFiles)"

$failures = 0
$volatileSeen = @{}
$keys = @($base.json.results | ForEach-Object { $_.key }) + @($cand.json.results | ForEach-Object { $_.key } | Where-Object { -not $bMap.ContainsKey($_) })
foreach ($key in $keys) {
    if (-not $bMap.ContainsKey($key)) { Write-Host "NEW       $key"; continue }
    if (-not $cMap.ContainsKey($key)) { Write-Host "MISSING   $key"; $failures++; continue }
    $b = $bMap[$key]; $c = $cMap[$key]
    $entry = $table[$key]
    $patterns = @($M7RSweepVolatileFields) + @($Ignore) + @(if ($entry) { $entry.Volatile })
    $patterns = @($patterns | Where-Object { $_ })
    $fb = Get-FlatEntry $b; $fc = Get-FlatEntry $c
    $diffs = @(); $ignored = 0; $tolerated = @()
    $paths = @($fb.Keys) + @($fc.Keys | Where-Object { -not $fb.Contains($_) })
    foreach ($p in $paths) {
        $vb = if ($fb.Contains($p)) { $fb[$p] } else { '<absent>' }
        $vc = if ($fc.Contains($p)) { $fc[$p] } else { '<absent>' }
        if ($vb -ceq $vc) { continue }
        if (Test-Volatile $p $patterns) { $ignored++; $volatileSeen[($p -replace '\[\d+\]', '[]' -replace '#\d+', '#')] = 1; continue }
        # Capture hashes of a tolerance-class entry are judged on the images instead.
        if ($entry -and $entry.Tolerance -and $p -match '^captures\[(\d+)\]\.(sha256|metadata\.image\.(sha256|signal\..*))$') {
            $tolerated += [int]$Matches[1]; continue
        }
        if ($entry -and $entry.Tolerance -and $p -match '^iridium\.IRIDIUM_CAPTURE#(\d+)\.sha256$') { $tolerated += [int]$Matches[1]; continue }
        $diffs += "$p : $vb -> $vc"
    }
    foreach ($i in ($tolerated | Sort-Object -Unique)) {
        $cb = @($b.captures)[$i]; $cc = @($c.captures)[$i]
        $limits = $M7RTolerances[$entry.Tolerance][$cb.point]
        if (-not $limits) { $diffs += "captures[$i]: no $($entry.Tolerance) envelope for point $($cb.point)"; continue }
        $ext = if ($cb.point -eq 'scene') { 'pfm' } else { 'tga' }
        $ib = Join-Path $base.dir "$key/capture/$($cb.stem).$ext"
        $ic = Join-Path $cand.dir "$key/capture/$($cc.stem).$ext"
        $report = python (Join-Path $PSScriptRoot 'Diff-Images.py') $ib $ic @limits
        if ($LASTEXITCODE -ne 0) { $diffs += "captures[$i] outside $($entry.Tolerance): $(($report | Select-Object -First 1))" }
        else { Write-Host "          $key captures[$i] within $($entry.Tolerance): $(($report | Select-Object -First 1))" }
    }
    if ($diffs.Count -eq 0) {
        Write-Host ("identical {0,-24} ({1} fields, {2} volatile differences ignored)" -f $key, $fb.Count, $ignored)
        continue
    }
    $failures++
    Write-Host ("CHANGED   {0,-24} {1} differences ({2} volatile ignored)" -f $key, $diffs.Count, $ignored)
    $shown = if ($ShowAll) { $diffs } else { $diffs | Select-Object -First 25 }
    foreach ($d in $shown) { Write-Host "          $d" }
    if (-not $ShowAll -and $diffs.Count -gt 25) { Write-Host "          ... $($diffs.Count - 25) more (-ShowAll)" }
}
if ($ListVolatile) { Write-Host 'volatile paths that differed:'; $volatileSeen.Keys | Sort-Object | ForEach-Object { Write-Host "          $_" } }
Write-Host "changed entries: $failures"
if ($failures -gt 0) { exit 1 }
