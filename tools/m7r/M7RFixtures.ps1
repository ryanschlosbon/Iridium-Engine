# M7R frozen fixture set (docs/milestones/M7R-architecture-consolidation.md, R0).
# Dot-sourced by the M7R capture and timing scripts. Model names an entry of
# $M7RModels cooked by Cook-FrozenModels.ps1; Args are route flags. Every capture is
# native 3840x2160, SDR transport, ACES 2, 0 EV, with the fixture's declared constant
# environment (no HDRI artifact).

# Cookable models: Source is under assets/; Metadata overrides the sidecar (the LOD
# variant is the M7.5 topology-transactional settings, kept under out/m7r/meta).
$M7RModels = [ordered]@{
    'alfa'      = @{ Source = 'models/alfa_romeo/alfa_romeo.gltf' }   # local-only third-party content
    'alfa-lod'  = @{ Source = 'models/alfa_romeo/alfa_romeo.gltf'; Metadata = 'out/m7r/meta/alfa_romeo.lod.iridium.meta' }
    'contact'   = @{ Source = 'benchmarks/m5/directional-shadow-contact.gltf' }
    'ordinary2' = @{ Source = 'benchmarks/m6/ordinary2_closed_tetrahedron.gltf' }
    'cine8'     = @{ Source = 'benchmarks/m6/cinematic8_nested_tetrahedra.gltf' }
    'woit'      = @{ Source = 'benchmarks/m6/weighted_oit_particles.gltf' }
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

function Get-M7RRepoRoot { (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }

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
