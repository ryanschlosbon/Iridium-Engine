"""Summarize an M9 feature-admission run (tools/m9/Run-FeatureAdmission.ps1).

Reads <dir>/runs.json (and machine-state.json when present) and writes summary.json
and summary.md next to them. Per-run figures come from tools/m7r/Summarize-Profiles.py
(imported, not modified): whole-run CPU frame / GPU frame median, p95, p99 from the run
summary; non-wait CPU, allocation calls/bytes and drain-frame counts from the retained
per-frame records. This script adds:

  * per side: the median of the five run medians (and of the run p95s/p99s) with the
    min/max spread of the run values;
  * per route: B-A deltas (absolute and %) for median/p95/p99, and how many of the five
    matched pairs (consecutive A/B processes in the interleaved order) favour B;
  * per-pass GPU-range and CPU-scope medians per side with B-A deltas sorted by absolute
    delta, for bisecting a regression by pass before attributing it;
  * engine memory (committed live/peak, persistent vs transient by lifetime class) and
    the counters whose retained-frame medians differ between the sides.

Standard library only.

    python tools/m9/Summarize-Admission.py out/m9/timing/<label>
"""
import importlib.util
import json
import statistics
import sys
from pathlib import Path

_M7R = Path(__file__).resolve().parent.parent / "m7r" / "Summarize-Profiles.py"
_spec = importlib.util.spec_from_file_location("m7r_summarize_profiles", _M7R)
m7r = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(m7r)

FIELDS = ("median", "p95", "p99")
METRICS = (  # key in the m7r per-run result, label
    ("cpu_frame", "CPU frame"),
    ("non_wait_cpu_retained_ms", "Non-wait CPU (retained)"),
    ("gpu_frame", "GPU frame"),
)
# Counters that legitimately vary frame to frame or by identity rather than workload.
COUNTER_DIFF_IGNORE = ("allocation.cpp.calls", "allocation.cpp.bytes", "transparent.oit.order_seed")


def median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def delta(a, b):
    if a is None or b is None:
        return {"abs": None, "pct": None}
    return {"abs": b - a, "pct": (b - a) / a * 100.0 if a else None}


def scan_extras(path):
    """Per-pass ranges, memory and counter medians not covered by the m7r summary."""
    summary = None
    counters = {}
    memory = None
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            record = json.loads(line)
            kind = record.get("type")
            if kind == "run_summary":
                summary = record
            elif kind == "frame":
                for counter in record.get("counters", []):
                    counters.setdefault(counter["name"], []).append(counter["value"])
                if record.get("memory"):
                    memory = record["memory"]
    if summary is None:
        raise SystemExit(f"incomplete profile: {path}")

    def ranges(table):
        return {name: {k: value[k] / 1e6 for k in FIELDS if value.get(k) is not None}
                | {"samples": value.get("sample_count")}
                for name, value in table.items() if value and value.get("sample_count")}

    by_class = {}
    for category in (memory or {}).get("categories", []):
        entry = by_class.setdefault(category.get("lifetime_class", "unknown"),
                                    {"committed_live_bytes": 0, "committed_peak_bytes": 0})
        entry["committed_live_bytes"] += category.get("committed_live_bytes", 0) or 0
        entry["committed_peak_bytes"] += category.get("committed_peak_bytes", 0) or 0
    latest = summary.get("memory_latest") or {}
    return {
        "frames_completed": summary.get("frames_completed"),
        "frames_retained": summary.get("frames_retained"),
        "dropped_frames": summary.get("dropped_frames"),
        "gpu_ranges": ranges(summary.get("gpu_ranges", {})),
        "cpu_ranges": ranges(summary.get("cpu_ranges", {})),
        "memory": {
            "committed_live_bytes": latest.get("committed_live_bytes"),
            "committed_peak_bytes": latest.get("committed_peak_bytes"),
            "live_allocation_count": latest.get("live_allocation_count"),
            "by_lifetime_class": by_class,
        },
        "counter_medians": {name: statistics.median(values) for name, values in counters.items()},
    }


def side_stats(runs, getter):
    values = [getter(r) for r in runs]
    values = [v for v in values if v is not None]
    if not values:
        return None
    return {"median": statistics.median(values), "min": min(values), "max": max(values), "runs": values}


def summarize_route(route, runs, order):
    sides = {s: [r for r in runs if r["side"] == s] for s in ("A", "B")}
    result = {"route": route, "processes": {s: len(v) for s, v in sides.items()}, "metrics": {}}
    for key, label in METRICS:
        metric = {"label": label, "sides": {}, "delta": {}}
        for field in FIELDS:
            for s in ("A", "B"):
                metric["sides"].setdefault(s, {})[field] = side_stats(sides[s], lambda r: (r[key] or {}).get(field))
            a = (metric["sides"]["A"][field] or {}).get("median")
            b = (metric["sides"]["B"][field] or {}).get("median")
            metric["delta"][field] = delta(a, b)
        # Matched pairs: consecutive processes in run-index order (A,B / B,A / ...).
        ordered = sorted(runs, key=lambda r: r["index"])
        pairs = []
        for i in range(0, len(ordered) - 1, 2):
            pair = {r["side"]: r for r in ordered[i:i + 2]}
            if set(pair) != {"A", "B"}:
                continue
            a = (pair["A"][key] or {}).get("median")
            b = (pair["B"][key] or {}).get("median")
            if a is None or b is None:
                continue
            pairs.append({"indices": [pair["A"]["index"], pair["B"]["index"]], "a": a, "b": b, "favours_b": b < a})
        metric["pairs"] = pairs
        metric["pairs_favouring_b"] = sum(p["favours_b"] for p in pairs)
        result["metrics"][key] = metric

    def pass_table(kind):
        names = sorted({n for r in runs for n in r["extras"][kind]})
        rows = []
        for name in names:
            row = {"name": name}
            for s in ("A", "B"):
                row[s] = side_stats(sides[s], lambda r: r["extras"][kind].get(name, {}).get("median"))
            a = (row["A"] or {}).get("median")
            b = (row["B"] or {}).get("median")
            row["delta"] = delta(a, b)
            rows.append(row)
        rows.sort(key=lambda row: -abs(row["delta"]["abs"] or 0))
        return rows

    result["gpu_passes"] = pass_table("gpu_ranges")
    result["cpu_scopes"] = pass_table("cpu_ranges")

    result["allocations"] = {s: {
        "calls_median": side_stats(sides[s], lambda r: r["allocation_calls_per_frame"]["median"]),
        "calls_max": max((r["allocation_calls_per_frame"]["max"] or 0) for r in sides[s]) if sides[s] else None,
        "bytes_median": side_stats(sides[s], lambda r: r["allocation_bytes_per_frame"]["median"]),
        "bytes_max": max((r["allocation_bytes_per_frame"]["max"] or 0) for r in sides[s]) if sides[s] else None,
    } for s in ("A", "B")}
    result["drain_frames"] = {s: {name: sum(r["drain_scope_frames_whole_run"].get(name, 0) for r in sides[s])
                                  for name in m7r.DRAIN_SCOPES} for s in ("A", "B")}
    result["memory"] = {s: {
        "committed_live_bytes": side_stats(sides[s], lambda r: r["extras"]["memory"]["committed_live_bytes"]),
        "committed_peak_bytes": side_stats(sides[s], lambda r: r["extras"]["memory"]["committed_peak_bytes"]),
        "by_lifetime_class": {cls: side_stats(sides[s], lambda r, c=cls: r["extras"]["memory"]["by_lifetime_class"]
                                              .get(c, {}).get("committed_live_bytes"))
                              for cls in sorted({c for r in sides[s] for c in r["extras"]["memory"]["by_lifetime_class"]})},
    } for s in ("A", "B")}

    counter_names = sorted({n for r in runs for n in r["extras"]["counter_medians"]} - set(COUNTER_DIFF_IGNORE))
    diffs = []
    for name in counter_names:
        a = median([r["extras"]["counter_medians"].get(name) for r in sides["A"]])
        b = median([r["extras"]["counter_medians"].get(name) for r in sides["B"]])
        if a != b:
            diffs.append({"name": name, "A": a, "B": b})
    result["counter_differences"] = diffs
    result["commits"] = {s: sorted({f"{r['commit']}{'+dirty' if r['dirty'] else ''}" for r in sides[s]}) for s in ("A", "B")}
    return result


def fmt(value, digits=4):
    return "-" if value is None else f"{value:.{digits}f}"


def fmt_pct(value):
    return "-" if value is None else f"{value:+.2f}%"


def fmt_mb(value):
    return "-" if value is None else f"{value / (1024 * 1024):.1f}"


def render(config, results, routes, machine):
    lines = [f"# Feature admission: {config.get('label', '(runs.json without config)')}", ""]
    sides = config.get("sides", {})
    for s in ("A", "B") if config else ():
        info = sides.get(s, {})
        args = " ".join(info.get("args") or [])
        lines.append(f"- **{s}**: `{info.get('exe', '')}`{(' `' + args + '`') if args else ''}")
    extra = " ".join(config.get("extra_args") or [])
    if config:
        lines += [f"- Order {','.join(config.get('order', []))}; warm-up {config.get('warmup_frames')}, "
                  f"measured {config.get('measured_frames')}; pipeline cache {config.get('pipeline_cache')}"
                  f"{'; extra `' + extra + '`' if extra else ''}"]
    if machine:
        pre = machine.get("preflight") or {}
        lines.append(f"- Preflight quiet: **{pre.get('quiet')}** (GPU util mean {fmt(pre.get('gpu_utilization_mean'), 1)}%; "
                     f"{len(pre.get('busy_processes') or [])} busy process(es))" +
                     ("".join(f"; {p}" for p in pre.get("problems") or [])))
    lines.append("")

    for route in routes:
        lines += [f"## {route['route']}", ""]
        a_commits, b_commits = route["commits"]["A"], route["commits"]["B"]
        lines.append(f"Commits: A {', '.join(a_commits)}; B {', '.join(b_commits)}")
        lines += ["", "| Metric | A median of run medians [min-max] | B median of run medians [min-max] | B-A median | B-A p95 | B-A p99 | Pairs favouring B |",
                  "|---|---|---|---|---|---|---:|"]
        for key, metric in route["metrics"].items():
            cells = []
            for s in ("A", "B"):
                st = metric["sides"][s]["median"]
                cells.append("-" if st is None else f"{fmt(st['median'])} [{fmt(st['min'])}-{fmt(st['max'])}]")
            d = metric["delta"]
            lines.append(f"| {metric['label']} ms | {cells[0]} | {cells[1]} | "
                         + " | ".join(f"{fmt(d[f]['abs'])} ({fmt_pct(d[f]['pct'])})" for f in FIELDS)
                         + f" | {metric['pairs_favouring_b']}/{len(metric['pairs'])} |")
        lines += ["", "| Side | p95 med of runs | p99 med of runs | GPU p95 | GPU p99 | Alloc calls/frame med (max) | Alloc bytes/frame med (max) | Drain frames (all/upload) | Committed live / peak MB |",
                  "|---|---|---|---|---|---|---|---|---|"]
        for s in ("A", "B"):
            cpu = route["metrics"]["cpu_frame"]["sides"][s]
            gpu = route["metrics"]["gpu_frame"]["sides"][s]
            al = route["allocations"][s]
            dr = route["drain_frames"][s]
            mem = route["memory"][s]
            lines.append(f"| {s} | {fmt((cpu['p95'] or {}).get('median'))} | {fmt((cpu['p99'] or {}).get('median'))} | "
                         f"{fmt((gpu['p95'] or {}).get('median'))} | {fmt((gpu['p99'] or {}).get('median'))} | "
                         f"{fmt((al['calls_median'] or {}).get('median'), 1)} ({al['calls_max']}) | "
                         f"{fmt((al['bytes_median'] or {}).get('median'), 1)} ({al['bytes_max']}) | "
                         f"{dr['cpu.renderer.drain_all_frames']}/{dr['cpu.renderer.upload_wait']} | "
                         f"{fmt_mb((mem['committed_live_bytes'] or {}).get('median'))} / {fmt_mb((mem['committed_peak_bytes'] or {}).get('median'))} |")
        classes = sorted(set(route["memory"]["A"]["by_lifetime_class"]) | set(route["memory"]["B"]["by_lifetime_class"]))
        if classes:
            lines += ["", "| Memory class (committed live MB) | A | B | B-A |", "|---|---:|---:|---:|"]
            for cls in classes:
                a = (route["memory"]["A"]["by_lifetime_class"].get(cls) or {}).get("median")
                b = (route["memory"]["B"]["by_lifetime_class"].get(cls) or {}).get("median")
                lines.append(f"| {cls} | {fmt_mb(a)} | {fmt_mb(b)} | {fmt_mb(None if a is None or b is None else b - a)} |")
        lines += ["", "GPU passes (median of run medians, sorted by |B-A|):", "",
                  "| Pass | A ms [min-max] | B ms [min-max] | B-A ms | B-A % |", "|---|---|---|---:|---:|"]
        for row in route["gpu_passes"]:
            cells = ["-" if row[s] is None else f"{fmt(row[s]['median'])} [{fmt(row[s]['min'])}-{fmt(row[s]['max'])}]" for s in ("A", "B")]
            lines.append(f"| {row['name']} | {cells[0]} | {cells[1]} | {fmt(row['delta']['abs'])} | {fmt_pct(row['delta']['pct'])} |")
        lines += ["", "CPU scopes, top 15 by |B-A|:", "", "| Scope | A ms | B ms | B-A ms | B-A % |", "|---|---:|---:|---:|---:|"]
        for row in route["cpu_scopes"][:15]:
            lines.append(f"| {row['name']} | {fmt((row['A'] or {}).get('median'))} | {fmt((row['B'] or {}).get('median'))} | "
                         f"{fmt(row['delta']['abs'])} | {fmt_pct(row['delta']['pct'])} |")
        if route["counter_differences"]:
            lines += ["", "Counters whose retained-frame medians differ (median over runs):", "", "| Counter | A | B |", "|---|---:|---:|"]
            for diff in route["counter_differences"][:40]:
                lines.append(f"| {diff['name']} | {'absent' if diff['A'] is None else diff['A']} | {'absent' if diff['B'] is None else diff['B']} |")
            if len(route["counter_differences"]) > 40:
                lines.append(f"| ... {len(route['counter_differences']) - 40} more in summary.json | | |")
        else:
            lines += ["", "Counters: no retained-frame median differs between A and B."]
        lines.append("")

    lines += ["## Runs", "", "| Route | Run | Side | CPU med/p95/p99 ms | Non-wait CPU med/p95/p99 ms | GPU med/p95/p99 ms | Alloc calls med/max | Alloc bytes med/max | Drain frames (all/upload) | GPU before (MHz/W/C/util%) |",
              "|---|---:|---|---|---|---|---|---|---|---|"]
    states = {(m["route"], m["index"]): m for m in (machine or {}).get("runs", [])}
    for r in results:
        c, g, n = r["cpu_frame"] or {}, r["gpu_frame"] or {}, r["non_wait_cpu_retained_ms"]
        d = r["drain_scope_frames_whole_run"]
        gpu = ((states.get((r["route"], r["index"])) or {}).get("before") or {}).get("gpu") or {}
        state = "-" if not gpu else f"{gpu.get('clocks.gr')}/{gpu.get('power.draw')}/{gpu.get('temperature.gpu')}/{gpu.get('utilization.gpu')}"
        lines.append(f"| {r['route']} | {r['index']} | {r['side']} | {fmt(c.get('median'))}/{fmt(c.get('p95'))}/{fmt(c.get('p99'))} | "
                     f"{fmt(n['median'])}/{fmt(n['p95'])}/{fmt(n['p99'])} | {fmt(g.get('median'))}/{fmt(g.get('p95'))}/{fmt(g.get('p99'))} | "
                     f"{r['allocation_calls_per_frame']['median']}/{r['allocation_calls_per_frame']['max']} | "
                     f"{r['allocation_bytes_per_frame']['median']}/{r['allocation_bytes_per_frame']['max']} | "
                     f"{d['cpu.renderer.drain_all_frames']}/{d['cpu.renderer.upload_wait']} | {state} |")
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    directory = Path(sys.argv[1])
    document = json.loads((directory / "runs.json").read_text(encoding="utf-8-sig"))
    config = document.get("config", {}) if isinstance(document, dict) else {}
    runs = document.get("runs", []) if isinstance(document, dict) else document
    if isinstance(runs, dict):
        runs = [runs]
    if not runs:
        raise SystemExit("runs.json lists no runs")
    machine_path = directory / "machine-state.json"
    machine = json.loads(machine_path.read_text(encoding="utf-8-sig")) if machine_path.exists() else None

    results = []
    for run in runs:
        result = m7r.summarize(run["profile"])
        result |= {"route": run["route"], "index": run["index"], "side": run["side"]}
        result["extras"] = scan_extras(run["profile"])
        results.append(result)

    route_names = list(dict.fromkeys(r["route"] for r in results))
    routes = [summarize_route(name, [r for r in results if r["route"] == name], config.get("order")) for name in route_names]

    per_run = []
    for r in results:
        slim = {k: v for k, v in r.items() if k != "extras"}
        slim["memory"] = r["extras"]["memory"]
        slim["gpu_ranges"] = r["extras"]["gpu_ranges"]
        slim["frames_completed"] = r["extras"]["frames_completed"]
        slim["dropped_frames"] = r["extras"]["dropped_frames"]
        per_run.append(slim)
    output = {"config": config, "routes": routes, "runs": per_run}
    (directory / "summary.json").write_text(json.dumps(output, indent=2), encoding="utf-8")
    text = render(config, results, routes, machine)
    (directory / "summary.md").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
