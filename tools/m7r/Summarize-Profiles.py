"""Summarize M7R timing-pair profiles (CPU JSON Lines from --profile-cpu-output).

Reads <dir>/runs.json written by Run-TimingPair.ps1 and writes summary.json and
summary.md next to it. Whole-run medians/p95/p99 come from the run summary; non-wait
CPU (frame total minus fence wait, acquire and present), allocation counters and
drain counts come from the retained per-frame records.

    python tools/m7r/Summarize-Profiles.py out/m7r/timing/<label>
"""
import json
import statistics
import sys
from pathlib import Path

WAIT_SCOPES = ("cpu.renderer.frame_fence_wait", "cpu.renderer.acquire", "cpu.renderer.present")
DRAIN_SCOPES = ("cpu.renderer.drain_all_frames", "cpu.renderer.upload_wait")


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1) + 0.5))]


def summarize(path):
    header = summary = None
    non_wait, alloc_calls, alloc_bytes = [], [], []
    drain_frames = {name: 0 for name in DRAIN_SCOPES}
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            record = json.loads(line)
            kind = record.get("type")
            if kind == "run_header":
                header = record
            elif kind == "run_summary":
                summary = record
            elif kind == "frame":
                totals = {}
                for event in record["cpu_events"]:
                    totals[event["name"]] = totals.get(event["name"], 0) + event["duration_ns"]
                frame = totals.get("cpu.frame.total")
                if frame is not None:
                    non_wait.append(frame - sum(totals.get(name, 0) for name in WAIT_SCOPES))
                for name in DRAIN_SCOPES:
                    if name in totals:
                        drain_frames[name] += 1
                for counter in record.get("counters", []):
                    if counter["name"] == "allocation.cpp.calls":
                        alloc_calls.append(counter["value"])
                    elif counter["name"] == "allocation.cpp.bytes":
                        alloc_bytes.append(counter["value"])
    if header is None or summary is None:
        raise SystemExit(f"incomplete profile: {path}")

    def whole(ranges, name):
        value = ranges.get(name)
        if not value:
            return None
        return {k: value[k] / 1e6 for k in ("median", "p95", "p99")} | {"samples": value["sample_count"]}

    cpu, gpu = summary["cpu_ranges"], summary["gpu_ranges"]
    return {
        "fixture": header["benchmark"]["fixture_id"],
        "commit": header["source"]["commit"],
        "dirty": header["source"].get("dirty_at_configure"),
        "measured_frames": header["measured_frames"],
        "wall_average_ms": header["measurement_wall_ns"] / header["measured_frames"] / 1e6,
        "cpu_frame": whole(cpu, "cpu.frame.total"),
        "cpu_fence_wait": whole(cpu, "cpu.renderer.frame_fence_wait"),
        "gpu_frame": whole(gpu, "gpu.frame"),
        "drain_scope_frames_whole_run": {name: (cpu.get(name) or {}).get("sample_count", 0) for name in DRAIN_SCOPES},
        "retained_frames": len(non_wait),
        "non_wait_cpu_retained_ms": {
            "median": statistics.median(non_wait) / 1e6 if non_wait else None,
            "p95": (percentile(non_wait, 0.95) or 0) / 1e6,
            "p99": (percentile(non_wait, 0.99) or 0) / 1e6,
        },
        "allocation_calls_per_frame": {"median": statistics.median(alloc_calls) if alloc_calls else None,
                                       "max": max(alloc_calls) if alloc_calls else None},
        "allocation_bytes_per_frame": {"median": statistics.median(alloc_bytes) if alloc_bytes else None,
                                       "max": max(alloc_bytes) if alloc_bytes else None},
    }


def fmt(value, digits=4):
    return "-" if value is None else f"{value:.{digits}f}"


def main():
    directory = Path(sys.argv[1])
    runs = json.loads((directory / "runs.json").read_text(encoding="utf-8-sig"))
    if isinstance(runs, dict):
        runs = [runs]
    results = []
    for run in runs:
        result = summarize(run["profile"])
        result |= {"route": run["route"], "index": run["index"], "side": run["side"]}
        results.append(result)
    (directory / "summary.json").write_text(json.dumps(results, indent=2), encoding="utf-8")

    lines = ["| Route | Run | Side | CPU med/p95/p99 ms | Non-wait CPU med/p99 ms | GPU med/p95/p99 ms | Alloc calls med/max | Drain frames (all/upload) |",
             "|---|---:|---|---|---|---|---|---|"]
    for r in results:
        c, g, n = r["cpu_frame"] or {}, r["gpu_frame"] or {}, r["non_wait_cpu_retained_ms"]
        d = r["drain_scope_frames_whole_run"]
        lines.append(f"| {r['route']} | {r['index']} | {r['side']} | {fmt(c.get('median'))}/{fmt(c.get('p95'))}/{fmt(c.get('p99'))} | "
                     f"{fmt(n['median'])}/{fmt(n['p99'])} | {fmt(g.get('median'))}/{fmt(g.get('p95'))}/{fmt(g.get('p99'))} | "
                     f"{r['allocation_calls_per_frame']['median']}/{r['allocation_calls_per_frame']['max']} | "
                     f"{d['cpu.renderer.drain_all_frames']}/{d['cpu.renderer.upload_wait']} |")
    lines += ["", "| Route | Side | CPU median (mean of 2) | GPU median (mean of 2) | Non-wait CPU median (mean of 2) |", "|---|---|---:|---:|---:|"]
    for route in sorted({r["route"] for r in results}):
        for side in ("A", "B"):
            subset = [r for r in results if r["route"] == route and r["side"] == side]
            if not subset:
                continue
            mean = lambda key, sub=subset, field="median": statistics.mean(
                (s[key] or {}).get(field) or 0 for s in sub)
            lines.append(f"| {route} | {side} | {fmt(mean('cpu_frame'))} | {fmt(mean('gpu_frame'))} | {fmt(mean('non_wait_cpu_retained_ms'))} |")
    (directory / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
