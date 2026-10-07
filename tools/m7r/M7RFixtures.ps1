# M7R frozen fixture set (docs/milestones/M7R-architecture-consolidation.md, R0).
# Dot-sourced by the M7R capture and timing scripts. Model names an entry of
# $M7RModels cooked by Cook-FrozenModels.ps1; Args are route flags. Every capture is
# native 3840x2160, SDR transport, ACES 2, 0 EV, with the fixture's declared constant
# environment (no HDRI artifact).

# Cookable models: Source is relative to the asset roots (the checkout's assets/, else
# the local asset library; Resolve-IridiumAssetPath); Metadata overrides the sidecar (the
# LOD variant is the M7.5 topology-transactional settings, kept under out/m7r/meta).
$M7RModels = [ordered]@{
    'alfa'      = @{ Source = 'models/alfa_romeo/alfa_romeo.gltf' }   # local-only third-party content
    'alfa-lod'  = @{ Source = 'models/alfa_romeo/alfa_romeo.gltf'; Metadata = 'out/m7r/meta/alfa_romeo.lod.iridium.meta' }
    'contact'   = @{ Source = 'benchmarks/m5/directional-shadow-contact.gltf' }
    'ordinary2' = @{ Source = 'benchmarks/m6/ordinary2_closed_tetrahedron.gltf' }
    'cine8'     = @{ Source = 'benchmarks/m6/cinematic8_nested_tetrahedra.gltf' }
    'woit'      = @{ Source = 'benchmarks/m6/weighted_oit_particles.gltf' }
    # M7 completion owner cases (local-only third-party content; sidecars beside the
    # sources). The 930 is the owner's editor import (porsche911930t, with its material
    # overrides); models/free_1975_porsche_911_930_turbo has no sidecar and cannot cook.
    'porsche911' = @{ Source = 'models/porsche_911_carrera4s/porsche_911_carrera4s.gltf' }
    'porsche930' = @{ Source = 'porsche911930t/porsche911930t.gltf' }
    # Cooked environment (IBL) for the probe-capture fixture; local-only third-party HDRI.
    'belfast-env' = @{ Source = 'hdri/belfast_sunset_puresky_4k.hdr' }
}
$alfa = 'alfa'
$contact = 'contact'
$threeDense = 'assets/m7-three-dense-assets-manifest.v1.json'
$m6 = 'assets/benchmarks/m6/ordinary2-runtime-manifest.v1.json'

$M7RFrozenSet = @(
    @{ Key = 'F1-all';    Id = 'm7_three_dense_all_visible_v1';          Manifest = $threeDense; Model = $alfa; Args = @() }
    @{ Key = 'F2-one';    Id = 'm7_three_dense_one_visible_v1';          Manifest = $threeDense; Model = $alfa; Args = @() }
    @{ Key = 'F3-stress'; Id = 'm7_many_instance_stress_v1';             Manifest = $threeDense; Model = $alfa; Args = @(); Tolerance = 'depth-tie' }
    @{ Key = 'F4-ord2';   Id = 'ordinary2_lit_closed_v1';                Manifest = $m6; Model = 'ordinary2'; Args = @() }
    @{ Key = 'F4-cine8';  Id = 'cinematic8_nested_tetrahedra_v1';        Manifest = $m6; Model = 'cine8'; Args = @() }
    @{ Key = 'F4-woit';   Id = 'weighted_oit_particles_v1';              Manifest = $m6; Model = 'woit'; Args = @(); Tolerance = 'woit-order' }
    @{ Key = 'F5-hetero'; Id = 'm7_heterogeneous_shadow_warm_motion_v1'; Manifest = 'assets/m7-heterogeneous-shadow-admission-manifest.v1.json'; Model = $contact; Args = @() }
    @{ Key = 'F5-point';  Id = 'point_shadow_contact_v1';                Manifest = 'assets/m7-local-shadow-device-manifest.v1.json'; Model = $contact; Args = @() }
    @{ Key = 'F6-probe';  Id = 'm7_probe_lod_reflection_motion_v1';      Manifest = 'assets/m7-probe-lod-admission-manifest.v1.json'; Model = $alfa; Args = @() }
    # F6 alone does not instantiate its probe (that needs --validate-reflection-probes and an
    # environment artifact); F6-probecap covers live reflection-probe capture (added in R2).
    @{ Key = 'F6-probecap'; Id = 'm7_probe_lod_reflection_motion_v1'; Manifest = 'assets/m7-probe-lod-admission-manifest.v1.json'; Model = $alfa; Environment = 'belfast-env'; Args = @('--validate-reflection-probes') }
    @{ Key = 'F7-hiz';    Id = 'm7_occlusion_dense_depth_stack_v1';      Manifest = 'assets/m7-occlusion-performance-manifest.v1.json'; Model = $alfa; Args = @('--experimental-depth-pyramid', '--experimental-depth-occlusion-rejection') }
    @{ Key = 'F7-lod';    Id = 'm7_many_instance_stress_v1';             Manifest = $threeDense; Model = 'alfa-lod'; Args = @('--experimental-gpu-lod-error-pixels', '2', '--gpu-lod-max-level', '15'); Tolerance = 'depth-tie' }
)

# Run-to-run variation measured at R0 (see the plan's R0 evidence). 'exact' fixtures
# must be byte-identical; the others pass when within these envelopes against the
# baseline. depth-tie: a few depth-equal pixels resolve differently because GPU
# compaction order varies with 256 instances. woit-order: WeightedOIT accumulation
# order varies; limits are the frozen M6.7 draw-order thresholds.
$M7RTolerances = @{
    'depth-tie'  = @{ scene = @('--max-changed-pixels', '64'); 'final-sdr' = @('--max-changed-pixels', '64') }
    'woit-order' = @{ scene = @('--max-abs', '0.0625', '--max-rmse', '0.002', '--max-rel', '0.005'); 'final-sdr' = @('--max-abs', '1', '--max-changed-fraction', '0.005') }
}

# Timing routes: F1 is GPU-representative; the dense depth stack is CPU-heavy.
# M7R R5c.8 (owner decision 5): a lit route (the F5-hetero fixture: directional,
# spot and point shadows with motion) and a probe route (the F6-probecap fixture:
# live reflection-probe capture) join the timing set, so their steady-frame
# allocations are checked with every pair.
$M7RTimingRoutes = @(
    @{ Key = 'T-F1-all';   Id = 'm7_three_dense_all_visible_v1';     Manifest = $threeDense; Model = $alfa; Args = @() }
    @{ Key = 'T-F7-stack'; Id = 'm7_occlusion_dense_depth_stack_v1'; Manifest = 'assets/m7-occlusion-performance-manifest.v1.json'; Model = $alfa; Args = @() }
    @{ Key = 'T-F5-hetero'; Id = 'm7_heterogeneous_shadow_warm_motion_v1'; Manifest = 'assets/m7-heterogeneous-shadow-admission-manifest.v1.json'; Model = $contact; Args = @() }
    @{ Key = 'T-F6-probecap'; Id = 'm7_probe_lod_reflection_motion_v1'; Manifest = 'assets/m7-probe-lod-admission-manifest.v1.json'; Model = $alfa; Environment = 'belfast-env'; Args = @('--validate-reflection-probes') }
)

# --- M7 completion owner cases (docs/milestones/M7-completion.md, M7.P1) -------------
# Not part of the frozen set or the default timing routes. Model names one cooked model;
# Models names several (a multi-model composition fixture): the first is the startup
# model (--cooked-model-artifact), the others are passed as --benchmark-model-artifact
# and the engine matches every artifact to its fixture source by asset identity.
# Captures: Run-OwnerCaseCaptures.ps1. Timing: $M7CTimingRoutes (T-<Key>), selected by
# name with Run-TimingPair.ps1 -Only or Run-FeatureAdmission.ps1 -Routes. The product
# route adds -ExtraArgs "--anti-aliasing taa --exposure auto --bloom on".
$m7c = 'assets/m7c-owner-cases-manifest.v1.json'
$M7COwnerCases = @(
    # PC1 many objects: the Carrera as 1, 4 and 16 instances, each all visible, about
    # half off-frustum and all off-frustum (camera turned away, cars inside the far
    # plane); the Alfa as 16; the existing 256-instance stress fixture for reference.
    foreach ($n in @(1, 4, 16)) {
        foreach ($state in @(@('all', 'all_visible'), @('half', 'half_off'), @('off', 'all_off'))) {
            # All off: the capture is the constant environment by design (ConstantImage).
            @{ Key = "PC1-911-n$n-$($state[0])"; Id = "m7c_pc1_911_n$($n)_$($state[1])_v1"; Manifest = $m7c; Model = 'porsche911'; Args = @(); ConstantImage = ($state[0] -eq 'off') }
        }
    }
    @{ Key = 'PC1-alfa-n16-all'; Id = 'm7c_pc1_alfa_n16_all_visible_v1'; Manifest = $m7c; Model = $alfa; Args = @() }
    @{ Key = 'PC1-stress256'; Id = 'm7_many_instance_stress_v1'; Manifest = $threeDense; Model = $alfa; Args = @() }
    # PC2 close to glass: the Carrera with a directional and a shadowed point light; the
    # camera puts glass at about 5, 25, 60 and 100% of the screen.
    foreach ($glass in @('far', 'mid', 'near', 'fill')) {
        @{ Key = "PC2-glass-$glass"; Id = "m7c_pc2_glass_$($glass)_v1"; Manifest = $m7c; Model = 'porsche911'; Args = @() }
    }
    # PC3 point-light cost: the 930 and the Carrera side by side, camera between them.
    foreach ($variant in @('nolight', 'point_r5', 'point_r10', 'point_r20', 'point_r40',
            'point_r10_unshadowed', 'point_r10_ultra', 'point_r10_1e3cd', 'point_r10_1e6cd', 'spot_r10')) {
        @{ Key = "PC3-$($variant -replace '_', '-')"; Id = "m7c_pc3_$($variant)_v1"; Manifest = $m7c; Models = @('porsche930', 'porsche911'); Args = @() }
    }
    # PC4 local-shadow receiver bias (M7.10.6): the Carrera's door lit at grazing
    # incidence by a shadowed point or spot light (High and Ultra), and a contact check
    # (the Carrera on a ground plane with a 1 cm triangle blocker beside it, overhead
    # point light) where over-biasing would leak light.
    foreach ($variant in @('graze_high', 'graze_ultra', 'graze_spot_high', 'graze_spot_ultra')) {
        @{ Key = "PC4-$($variant -replace '_', '-')"; Id = "m7c_pc4_$($variant)_v1"; Manifest = $m7c; Model = 'porsche911'; Args = @() }
    }
    foreach ($quality in @('high', 'ultra')) {
        @{ Key = "PC4-contact-$quality"; Id = "m7c_pc4_contact_$($quality)_v1"; Manifest = $m7c; Models = @('porsche911', 'contact'); Args = @() }
    }
)
$M7CTimingRoutes = @($M7COwnerCases | ForEach-Object {
    $route = $_.Clone()
    $route.Key = "T-$($_.Key)"
    $route
})

# Timing routes by key: every M7R and M7C route; with no keys, the M7R routes (the
# owner-case routes run only when named).
function Select-M7RTimingRoutes([string[]] $keys) {
    if (-not $keys -or $keys.Count -eq 0) { return @($M7RTimingRoutes) }
    return @(@($M7RTimingRoutes) + @($M7CTimingRoutes) | Where-Object { $keys -contains $_.Key })
}

function Get-M7RRepoRoot { (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }

# --- Local asset library ---------------------------------------------------------------
# Licensed third-party content (the Alfa, HDRIs, scenes) lives in a local asset root
# outside the repository and is read in place; never copy or link it into a checkout.
# The configuration matches the engine's (src/core/ProjectAssetRoots.h): the
# IRIDIUM_LOCAL_ASSET_ROOT environment variable, else <checkout>/iridium.local.json
# ({ "localAssetRoot": "D:/IridiumAssets" }), else, in a git worktree, the main
# checkout's iridium.local.json.

# The main checkout of a linked git worktree (<checkout>/.git is a "gitdir:" file), or
# $null for a main checkout.
function Get-IridiumMainCheckout([string] $checkout) {
    $dotGit = Join-Path $checkout '.git'
    if (-not (Test-Path $dotGit -PathType Leaf)) { return $null }
    $line = [string](Get-Content $dotGit -TotalCount 1)
    if ($line -notmatch '^gitdir:\s*(.+?)\s*$') { return $null }
    $gitDir = $Matches[1]
    if (-not [IO.Path]::IsPathRooted($gitDir)) { $gitDir = Join-Path $checkout $gitDir }
    $commonFile = Join-Path $gitDir 'commondir'
    if (Test-Path $commonFile -PathType Leaf) {
        $common = ([string](Get-Content $commonFile -TotalCount 1)).Trim()
        if (-not [IO.Path]::IsPathRooted($common)) { $common = Join-Path $gitDir $common }
    }
    elseif ((Split-Path (Split-Path $gitDir) -Leaf) -eq 'worktrees') { $common = Split-Path (Split-Path $gitDir) }
    else { return $null }
    $common = [IO.Path]::GetFullPath($common).TrimEnd('\', '/')
    if ((Split-Path $common -Leaf) -ne '.git') { return $null }
    return (Split-Path $common)
}

# The local asset root directory, or $null when none is configured or it is missing.
function Get-IridiumLocalAssetRoot([string] $checkout = (Get-M7RRepoRoot)) {
    $configured = $null
    if ($env:IRIDIUM_LOCAL_ASSET_ROOT -and $env:IRIDIUM_LOCAL_ASSET_ROOT.Trim()) {
        $value = $env:IRIDIUM_LOCAL_ASSET_ROOT.Trim()
        $configured = if ([IO.Path]::IsPathRooted($value)) { $value } else { Join-Path $checkout $value }
    }
    else {
        $files = @(Join-Path $checkout 'iridium.local.json')
        $main = Get-IridiumMainCheckout $checkout
        if ($main) { $files += Join-Path $main 'iridium.local.json' }
        foreach ($file in $files) {
            if (-not (Test-Path $file -PathType Leaf)) { continue }
            # The first file present decides, even when it names no root.
            $json = Get-Content $file -Raw | ConvertFrom-Json
            $value = [string]$json.localAssetRoot
            if ($value.Trim()) {
                $value = $value.Trim()
                $configured = if ([IO.Path]::IsPathRooted($value)) { $value } else { Join-Path (Split-Path $file) $value }
            }
            break
        }
    }
    if (-not $configured) { return $null }
    $configured = [IO.Path]::GetFullPath($configured)
    if (-not (Test-Path $configured -PathType Container)) { return $null }
    return $configured
}

# A root-relative asset path (e.g. models/alfa_romeo/alfa_romeo.gltf): the checkout's
# assets/ copy when present, else the local asset library's, else the assets/ path (so
# a missing-file error names it as before).
function Resolve-IridiumAssetPath([string] $checkout, [string] $relative) {
    $project = [IO.Path]::GetFullPath((Join-Path (Join-Path $checkout 'assets') $relative))
    if (Test-Path $project) { return $project }
    $local = Get-IridiumLocalAssetRoot $checkout
    if ($local) {
        $candidate = [IO.Path]::GetFullPath((Join-Path $local $relative))
        if (Test-Path $candidate) { return $candidate }
    }
    return $project
}

# The checkout whose out/m7r/ddc (cooked frozen set) a run uses: this one when it has
# cooked the set, else the main checkout (a worktree reuses the main checkout's DDC).
function Get-M7RArtifactRoot([string] $checkout) {
    if (Test-Path (Join-Path $checkout 'out/m7r/ddc/artifacts.json')) { return $checkout }
    $main = Get-IridiumMainCheckout $checkout
    if ($main -and (Test-Path (Join-Path $main 'out/m7r/ddc/artifacts.json'))) { return $main }
    return $checkout
}

# Persisted pipeline cache (M7R R4c.4, --pipeline-cache DIR|off). Every M7R run script
# takes -PipelineCache and defaults to 'off': no VkPipelineCache, exactly the pipeline
# creation of builds before R4c.4, and independent of what earlier runs left on disk.
#   captures, digests, sweeps  'off': evidence never depends on cache state.
#   timing pairs, hitch runs   'off': both sides create pipelines the same way (a
#                              pre-R4c.4 baseline has no cache at all); pipeline
#                              creation in mid-run events stays comparable.
# Pass a directory (e.g. out/m7r/pipeline-cache/<label>) for an explicitly warm run; it
# is made absolute, because an engine resolves relative paths against its own working
# directory. Executables built before R4c.4 lack the flag and get no argument.
$M7RPipelineCacheSupport = @{}
function Get-M7RPipelineCacheArgs([string] $exe, [string] $mode) {
    if (-not $mode) { return @() }
    if (-not $M7RPipelineCacheSupport.ContainsKey($exe)) {
        $usage = (cmd /c "`"$exe`" --help 2>&1") -join "`n"
        $M7RPipelineCacheSupport[$exe] = [bool]($usage -match '--pipeline-cache')
    }
    if (-not $M7RPipelineCacheSupport[$exe]) { return @() }
    if ($mode -eq 'off') { return @('--pipeline-cache', 'off') }
    return @('--pipeline-cache', $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($mode))
}

# M9.7: the product defaults are TAA, Auto exposure and bloom. Measurement routes (frozen
# set, timing, sweeps, digests, admission baselines) pin the M7R route explicitly so
# their results stay comparable: TAA off, Manual exposure, bloom off. A tool or side that
# wants a feature passes it afterwards (a later option wins). Executables that predate a
# flag do not receive it (they cannot run the feature anyway).
$M7RRouteSupport = @{}
function Get-M7RRouteArgs([string] $exe) {
    if (-not $M7RRouteSupport.ContainsKey($exe)) {
        $usage = (cmd /c "`"$exe`" --help 2>&1") -join "`n"
        $M7RRouteSupport[$exe] = @(
            $(if ($usage -match '--anti-aliasing') { '--anti-aliasing'; 'none' })
            $(if ($usage -match '--exposure ') { '--exposure'; 'manual' })
            $(if ($usage -match '--bloom ') { '--bloom'; 'off' }))
    }
    return $M7RRouteSupport[$exe]
}

# The pinned route plus the pipeline-cache arguments: the base of every measurement run.
function Get-M7REngineBaseArgs([string] $exe, [string] $mode) {
    return @(Get-M7RRouteArgs $exe) + @(Get-M7RPipelineCacheArgs $exe $mode)
}

# Runs the engine with stdout and stderr merged into $log (Windows PowerShell 5.1
# would otherwise turn native stderr into terminating errors). Returns the exit code.
function Invoke-M7REngine([string] $exe, [string[]] $arguments, [string] $log) {
    $quoted = $arguments | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
    cmd /c "`"$exe`" $($quoted -join ' ') > `"$log`" 2>&1"
    return $LASTEXITCODE
}

function Get-M7RModelArtifact([string] $root, [string] $model) {
    $mapPath = Join-Path $root 'out/m7r/ddc/artifacts.json'
    if (-not (Test-Path $mapPath)) { throw 'Run tools/m7r/Cook-FrozenModels.ps1 first.' }
    $map = Get-Content $mapPath -Raw | ConvertFrom-Json
    $entry = $map.$model
    if (-not $entry) { throw "No cooked artifact for $model; rerun Cook-FrozenModels.ps1." }
    return $entry.artifact
}

# The cooked-model arguments of a fixture or route (absolute paths under $root): its
# Model as --cooked-model-artifact, or for Models the first so and each further one as
# --benchmark-model-artifact (the engine matches them to sources by asset identity).
function Get-M7RModelArtifactArgs([string] $root, $entry) {
    $models = @(if ($entry.Models) { $entry.Models } else { $entry.Model })
    $arguments = @()
    for ($i = 0; $i -lt $models.Count; $i++) {
        $flag = if ($i -eq 0) { '--cooked-model-artifact' } else { '--benchmark-model-artifact' }
        $arguments += @($flag, (Join-Path $root (Get-M7RModelArtifact $root $models[$i])))
    }
    return $arguments
}
