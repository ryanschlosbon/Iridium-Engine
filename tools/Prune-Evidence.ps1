<#
.SYNOPSIS
Prunes raw qualification evidence under out/ while keeping its durable summaries.

.DESCRIPTION
Evidence retention policy (owner decision, 2026-10-07): raw images, profile JSONL,
logs and references live only while their slice is active. When a slice is accepted
its evidence is pruned to summaries: every *.md and *.txt, and every *.json up to
-SummaryMaxBytes (summary.json, runs.json, machine-state.json, hashes.json, capture
sidecars). Accepted conclusions live in docs/ under git.

-Summarize prunes directories to their summaries; -Remove deletes directories or
files entirely. Without -Execute the script only reports what it would delete.

Safety:
- every target must resolve inside <repo>/out;
- the script refuses to run if any target is, or contains, a junction or symbolic
  link, so a delete can never reach outside the target (for example the local asset
  library);
- protected paths (frozen baselines, caches, builds, tools) are never deleted, even
  when they lie inside a target. Add slice-active labels with -Protect.

.EXAMPLE
tools/Prune-Evidence.ps1 -Summarize out/m7r/captures/* -Protect out/m7r/captures/m7c-p1-fixtures
tools/Prune-Evidence.ps1 -Summarize out/m7r/captures/* -Execute
#>
param(
    [string[]] $Summarize = @(),
    [string[]] $Remove = @(),
    [string[]] $Protect = @(),
    [long] $SummaryMaxBytes = 262144,
    [switch] $Execute
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outRoot = [IO.Path]::GetFullPath((Join-Path $repo 'out'))

# Standing protection: the frozen baseline, caches, builds and tool binaries.
$standing = @(
    'out/build', 'out/editor', 'out/tools', 'out/ddc',
    'out/m7r/ddc', 'out/m7r/pipeline-cache', 'out/m7r/digests', 'out/m7r/meta',
    'out/m7r/build-times', 'out/m7r/worktrees', 'out/m9/ddc',
    'out/m7r/captures/m9-g8'
)

function Get-FullPath([string] $path) {
    if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $repo $path }
    return [IO.Path]::GetFullPath($path).TrimEnd('\')
}

function Test-Within([string] $path, [string] $root) {
    return $path -eq $root -or $path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)
}

$protected = @(($standing + $Protect) | ForEach-Object { Get-FullPath $_ })

function Expand-Targets([string[]] $patterns) {
    $result = @()
    foreach ($pattern in $patterns) {
        $full = if ([IO.Path]::IsPathRooted($pattern)) { $pattern } else { Join-Path $repo $pattern }
        $items = @(Get-Item -Path $full -Force -ErrorAction SilentlyContinue)
        if ($items.Count -eq 0) { Write-Warning "No match: $pattern"; continue }
        foreach ($item in $items) {
            $path = $item.FullName.TrimEnd('\')
            if (-not (Test-Within $path $outRoot) -or $path -eq $outRoot) {
                throw "Refusing a target outside out/ (or out/ itself): $path"
            }
            $result += $path
        }
    }
    return $result
}

$summarizeTargets = @(Expand-Targets $Summarize)
$removeTargets = @(Expand-Targets $Remove)

# Refuse links anywhere in a target before touching anything.
foreach ($target in $summarizeTargets + $removeTargets) {
    $item = Get-Item -LiteralPath $target -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing: target is a junction or symbolic link: $target"
    }
    if ($item.PSIsContainer) {
        $link = Get-ChildItem -LiteralPath $target -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($link) { throw "Refusing: $target contains a link: $($link.FullName)" }
    }
}

function Test-Protected([string] $path) {
    foreach ($p in $protected) {
        if ((Test-Within $path $p) -or (Test-Within $p $path)) { return $true }
    }
    return $false
}

function Test-Summary([IO.FileInfo] $file) {
    switch ($file.Extension.ToLowerInvariant()) {
        '.md' { return $true }
        '.txt' { return $true }
        '.json' { return $file.Length -le $SummaryMaxBytes }
        default { return $false }
    }
}

$doomed = New-Object System.Collections.Generic.List[IO.FileInfo]
$skippedProtected = New-Object System.Collections.Generic.List[string]

foreach ($target in $summarizeTargets) {
    if (@($protected | Where-Object { Test-Within $target $_ }).Count -gt 0) {
        $skippedProtected.Add($target); continue
    }
    $item = Get-Item -LiteralPath $target -Force
    $files = if ($item.PSIsContainer) { Get-ChildItem -LiteralPath $target -Recurse -File -Force } else { @($item) }
    foreach ($file in $files) {
        if (Test-Protected $file.FullName) { continue }
        if (-not (Test-Summary $file)) { $doomed.Add($file) }
    }
}
foreach ($target in $removeTargets) {
    if (Test-Protected $target) {
        # A protected path lies inside: remove everything else in the target.
        $item = Get-Item -LiteralPath $target -Force
        if (-not $item.PSIsContainer) { $skippedProtected.Add($target); continue }
        foreach ($file in Get-ChildItem -LiteralPath $target -Recurse -File -Force) {
            if (-not (Test-Protected $file.FullName)) { $doomed.Add($file) }
        }
        $skippedProtected.Add("$target (partially: protected content kept)")
        continue
    }
    $item = Get-Item -LiteralPath $target -Force
    if ($item.PSIsContainer) {
        foreach ($file in Get-ChildItem -LiteralPath $target -Recurse -File -Force) { $doomed.Add($file) }
    }
    else { $doomed.Add($item) }
}

$bytes = ($doomed | Measure-Object -Property Length -Sum).Sum
if (-not $bytes) { $bytes = 0 }
Write-Host ("{0} files, {1:n2} GB {2}" -f $doomed.Count, ($bytes / 1GB),
    $(if ($Execute) { 'deleting' } else { 'would be deleted (dry run; pass -Execute)' }))
foreach ($s in $skippedProtected) { Write-Host "protected, kept: $s" }

if ($Execute) {
    foreach ($file in $doomed) { Remove-Item -LiteralPath $file.FullName -Force }
    # Remove directories left empty, deepest first; Remove targets go entirely.
    foreach ($target in $summarizeTargets + $removeTargets) {
        if (-not (Test-Path -LiteralPath $target)) { continue }
        if (-not (Get-Item -LiteralPath $target -Force).PSIsContainer) { continue }
        Get-ChildItem -LiteralPath $target -Recurse -Directory -Force |
            Sort-Object { $_.FullName.Length } -Descending |
            Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1) } |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
        if (-not (Get-ChildItem -LiteralPath $target -Force | Select-Object -First 1)) {
            Remove-Item -LiteralPath $target -Force
        }
    }
    Write-Host 'done'
}
