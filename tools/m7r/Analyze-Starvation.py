"""Analyze an M7R R5b.3 cook-while-render starvation run (Run-StarvationTest.ps1).

Reads <dir>/runs.json and writes summary.json and summary.md. Per route, A runs are
frame work only and B runs add the background cook; both run the frame-task probe.

Pass criteria (R5 design section 4.2, ADR-0015):
  1. non-wait CPU median and p99, B against A, within the noise band + 3 %
     (noise band: the larger of the A/A and B/B spreads, relative to A);
  2. frame-task worker start latency p99 <= 50 us in the B runs;
  3. no frame over 2x the median caused by the frame task waiting (B runs);
  4. cook throughput >= 90 % of the pre-R5b dedicated-thread build (isolated cooks);
plus every background cook produced the same artifact hash.

    python tools/m7r/Analyze-Starvation.py <DataRoot>/out/m7r/starvation/<label>
"""
import json
import statistics
import sys
from pathlib import Path

WAIT_SCOPES = ("cpu.renderer.frame_fence_wait", "cpu.renderer.acquire", "cpu.renderer.present")
NOISE_ALLOWANCE_PERCENT = 3.0
WORKER_START_P99_LIMIT_US = 50.0
THROUGHPUT_FLOOR = 0.90


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1) + 0.5))]


def iridium_line(log_path, tag):
    prefix = tag + " "
    with open(log_path, encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith(prefix):
                return json.loads(line[len(prefix):])
    return None


def profile_summary(path):
    non_wait, start_latency = [], []
    whole = {}
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            record = json.loads(line)
            kind = record.get("type")
            if kind == "frame":
                totals = {}
                for event in record["cpu_events"]:
                    totals[event["name"]] = totals.get(event["name"], 0) + event["duration_ns"]
                frame = totals.get("cpu.frame.total")
                if frame is not None:
                    non_wait.append(frame - sum(totals.get(name, 0) for name in WAIT_SCOPES))
                for counter in record.get("counters", []):
                    if counter["name"] == "task.frame.start_latency_us":
                        start_latency.append(counter["value"])
            elif kind == "run_summary":
                for name in ("cpu.frame.total", "cpu.task.probe"):
                    value = record["cpu_ranges"].get(name)
                    if value:
                        whole[name] = {k: value[k] / 1e6 for k in ("median", "p99")}
    return {
        "retained_frames": len(non_wait),
        "non_wait_ms": {
            "median": statistics.median(non_wait) / 1e6 if non_wait else None,
            "p99": (percentile(non_wait, 0.99) or 0) / 1e6,
        },
        "whole_run_ms": whole,
        "task_frame_start_latency_us_p99": percentile(start_latency, 0.99),
    }


def relative_delta(a_values, b_values):
    mean_a = statistics.mean(a_values)
    mean_b = statistics.mean(b_values)
    spread = max(abs(a_values[0] - a_values[-1]), abs(b_values[0] - b_values[-1]))
    return {
        "a_ms": mean_a,
        "b_ms": mean_b,
        "delta_percent": 100.0 * (mean_b - mean_a) / mean_a if mean_a else None,
        "noise_percent": 100.0 * spread / mean_a if mean_a else None,
    }


def main(directory):
    out = Path(directory)
    runs = json.loads((out / "runs.json").read_text(encoding="utf-8-sig"))
    routes = {}
    for run in [r for r in runs if r["kind"] == "frames"]:
        entry = routes.setdefault(run["route"], {"A": [], "B": []})
        summary = profile_summary(run["profile"])
        summary["probe"] = iridium_line(run["log"], "IRIDIUM_FRAME_TASK_PROBE")
        summary["cook"] = iridium_line(run["log"], "IRIDIUM_BACKGROUND_COOK")
        summary["index"] = run["index"]
        entry[run["side"]].append(summary)

    criteria = []
    route_results = {}
    for route, sides in routes.items():
        result = {}
        a, b = sides["A"], sides["B"]
        for metric in ("median", "p99"):
            delta = relative_delta([r["non_wait_ms"][metric] for r in a],
                [r["non_wait_ms"][metric] for r in b])
            result[f"non_wait_{metric}"] = delta
            criteria.append({
                "route": route,
                "criterion": f"non-wait CPU {metric} within noise + {NOISE_ALLOWANCE_PERCENT:g} %",
                "value": f"{delta['delta_percent']:+.2f} % (noise {delta['noise_percent']:.2f} %)",
                "pass": delta["delta_percent"] <= delta["noise_percent"] + NOISE_ALLOWANCE_PERCENT,
            })
        worker_start = [r["probe"]["worker_start_us"]["p99"] for r in b if r["probe"]]
        result["worker_start_us_p99_b"] = worker_start
        result["worker_start_us_p99_a"] = [r["probe"]["worker_start_us"]["p99"] for r in a if r["probe"]]
        criteria.append({
            "route": route,
            "criterion": f"frame-task worker start p99 <= {WORKER_START_P99_LIMIT_US:g} us (B)",
            "value": ", ".join(f"{v:.1f} us" for v in worker_start) or "missing",
            "pass": bool(worker_start) and max(worker_start) <= WORKER_START_P99_LIMIT_US,
        })
        slow = [r["probe"]["frames_over_2x_median_from_frame_task_wait"] for r in b if r["probe"]]
        slow_a = [r["probe"]["frames_over_2x_median_from_frame_task_wait"] for r in a if r["probe"]]
        result["slow_frames_from_frame_task_b"] = slow
        result["slow_frames_from_frame_task_a"] = slow_a
        # The A runs show what the machine does without cooking: on a loaded
        # machine both sides have such frames, and only B counts.
        criteria.append({
            "route": route,
            "criterion": "no frame over 2x median from a frame-task wait (B)",
            "value": (", ".join(str(v) for v in slow) or "missing") +
                " (A: " + (", ".join(str(v) for v in slow_a) or "missing") + ")",
            "pass": bool(slow) and max(slow) == 0,
        })
        cooks = [r["cook"] for r in b if r["cook"]]
        result["background_cooks"] = cooks
        criteria.append({
            "route": route,
            "criterion": "background cooks completed with identical artifacts (B)",
            "value": ", ".join(f"{c['completed']} cooks, {c['cooks_per_minute']:.2f}/min" for c in cooks) or "missing",
            "pass": bool(cooks) and all(c["completed"] > 0 and c["failed"] == 0 and
                c["artifact_hashes_identical"] for c in cooks) and
                len({c["artifact_hash"] for c in cooks}) == 1,
        })
        result["runs"] = {"A": a, "B": b}
        route_results[route] = result

    cook_runs = [r for r in runs if r["kind"] == "cook"]
    throughput = {}
    for side in ("A", "B"):
        seconds = [r["seconds"] for r in cook_runs if r["side"] == side]
        if seconds:
            throughput[side] = {"seconds": seconds, "mean_seconds": statistics.mean(seconds),
                "cooks_per_minute": 60.0 / statistics.mean(seconds)}
    hashes = {r["artifactHash"] for r in cook_runs}
    if "A" in throughput and "B" in throughput:
        ratio = throughput["B"]["cooks_per_minute"] / throughput["A"]["cooks_per_minute"]
        criteria.append({
            "route": "isolated cook",
            "criterion": f"cook throughput >= {THROUGHPUT_FLOOR:.0%} of the pre-R5b build",
            "value": f"{ratio:.1%} ({throughput['B']['mean_seconds']:.2f} s against {throughput['A']['mean_seconds']:.2f} s)",
            "pass": ratio >= THROUGHPUT_FLOOR,
        })
    elif cook_runs:
        criteria.append({
            "route": "isolated cook",
            "criterion": f"cook throughput >= {THROUGHPUT_FLOOR:.0%} of the pre-R5b build",
            "value": "no -BaselineCookExe: not measured",
            "pass": None,
        })
    if cook_runs:
        criteria.append({
            "route": "isolated cook",
            "criterion": "isolated cooks produced one artifact hash",
            "value": ", ".join(sorted(h[:16] for h in hashes)),
            "pass": len(hashes) == 1,
        })

    passed = all(c["pass"] is not False for c in criteria)
    summary = {"criteria": criteria, "routes": route_results, "cook_throughput": throughput,
        "passed": passed}
    (out / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    lines = [f"# Starvation test {out.name}", "", "| Route | Criterion | Value | Result |",
        "|---|---|---|---|"]
    for c in criteria:
        verdict = "not measured" if c["pass"] is None else ("pass" if c["pass"] else "FAIL")
        lines.append(f"| {c['route']} | {c['criterion']} | {c['value']} | {verdict} |")
    lines += ["", f"Overall: {'pass' if passed else 'FAIL'}"]
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 0 if passed else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    sys.exit(main(sys.argv[1]))
