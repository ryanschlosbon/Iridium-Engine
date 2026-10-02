# M7R R2.0 qualification sweep table (docs/milestones/M7R-R2-qualification-harness-design.md,
# section 6). Every qualification capability of the engine runs once on its fixture so the
# R2 harness move can be diffed payload by payload. Dot-sourced by
# Run-QualificationSweep.ps1 and Compare-QualificationSweep.ps1.
#
# Entry fields:
#   Key, Capability (the flag under test), Id/Manifest (benchmark fixture), Model and
#   Environment (names in artifacts.json or $M7RSweepInputs), Size (default 1280x720),
#   Transport (default sdr), Warmup, Frames, Validation ($true: --validation),
#   Profile ($false: no --profile-cpu-output), Capture (@{ Frame; Point; RequireSignal }),
#   Args (flags under test), ExpectTags (IRIDIUM_* lines that must appear),
#   RequireCounters (profile counters whose max must be nonzero), MatchCapture (key of an
#   entry whose capture hashes must be identical), Tolerance (capture tolerance class in
#   $M7RTolerances), Volatile (extra field regexes this entry may vary in), Notes.
# Generic pass rules (M7RSweepCommon.ps1): exit 0, no [Validation]: line, no fatal
# error, no payload with passed=false, no profile counter named *mismatch* nonzero.
. (Join-Path $PSScriptRoot 'M7RFixtures.ps1')
. (Join-Path $PSScriptRoot 'M7RSweepCommon.ps1')

# Sweep-only cooked inputs (frozen-set models come from artifacts.json). Sources are under
# assets/ of the data root; hdri/ content is local-only third-party and never committed.
$M7RSweepInputs = [ordered]@{
    'ordinary2-open' = @{ Source = 'benchmarks/m6/ordinary2_invalid_open_tetrahedron.gltf' }
    'hero4'          = @{ Source = 'benchmarks/m6/hero4_nested_tetrahedra.gltf' }
    'material-lab'   = @{ Source = 'benchmarks/m2/material_gpu_lab.gltf' }
    'complex-lab'    = @{ Source = 'benchmarks/m2/complex_closure_lab.gltf' }
    'belfast-env'    = @{ Source = 'hdri/belfast_sunset_puresky_4k.hdr' }   # local-only
}

$localShadow = 'assets/m7-local-shadow-device-manifest.v1.json'
$hetero = 'assets/m7-heterogeneous-shadow-admission-manifest.v1.json'
$probeLod = 'assets/m7-probe-lod-admission-manifest.v1.json'
$occlusion = 'assets/m7-occlusion-performance-manifest.v1.json'
$vsm = 'assets/m7-virtual-shadow-depth-qualification-manifest.v1.json'
$materialGpu = 'assets/benchmarks/m2/material-gpu-manifest.v1.json'
$scene = @{ Frame = 2; Point = 'scene' }
$sdr = @{ Frame = 2; Point = 'final-sdr' }
$lodRoute = @('--experimental-gpu-lod-error-pixels', '2', '--gpu-lod-max-level', '15')
$hizRoute = @('--experimental-depth-pyramid', '--experimental-depth-occlusion-rejection')
# Probe validation scene: the M7.7 compact/direct probe-capture qualification setup. Probe
# entries use 0 warm-up frames: the realtime probe captures in the first frames and then is
# cadence-deferred, so warm-up would hide the capture counters (M7.7 ran 0/1 frames).
$probeDirectSdr = @('--max-abs', '1', '--max-changed-fraction', '0.005')
$probeScene = @{ Id = 'point_shadow_contact_v1'; Manifest = $localShadow; Model = $contact; Environment = 'belfast-env' }

$M7RQualificationSweep = @(
    # --- Validators (Vulkan validation on; each reports IRIDIUM_* JSON) ---
    @{ Key = 'V01-residency-churn'; Capability = '--validate-texture-residency-churn'; Id = 'ordinary2_lit_closed_v1'; Manifest = $m6; Model = 'ordinary2'
       Warmup = 8; Frames = 4; Validation = $true; Args = @('--validate-texture-residency-churn'); ExpectTags = @('IRIDIUM_TEXTURE_RESIDENCY_CHURN') }
    ($probeScene + @{ Key = 'V02-reflection-probes'; Capability = '--validate-reflection-probes'
       Warmup = 0; Frames = 4; Validation = $true; Capture = $scene; Args = @('--validate-reflection-probes')
       RequireCounters = @('probe.capture.casters.gpu_scene', 'probe.capture.published') })
    @{ Key = 'V03-ord2-capture'; Capability = '--validate-ordinary2-capture'; Id = 'ordinary2_lit_closed_v1'; Manifest = $m6; Model = 'ordinary2'
       Warmup = 8; Frames = 4; Validation = $true; Capture = $scene; Args = @('--validate-ordinary2-capture'); ExpectTags = @('IRIDIUM_ORDINARY2_CAPTURE_VALIDATION') }
    @{ Key = 'V04-ord2-fallback'; Capability = '--validate-ordinary2-fallback'; Id = 'ordinary2_invalid_open_fallback_v1'; Manifest = $m6; Model = 'ordinary2-open'
       Warmup = 8; Frames = 4; Validation = $true; Capture = $scene; Args = @('--validate-ordinary2-fallback'); ExpectTags = @('IRIDIUM_ORDINARY2_FALLBACK_VALIDATION') }
    @{ Key = 'V05-ord2-resize'; Capability = '--validate-ordinary2-resize'; Id = 'ordinary2_lit_populated_grid_v1'; Manifest = $m6; Model = 'ordinary2'
       Warmup = 8; Frames = 8; Validation = $true; Args = @('--validate-ordinary2-resize')
       ExpectTags = @('IRIDIUM_ORDINARY2_RESIZE_EVENT', 'IRIDIUM_ORDINARY2_RESIZE_VALIDATION', 'IRIDIUM_ORDINARY2_CAPTURE_VALIDATION') }
    @{ Key = 'V06-woit-resize'; Capability = '--validate-weighted-oit-resize'; Id = 'weighted_oit_particles_v1'; Manifest = $m6; Model = 'woit'
       Warmup = 8; Frames = 8; Validation = $true; Args = @('--validate-weighted-oit-resize'); ExpectTags = @('IRIDIUM_WEIGHTED_OIT_RESIZE_EVENT', 'IRIDIUM_WEIGHTED_OIT_RESIZE_VALIDATION') }
    @{ Key = 'V07-deep-capture-hero4'; Capability = '--validate-deep-layered-capture'; Id = 'hero4_nested_tetrahedra_v1'; Manifest = $m6; Model = 'hero4'
       Warmup = 8; Frames = 4; Validation = $true; Capture = $scene; Args = @('--validate-deep-layered-capture', '--deep-layered-validation-quality', 'hero4')
       ExpectTags = @('IRIDIUM_DEEP_LAYERED_CAPTURE_VALIDATION') }
    @{ Key = 'V08-deep-capture-cine8'; Capability = '--validate-deep-layered-capture'; Id = 'cinematic8_nested_tetrahedra_v1'; Manifest = $m6; Model = 'cine8'
       Warmup = 8; Frames = 4; Validation = $true; Capture = $scene; Args = @('--validate-deep-layered-capture', '--deep-layered-validation-quality', 'cinematic8')
       ExpectTags = @('IRIDIUM_DEEP_LAYERED_CAPTURE_VALIDATION') }
    @{ Key = 'V09-deep-lifecycle'; Capability = '--validate-deep-layered-lifecycle'; Id = 'cinematic8_nested_tetrahedra_v1'; Manifest = $m6; Model = 'cine8'
       Warmup = 8; Frames = 250; Validation = $true; Args = @('--validate-deep-layered-lifecycle', '--deep-layered-validation-quality', 'cinematic8')
       ExpectTags = @('IRIDIUM_DEEP_LAYERED_LIFECYCLE_EVENT', 'IRIDIUM_DEEP_LAYERED_LIFECYCLE_VALIDATION') }
    @{ Key = 'V10-texture-table'; Capability = '--validate-texture-table-scale'; Id = 'material_gpu_lab_v1'; Manifest = $materialGpu; Model = 'material-lab'
       Warmup = 2; Frames = 4; Validation = $true; Capture = $scene; Args = @('--validate-texture-table-scale', '8192'); ExpectTags = @('IRIDIUM_TEXTURE_TABLE_SCALE') }
    @{ Key = 'V11-material-table'; Capability = '--validate-material-table-scale'; Id = 'material_gpu_lab_v1'; Manifest = $materialGpu; Model = 'material-lab'
       Warmup = 8; Frames = 4; Validation = $true; Capture = $scene; Args = @('--validate-material-table-scale', '65536'); ExpectTags = @('IRIDIUM_MATERIAL_TABLE_SCALE') }
    @{ Key = 'V12-light-table'; Capability = '--validate-light-table-scale'; Id = 'material_gpu_lab_v1'; Manifest = $materialGpu; Model = 'material-lab'
       Warmup = 8; Frames = 4; Validation = $true; Capture = $scene; Args = @('--validate-light-table-scale', '4096'); RequireCounters = @('light.capacity', 'light.active')
       # 4096 generated lights overflow the cluster lists completely (overflow_code 3, 64
       # fallback, 4032 dropped, as in M5.3); in that state the GPU directional/local
       # classification counts vary per frame and per run (1024-1027 / 2048-2052).
       Volatile = @('^profile\.counters\.cluster\.lights\.(directional|local)\.') }
    @{ Key = 'V13-cluster-stress'; Capability = '--cluster-stress-lights'; Id = 'material_gpu_lab_v1'; Manifest = $materialGpu; Model = 'material-lab'
       Warmup = 8; Frames = 4; Validation = $true; Capture = $scene; Args = @('--cluster-stress-lights', '512'); RequireCounters = @('cluster.references.published', 'cluster.lights.local') }
    @{ Key = 'V14-transport-switch'; Capability = '--validate-output-transport-switch'; Id = 'ordinary2_lit_closed_v1'; Manifest = $m6; Model = 'ordinary2'
       Warmup = 8; Frames = 12; Validation = $true; Args = @('--validate-output-transport-switch'); ExpectTags = @('IRIDIUM_OUTPUT_TRANSPORT_SWITCH') }
    @{ Key = 'V15-depth-pyramid-capture'; Capability = '--validate-depth-pyramid-capture'; Id = 'point_shadow_contact_v1'; Manifest = $localShadow; Model = $contact
       Warmup = 8; Frames = 4; Validation = $true; Args = @('--validate-depth-pyramid-capture'); ExpectTags = @('IRIDIUM_DEPTH_PYRAMID_CAPTURE_VALIDATION') }
    @{ Key = 'V16-depth-pyramid-resize'; Capability = '--validate-depth-pyramid-resize'; Id = 'point_shadow_contact_v1'; Manifest = $localShadow; Model = $contact
       Warmup = 8; Frames = 12; Validation = $true; Args = $hizRoute + @('--validate-depth-pyramid-resize'); ExpectTags = @('IRIDIUM_DEPTH_PYRAMID_RESIZE_EVENT', 'IRIDIUM_DEPTH_PYRAMID_RESIZE_VALIDATION') }

    # --- Qualification oracles (validation off, so only the flag enables the oracle) ---
    @{ Key = 'O01-shadow-oracle'; Capability = '--shadow-indirect-qualification-oracle'; Id = 'm7_heterogeneous_shadow_warm_motion_v1'; Manifest = $hetero; Model = $contact
       Warmup = 8; Frames = 8; Validation = $false; Capture = $scene; Args = @('--shadow-indirect-qualification-oracle')
       RequireCounters = @('shadow.directional.indirect.qualification_oracle', 'shadow.spot.indirect.qualification_oracle', 'shadow.point.indirect.qualification_oracle') }
    @{ Key = 'O02-gpu-lod-oracle'; Capability = '--gpu-lod-qualification-oracle'; Id = 'm7_three_dense_near_mid_far_v1'; Manifest = $threeDense; Model = 'alfa-lod'
       Warmup = 8; Frames = 8; Validation = $false; Capture = $scene; Args = $lodRoute + @('--gpu-lod-qualification-oracle')
       RequireCounters = @('gpu_scene.lod.oracle_triangles') }
    @{ Key = 'O03-probe-lod-oracle'; Capability = '--probe-lod-qualification-oracle'; Id = 'm7_probe_lod_reflection_motion_v1'; Manifest = $probeLod; Model = 'alfa-lod'; Environment = 'belfast-env'
       Warmup = 0; Frames = 8; Validation = $false; Capture = $scene; Args = @('--validate-reflection-probes', '--experimental-probe-lod-error-pixels', '8', '--probe-lod-qualification-oracle')
       Notes = 'The fixture probe (reflection_probe_capture) is only instantiated by --validate-reflection-probes, which needs an environment artifact.'
       RequireCounters = @('probe.capture.indirect.qualification_oracle', 'probe.capture.lod.oracle_triangles') }
    @{ Key = 'O04-occlusion-oracle'; Capability = '--depth-occlusion-qualification-oracle'; Id = 'm7_occlusion_dense_depth_stack_v1'; Manifest = $occlusion; Model = $alfa
       Warmup = 8; Frames = 8; Validation = $false; Capture = $scene; Args = $hizRoute + @('--depth-occlusion-qualification-oracle')
       RequireCounters = @('depth.occlusion.gpu_scene.tested') }
    @{ Key = 'O05-vsm-depth-oracle'; Capability = '--virtual-shadow-depth-qualification-oracle'; Id = 'm7_virtual_shadow_complex_v1'; Manifest = $vsm; Model = 'complex-lab'
       Warmup = 0; Frames = 6; Validation = $false; Capture = $scene; Args = @('--experimental-virtual-shadow-resources', '--virtual-shadow-depth-qualification-oracle')
       RequireCounters = @('shadow.virtual.oracle.validated', 'shadow.virtual.oracle.compared_requests') }

    # --- Reference routes: must reproduce the automatic route's capture exactly ---
    # Probe scene (M7.7): the scene-linear PFM of any direct probe-capture route differs
    # from the GPU-scene capture by about 1 R16 step (recorded in M7.7), so these compare
    # final-SDR, which M7.7 (independent probe visibility) recorded byte-identical. At R2.0 the
    # direct probe-capture routes (R01, R03) differ by at most 1 code on about 0.3% of pixels,
    # so they use $probeDirectSdr (the WeightedOIT final-SDR envelope); R02 must be identical.
    ($probeScene + @{ Key = 'R00-auto-probe-sdr'; Capability = 'automatic (reference for R01-R03)'; Warmup = 0; Frames = 4; Validation = $true; Capture = $sdr
       Args = @('--validate-reflection-probes') })
    ($probeScene + @{ Key = 'R01-ref-direct-gbuffer'; Capability = '--reference-direct-gbuffer'; Warmup = 0; Frames = 4; Validation = $true; Capture = $sdr
       Args = @('--validate-reflection-probes', '--reference-direct-gbuffer'); MatchCapture = 'R00-auto-probe-sdr'; MatchTolerance = $probeDirectSdr; RequireCounters = @('probe.capture.casters.direct_fallback') })
    ($probeScene + @{ Key = 'R02-ref-direct-shadows'; Capability = '--reference-direct-shadows'; Warmup = 0; Frames = 4; Validation = $true; Capture = $sdr
       Args = @('--validate-reflection-probes', '--reference-direct-shadows'); MatchCapture = 'R00-auto-probe-sdr' })
    ($probeScene + @{ Key = 'R03-ref-direct-probe'; Capability = '--reference-direct-probe-capture'; Warmup = 0; Frames = 4; Validation = $true; Capture = $sdr
       Args = @('--validate-reflection-probes', '--reference-direct-probe-capture'); MatchCapture = 'R00-auto-probe-sdr'; MatchTolerance = $probeDirectSdr; RequireCounters = @('probe.capture.casters.direct_fallback') })
    # Probe-free dense scene (M7 lit requalification): scene-linear PFM byte-identical.
    @{ Key = 'R04-auto-dense-scene'; Capability = 'automatic (reference for R05)'; Id = 'm7_three_dense_all_visible_v1'; Manifest = $threeDense; Model = $alfa
       Warmup = 12; Frames = 6; Validation = $true; Capture = @{ Frame = 4; Point = 'scene' } }
    @{ Key = 'R05-ref-direct-gbuffer-dense'; Capability = '--reference-direct-gbuffer'; Id = 'm7_three_dense_all_visible_v1'; Manifest = $threeDense; Model = $alfa
       Warmup = 12; Frames = 6; Validation = $true; Capture = @{ Frame = 4; Point = 'scene' }; Args = @('--reference-direct-gbuffer'); MatchCapture = 'R04-auto-dense-scene' }

    # --- Other qualification controls ---
    @{ Key = 'X01-lod-resident-floor'; Capability = '--gpu-lod-minimum-resident-level'; Id = 'm7_three_dense_near_mid_far_v1'; Manifest = $threeDense; Model = 'alfa-lod'
       Warmup = 8; Frames = 8; Validation = $true; Capture = $scene; Args = @('--experimental-gpu-lod-error-pixels', '2', '--gpu-lod-max-level', '0', '--gpu-lod-minimum-resident-level', '1')
       ExpectTags = @('IRIDIUM_LOD_PHYSICAL_FALLBACK') }
    @{ Key = 'X02-woit-order-seed'; Capability = '--weighted-oit-order-seed'; Id = 'weighted_oit_particles_v1'; Manifest = $m6; Model = 'woit'
       Warmup = 8; Frames = 4; Validation = $true; Capture = $scene; Args = @('--weighted-oit-order-seed', '7'); Tolerance = 'woit-order' }
    @{ Key = 'X03-select-entity'; Capability = '--select-benchmark-entity'; Id = 'ordinary2_lit_closed_v1'; Manifest = $m6; Model = 'ordinary2'
       Warmup = 8; Frames = 4; Validation = $true; Capture = @{ Frame = 2; Point = 'final-sdr' }; Args = @('--select-benchmark-entity') }
    @{ Key = 'X04-wireframe'; Capability = '--wireframe'; Id = 'point_shadow_contact_v1'; Manifest = $localShadow; Model = $contact
       Warmup = 8; Frames = 4; Validation = $true; Capture = @{ Frame = 2; Point = 'final-sdr' }; Args = @('--wireframe') }
    @{ Key = 'X05-no-local-shadows'; Capability = '--benchmark-disable-local-shadows'; Id = 'point_shadow_contact_v1'; Manifest = $localShadow; Model = $contact
       Warmup = 8; Frames = 4; Validation = $true; Capture = $scene; Args = @('--benchmark-disable-local-shadows') }
    @{ Key = 'X06-profile-output'; Capability = '--profile-cpu-output'; Id = 'ordinary2_lit_closed_v1'; Manifest = $m6; Model = 'ordinary2'
       Warmup = 8; Frames = 16; Validation = $false; Args = @('--profile-gpu', '--cache-state', 'warm-steady-state')
       Notes = 'Every entry except C01-C03 also writes a profile; this one adds GPU timestamps and a cache state.' }

    # --- Capture points (no profiling: the frozen-set shape) ---
    @{ Key = 'C01-capture-scene'; Capability = '--capture-point scene'; Id = 'ordinary2_lit_closed_v1'; Manifest = $m6; Model = 'ordinary2'
       Warmup = 8; Frames = 4; Validation = $true; Profile = $false; Capture = @{ Frame = 2; Point = 'scene' } }
    @{ Key = 'C02-capture-final-sdr'; Capability = '--capture-point final-sdr'; Id = 'ordinary2_lit_closed_v1'; Manifest = $m6; Model = 'ordinary2'
       Warmup = 8; Frames = 4; Validation = $true; Profile = $false; Capture = @{ Frame = 2; Point = 'final-sdr' } }
    @{ Key = 'C03-capture-final-output'; Capability = '--capture-point final-output'; Id = 'ordinary2_lit_closed_v1'; Manifest = $m6; Model = 'ordinary2'
       Warmup = 8; Frames = 4; Validation = $true; Profile = $false; Capture = @{ Frame = 2; Point = 'final-output' } }
)

# Field-path regexes (flattened paths, see Compare-QualificationSweep.ps1) that differ
# between two runs of identical code. Never add behavioral fields (counts, passed,
# mismatch counters) here.
$M7RSweepVolatileFields = @(
    # Wall-clock and durations: IRIDIUM_* *_ns fields (prewarm, run metrics, transport
    # switch), startup *_nanoseconds, the per-entry elapsed time.
    '_ns$', '_ms$', '_nanoseconds$', '^seconds$'
    # Absolute capture paths in IRIDIUM_CAPTURE embed the sweep label (the hash is kept).
    '^iridium\.IRIDIUM_CAPTURE#\d+\.(image|metadata)$'
    # Run identity, source and per-process ids in the capture sidecar / profile header.
    '\.source\.(commit|branch|dirty_at_configure)$', '\.run_id$', 'process_id$'
    '\.(model_input|environment_input)\.location$'
)

