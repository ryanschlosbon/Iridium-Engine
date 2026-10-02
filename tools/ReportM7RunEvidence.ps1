#Requires -Version 7.0
# Read-only analysis of completed engine JSONL profiles. No engine launches and
# no baseline rewriting. Timing summaries cover all frames; counters cover only
# the profiler's explicitly reported retained-frame window.
param(
    [Parameter(Mandatory = $true)][string]$InputDirectory,
    [Parameter(Mandatory = $true)][string]$ReportPath
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $ReportPath) { throw 'Refusing to overwrite an evidence report' }
$counterNames = @(
    'allocation.cpp.calls', 'allocation.cpp.bytes', 'gpu_scene.upload.bytes',
    'gpu_scene.publication.unchanged_fast_path', 'opaque.indirect.command_count',
    'gpu_scene.visibility.device_commands', 'gpu_scene.visibility.device_mismatched_bins',
    'gpu_scene.visibility.device_overflow_commands', 'opaque.indirect.bin_count',
    'gpu_scene.cpu_mirror.capacity_bytes',
    'gpu_scene.lod.base_triangles', 'gpu_scene.lod.oracle_triangles',
    'gpu_scene.lod.device_triangles', 'gpu_scene.lod.oracle_reduced_commands',
    'gpu_scene.lod.device_mismatched_commands', 'gpu_scene.lod.history_buffer_bytes',
    'gpu_scene.lod.history_valid', 'gpu_scene.lod.history_reset', 'gpu_scene.lod.history_changed'
)
$runs = @(foreach ($file in (Get-ChildItem -LiteralPath $InputDirectory -Filter '*.jsonl' | Sort-Object Name)) {
    $header = $null; $summary = $null; $frameCount = 0
    $counters = @{}
    foreach ($name in $counterNames) {
        $counters[$name] = @{ samples = 0; exact_samples = 0; minimum = $null; maximum = $null }
    }
    foreach ($line in [System.IO.File]::ReadLines($file.FullName)) {
        $value = ConvertFrom-Json -InputObject $line -AsHashtable
        if ($value.ContainsKey('measurement_wall_ns')) { $header = $value }
        elseif ($value.type -eq 'run_summary') { $summary = $value }
        elseif ($value.ContainsKey('counters')) {
            ++$frameCount
            foreach ($counter in $value.counters) {
                if (!$counters.ContainsKey($counter.name)) { continue }
                $range = $counters[$counter.name]
                ++$range.samples
                if ($counter.status -eq 'exact') { ++$range.exact_samples }
                if ($null -eq $range.minimum -or $counter.value -lt $range.minimum) { $range.minimum = $counter.value }
                if ($null -eq $range.maximum -or $counter.value -gt $range.maximum) { $range.maximum = $counter.value }
            }
        }
    }
    if (!$header -or !$summary -or !$summary.cpu_ranges.ContainsKey('cpu.frame.total') -or
        !$summary.gpu_ranges.ContainsKey('gpu.frame')) { throw "Incomplete profile: $($file.Name)" }
    if ($frameCount -ne $summary.frames_retained) { throw "Retained-frame count mismatch: $($file.Name)" }
    $timing = @{}
    foreach ($entry in @(
        @('cpu_frame', $summary.cpu_ranges, 'cpu.frame.total'),
        @('gpu_frame', $summary.gpu_ranges, 'gpu.frame'),
        @('gbuffer_cpu', $summary.cpu_ranges, 'cpu.render.record.gbuffer'),
        @('gbuffer_gpu', $summary.gpu_ranges, 'gpu.gbuffer.opaque'),
        @('compaction_gpu', $summary.gpu_ranges, 'gpu.gpu_scene.frustum_compact'),
        @('publication_cpu', $summary.cpu_ranges, 'cpu.gpu_scene.publish')
    )) {
        $range = $entry[1][$entry[2]]
        $timing[$entry[0]] = if ($range) { @{
            median_ms = $range.median / 1e6; p95_ms = $range.p95 / 1e6; p99_ms = $range.p99 / 1e6
            samples = $range.sample_count; missing = $range.missing_frame_count
            capacity_overflow = $range.sample_capacity_overflow_count
        } } else { $null }
    }
    @{
        file = $file.Name; sha256 = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()
        fixture = $header.benchmark.fixture_id; manifest_sha256 = $header.benchmark.manifest_sha256
        quality = $header.render_configuration.quality_settings
        wall_average_ms = $header.measurement_wall_ns / $header.measured_frames / 1e6
        validation_enabled = $header.validation_enabled; render_extent = $header.render_extent
        frames_completed = $summary.frames_completed; frames_retained = $frameCount
        dropped_frames = $summary.dropped_frames; timing = $timing; counters = $counters
        memory = $summary.memory_latest
    }
})
if ($runs.Count -eq 0) { throw 'No profiles found' }
$report = @{
    schema = 'iridium.m7.derived_run_evidence'; schema_version = 1
    input_directory = (Resolve-Path -LiteralPath $InputDirectory).Path
    timing_scope = 'all measured frames'; counter_scope = 'retained frames only'; runs = $runs
}
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $ReportPath -Encoding utf8
Write-Output "Wrote $($runs.Count) completed run summaries to $ReportPath"
