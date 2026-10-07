# M9 temporal fixture set (docs/milestones/M9-temporal-and-post.md, slice G6b).
# Dot-sourced by M9 capture, admission and metric scripts, like tools/m7r/M7RFixtures.ps1.
# Model names an entry of $M9Models cooked by Cook-TemporalModels.ps1 into out/m9/ddc;
# Args are route flags. Every fixture is engine-authored (tools/m9/Generate-TemporalAssets.py)
# and declares its own lights and constant environment, so no environment artifact is needed.
#
# The fixtures use scene_factory.kind = "composition": each composition entity draws one
# top-level glTF node of the fixture's single source asset, so one --cooked-model-artifact
# backs the whole fixture. TF-dual (editor-like view alternation) is harness-driven and is
# added by the lead.

$M9Models = [ordered]@{
    'tf-thin'       = @{ Source = 'benchmarks/m9/temporal_thin.gltf' }
    'tf-foliage'    = @{ Source = 'benchmarks/m9/temporal_foliage.gltf' }
    'tf-disocclude' = @{ Source = 'benchmarks/m9/temporal_disocclude.gltf' }
    'tf-pan'        = @{ Source = 'benchmarks/m9/temporal_pan.gltf' }
    'tf-emissive'   = @{ Source = 'benchmarks/m9/temporal_emissive.gltf' }
    'tf-glass'      = @{ Source = 'benchmarks/m9/temporal_glass.gltf' }
    'tf-specular'   = @{ Source = 'benchmarks/m9/temporal_specular.gltf' }
    'tf-static'     = @{ Source = 'benchmarks/m9/temporal_static.gltf' }
    'tf-hdr'        = @{ Source = 'benchmarks/m9/temporal_hdr.gltf' }
    'tf-teleport'   = @{ Source = 'benchmarks/m9/temporal_teleport.gltf' }
    'tf-reactive'   = @{ Source = 'benchmarks/m9/temporal_reactive.gltf' }
}
$m9Temporal = 'assets/benchmarks/m9/temporal-manifest.v1.json'

$M9TemporalSet = @(
    @{ Key = 'TF-thin';       Id = 'm9_tf_thin_v1';       Manifest = $m9Temporal; Model = 'tf-thin';       Args = @() }
    @{ Key = 'TF-foliage';    Id = 'm9_tf_foliage_v1';    Manifest = $m9Temporal; Model = 'tf-foliage';    Args = @() }
    @{ Key = 'TF-disocclude'; Id = 'm9_tf_disocclude_v1'; Manifest = $m9Temporal; Model = 'tf-disocclude'; Args = @() }
    @{ Key = 'TF-pan';        Id = 'm9_tf_pan_v1';        Manifest = $m9Temporal; Model = 'tf-pan';        Args = @() }
    @{ Key = 'TF-emissive';   Id = 'm9_tf_emissive_v1';   Manifest = $m9Temporal; Model = 'tf-emissive';   Args = @() }
    @{ Key = 'TF-glass';      Id = 'm9_tf_glass_v1';      Manifest = $m9Temporal; Model = 'tf-glass';      Args = @() }
    @{ Key = 'TF-specular';   Id = 'm9_tf_specular_v1';   Manifest = $m9Temporal; Model = 'tf-specular';   Args = @() }
    @{ Key = 'TF-static';     Id = 'm9_tf_static_v1';     Manifest = $m9Temporal; Model = 'tf-static';     Args = @() }
    @{ Key = 'TF-hdr';        Id = 'm9_tf_hdr_v1';        Manifest = $m9Temporal; Model = 'tf-hdr';        Args = @() }
    @{ Key = 'TF-teleport';   Id = 'm9_tf_teleport_v1';   Manifest = $m9Temporal; Model = 'tf-teleport';   Args = @() }
    @{ Key = 'TF-reactive';   Id = 'm9_tf_reactive_v1';   Manifest = $m9Temporal; Model = 'tf-reactive';   Args = @() }
)

function Get-M9RepoRoot { (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }

function Get-M9ModelArtifact([string] $root, [string] $model) {
    $mapPath = Join-Path $root 'out/m9/ddc/artifacts.json'
    if (-not (Test-Path $mapPath)) { throw 'Run tools/m9/Cook-TemporalModels.ps1 first.' }
    $map = Get-Content $mapPath -Raw | ConvertFrom-Json
    $entry = $map.$model
    if (-not $entry) { throw "No cooked artifact for $model; rerun Cook-TemporalModels.ps1." }
    return $entry.artifact
}
