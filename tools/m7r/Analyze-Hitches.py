"""Analyze M7R hitch-scenario profiles (tools/m7r/Run-HitchScenario.ps1).

Each profile is the CPU JSON Lines from --profile-cpu-output with the records the
harness appends for --qualification-scripted-changes: one "scripted_changes"
header (the applied events) and one "scripted_frame" record per measured frame
(cpu.frame.total, cpu.renderer.drain_all_frames and cpu.renderer.upload_wait per
frame). The profiler's own per-frame detail keeps only its last 512 frames, so the
analysis uses the scripted_frame records.

Per run it reports:
  - median and nearest-rank p99 CPU frame time over every measured frame;
  - hitches: frames above 2x the run's median CPU frame (and, as a secondary
    count, above 2x the median of the previous 120 frames);
  - per event: the maximum CPU frame over the event frame and the 3 frames after
    it (4 frames), with the drain and upload-wait scopes inside that window;
  - frames containing cpu.renderer.drain_all_frames / cpu.renderer.upload_wait,
    split into event windows and the rest of the run.
Per route and side it aggregates across runs (median, min, max).

    python tools/m7r/Analyze-Hitches.py out/m7r/hitch/<label>
    python tools/m7r/Analyze-Hitches.py --profiles a.jsonl b.jsonl [--out dir]
"""
import argparse
import bisect
import json
import math
import statistics
import sys
from pathlib import Path

EVENT_WINDOW = 4
HITCH_FACTOR = 2.0
LOCAL_WINDOW = 120


def nearest_rank(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(fraction * len(ordered)))
    return ordered[rank - 1]


def load_profile(path):
    header = summary = scripted = None
    frames = []
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            record = json.loads(line)
            kind = record.get("type")
            if kind == "run_header":
                header = record
            elif kind == "run_summary":
                summary = record
            elif kind == "scripted_changes":
                scripted = record
            elif kind == "scripted_frame":
                frames.append(record)
    if scripted is None:
        raise SystemExit(f"{path}: no scripted_changes record (run without --qualification-scripted-changes?)")
    return header, summary, scripted, frames


def analyze(path):
    header, summary, scripted, records = load_profile(path)
    frames = {record["m"]: record for record in records if record["m"] >= 0}
    if not frames:
        raise SystemExit(f"{path}: no measured scripted_frame records")
    measured = header["measured_frames"] if header else max(frames) + 1
    missing = [m for m in range(measured) if m not in frames]
    order = sorted(frames)
    cpu = [frames[m]["cpu_ns"] / 1e6 for m in order]
    median = statistics.median(cpu)
    threshold = HITCH_FACTOR * median
    hitches = [m for m in order if frames[m]["cpu_ns"] / 1e6 > threshold]
    # Secondary, segment-insensitive count: frames above 2x the median of the
    # previous LOCAL_WINDOW measured frames (the steady frame changes when a
    # scenario adds work, which moves the whole-run median).
    local_hitches = []
    window_values = []
    for position, m in enumerate(order):
        value = cpu[position]
        if len(window_values) >= LOCAL_WINDOW // 4:
            if value > HITCH_FACTOR * window_values[len(window_values) // 2]:
                local_hitches.append(m)
        bisect.insort(window_values, value)
        if len(window_values) > LOCAL_WINDOW:
            window_values.pop(bisect.bisect_left(window_values, cpu[position - LOCAL_WINDOW]))

    windows = set()
    events = []
    for event in scripted["applied_events"]:
        first = event["applied_measured_frame"]
        window = [m for m in range(first, first + EVENT_WINDOW) if m in frames]
        windows.update(window)
        worst = max(window, key=lambda m: frames[m]["cpu_ns"]) if window else None
        before = [frames[m]["cpu_ns"] / 1e6 for m in range(max(0, first - 100), first) if m in frames]
        entry = {
            "index": event["index"],
            "frame": event["frame"],
            "action": event["action"],
            "result": event.get("result"),
            "apply_ms": event.get("apply_ns", 0) / 1e6,
            "max_ms": frames[worst]["cpu_ns"] / 1e6 if worst is not None else None,
            "max_frame": worst,
            "pre_event_median_ms": statistics.median(before) if before else None,
            "drain_scopes": sum(frames[m]["drain"] for m in window),
            "drain_ms": sum(frames[m]["drain_ns"] for m in window) / 1e6,
            "upload_wait_scopes": sum(frames[m]["upload_wait"] for m in window),
            "upload_wait_ms": sum(frames[m]["upload_wait_ns"] for m in window) / 1e6,
            "hitch_frames": sum(1 for m in window if frames[m]["cpu_ns"] / 1e6 > threshold),
        }
        for key in ("count", "resolution", "color"):
            if key in event:
                entry[key] = event[key]
        events.append(entry)

    def frame_counts(field):
        with_scope = [m for m in order if frames[m][field] > 0]
        return {
            "frames": len(with_scope),
            "in_event_windows": sum(1 for m in with_scope if m in windows),
            "outside_event_windows": sum(1 for m in with_scope if m not in windows),
            "scopes": sum(frames[m][field] for m in order),
            "total_ms": sum(frames[m][field + "_ns"] for m in order) / 1e6,
        }

    run_summary_cpu = None
    if summary:
        value = summary.get("cpu_ranges", {}).get("cpu.frame.total")
        if value:
            run_summary_cpu = {k: value[k] / 1e6 for k in ("median", "p99")}
    return {
        "profile": str(path),
        "scenario_id": scripted["scenario_id"],
        "fixture": header["benchmark"]["fixture_id"] if header else None,
        "commit": header["source"]["commit"] if header else None,
        "dirty": header["source"].get("dirty_at_configure") if header else None,
        "measured_frames": measured,
        "sampled_frames": len(order),
        "missing_frames": len(missing),
        "events_scheduled": scripted["scheduled_events"],
        "events_applied": len(scripted["applied_events"]),
        "cpu_median_ms": median,
        "cpu_p99_ms": nearest_rank(cpu, 0.99),
        "cpu_max_ms": max(cpu),
        "hitch_threshold_ms": threshold,
        "hitches": len(hitches),
        "hitches_in_event_windows": sum(1 for m in hitches if m in windows),
        "hitches_local": len(local_hitches),
        "hitches_local_in_event_windows": sum(1 for m in local_hitches if m in windows),
        "worst_hitches": [
            {"m": m, "ms": frames[m]["cpu_ns"] / 1e6, "drain": frames[m]["drain"],
             "upload_wait": frames[m]["upload_wait"]}
            for m in sorted(hitches, key=lambda m: -frames[m]["cpu_ns"])[:10]
        ],
        "drain_all_frames": frame_counts("drain"),
        "upload_wait": frame_counts("upload_wait"),
        "events": events,
        "run_summary_cpu_frame_ms": run_summary_cpu,
    }


def spread(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return {"median": statistics.median(values), "min": min(values), "max": max(values)}


def aggregate(results):
    groups = {}
    for result in results:
        groups.setdefault((result["route"], result["side"]), []).append(result)
    output = []
    for (route, side), group in sorted(groups.items()):
        events = []
        for position, event in enumerate(group[0]["events"]):
            per_run = [g["events"][position] for g in group if len(g["events"]) > position]
            events.append({
                "index": event["index"], "frame": event["frame"], "action": event["action"],
                "max_ms": spread([e["max_ms"] for e in per_run]),
                "drain_scopes": spread([e["drain_scopes"] for e in per_run]),
                "upload_wait_scopes": spread([e["upload_wait_scopes"] for e in per_run]),
            })
        output.append({
            "route": route, "side": side, "runs": len(group),
            "cpu_median_ms": spread([g["cpu_median_ms"] for g in group]),
            "cpu_p99_ms": spread([g["cpu_p99_ms"] for g in group]),
            "hitches": spread([g["hitches"] for g in group]),
            "hitches_local": spread([g["hitches_local"] for g in group]),
            "drain_frames": spread([g["drain_all_frames"]["frames"] for g in group]),
            "drain_frames_outside_events": spread([g["drain_all_frames"]["outside_event_windows"] for g in group]),
            "upload_wait_frames": spread([g["upload_wait"]["frames"] for g in group]),
            "events": events,
        })
    return output


def fmt(value, digits=2):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def markdown(results, aggregates):
    lines = ["# M7R hitch scenario", ""]
    lines.append("Per run (CPU frame = cpu.frame.total; hitch = frame > 2x run median; "
                 "event max = worst of the event frame and the 3 after it).")
    lines.append("")
    lines.append("| route | run | side | commit | median ms | p99 ms | max ms | hitches (in events) | local hitches (in events) | drain frames (events/other) | upload-wait frames (events/other) |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for r in results:
        d, u = r["drain_all_frames"], r["upload_wait"]
        lines.append(
            f"| {r['route']} | {r['index']} | {r['side']} | {str(r['commit'])[:8]} | {fmt(r['cpu_median_ms'])} | "
            f"{fmt(r['cpu_p99_ms'])} | {fmt(r['cpu_max_ms'])} | {r['hitches']} ({r['hitches_in_event_windows']}) | "
            f"{r['hitches_local']} ({r['hitches_local_in_event_windows']}) | {d['frames']} ({d['in_event_windows']}/{d['outside_event_windows']}) | "
            f"{u['frames']} ({u['in_event_windows']}/{u['outside_event_windows']}) |")
    for r in results:
        lines += ["", f"## {r['route']} run {r['index']} ({r['side']}): events", "",
                  "| # | frame | action | result | pre-event median ms | max ms (4 frames) | drains | drain ms | upload waits | apply ms |",
                  "|---|---|---|---|---|---|---|---|---|---|"]
        for e in r["events"]:
            lines.append(
                f"| {e['index']} | {e['frame']} | {e['action']} | {fmt(e['result'])} | {fmt(e['pre_event_median_ms'])} | "
                f"{fmt(e['max_ms'])} | {e['drain_scopes']} | {fmt(e['drain_ms'])} | {e['upload_wait_scopes']} | {fmt(e['apply_ms'])} |")
    lines += ["", "## Aggregate (median [min..max] across runs)", ""]
    for a in aggregates:
        def s(x):
            return "-" if x is None else f"{fmt(x['median'])} [{fmt(x['min'])}..{fmt(x['max'])}]"
        lines += [f"### {a['route']} side {a['side']} ({a['runs']} runs)", "",
                  f"- CPU median ms: {s(a['cpu_median_ms'])}",
                  f"- CPU p99 ms: {s(a['cpu_p99_ms'])}",
                  f"- hitches: {s(a['hitches'])}; local (2x median of the previous {LOCAL_WINDOW} frames): {s(a['hitches_local'])}",
                  f"- drain frames: {s(a['drain_frames'])} (outside event windows {s(a['drain_frames_outside_events'])})",
                  f"- upload-wait frames: {s(a['upload_wait_frames'])}", "",
                  "| # | frame | action | max ms | drains | upload waits |", "|---|---|---|---|---|---|"]
        for e in a["events"]:
            lines.append(f"| {e['index']} | {e['frame']} | {e['action']} | {s(e['max_ms'])} | {s(e['drain_scopes'])} | {s(e['upload_wait_scopes'])} |")
        lines.append("")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", nargs="?", help="Run-HitchScenario.ps1 output directory (runs.json)")
    parser.add_argument("--profiles", nargs="*", default=[], help="profile JSONL files instead of runs.json")
    parser.add_argument("--out", help="where to write summary.json/summary.md (default: the directory)")
    args = parser.parse_args()

    if args.profiles:
        runs = [{"route": Path(p).stem.split("__")[0], "index": i + 1, "side": "A", "profile": p}
                for i, p in enumerate(args.profiles)]
        out = Path(args.out or Path(args.profiles[0]).parent)
    elif args.directory:
        directory = Path(args.directory)
        runs = json.loads((directory / "runs.json").read_text(encoding="utf-8-sig"))
        if isinstance(runs, dict):
            runs = [runs]
        out = Path(args.out or directory)
    else:
        parser.error("give a directory or --profiles")

    results = []
    for run in runs:
        result = analyze(run["profile"])
        result.update({"route": run["route"], "index": run["index"], "side": run["side"]})
        results.append(result)
    aggregates = aggregate(results)
    out.mkdir(parents=True, exist_ok=True)
    (out / "summary.json").write_text(json.dumps({"runs": results, "aggregate": aggregates}, indent=2), encoding="utf-8")
    text = markdown(results, aggregates)
    (out / "summary.md").write_text(text, encoding="utf-8")
    print(text)
    incomplete = [r for r in results if r["missing_frames"] or r["events_applied"] != r["events_scheduled"]]
    for r in incomplete:
        print(f"WARNING: {r['profile']}: {r['missing_frames']} missing frames, "
              f"{r['events_applied']}/{r['events_scheduled']} events applied", file=sys.stderr)
    return 1 if incomplete else 0


if __name__ == "__main__":
    sys.exit(main())
