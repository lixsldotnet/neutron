#!/usr/bin/env python3
"""neutron bench helper for dev/bench/run.sh (stdlib only).

  bench.py wrap <group> [unit] [keys] < output
                                           turns a bench program's JSON line into a raw record
                                           (keys: comma list, keep only these values)
  bench.py summarize <raw.jsonl>           median of the raw runs -> one result object
  bench.py watch <log> <offset> <pid> <warmup_lines> <measure_lines> <first_timeout_s> <proc_regex>
                                           follows a game log, measures FPS lines and the CPU
                                           time of the game process (pgrep -f proc_regex)
  bench.py baseline <result.json>...       merges full-suite results into a baseline
                                           with a noise estimate per metric
  bench.py compare <baseline.json> <result.json>   table of changes against a baseline

A result is {"meta": {...}, "metrics": {name: value}, "spread_pct": {name: pct}}.
Metric names carry their unit as suffix (_ns, _ms, _s, fps, _cores); everything
but fps is lower-is-better.
"""
import json
import re
import statistics
import sys
import time


def unit_of(name):
    if name.endswith("fps"):
        return "fps"
    if name.endswith("_cores"):
        return "cores"
    for suffix, unit in (("_ns", "ns"), ("_ms", "ms"), ("_fps", "fps"), ("_s", "s")):
        if name.endswith(suffix):
            return unit
    return ""


def higher_is_better(name):
    return name.endswith("fps")


NOT_METRICS = {"bench", "draws", "frames", "lines", "error"}


def wrap(group, unit, keys):
    """Last JSON line of a bench program (stdin) -> {"group", "values"} or {"group", "skipped"}"""
    obj = None
    for line in sys.stdin:
        line = line.strip()
        if line.startswith("{"):
            try:
                obj = json.loads(line)
            except ValueError:
                pass
    if obj is None:
        print(json.dumps({"group": group, "skipped": "no result line"}))
        return 1
    values = obj.get(unit, obj) if unit else obj
    values = {k: v for k, v in values.items() if k not in NOT_METRICS and isinstance(v, (int, float))}
    if keys:
        values = {k: v for k, v in values.items() if k in keys.split(",")}
    if unit:
        values = {(k if unit_of(k) else f"{k}_{unit}"): v for k, v in values.items()}
    rec = {"group": group, "values": values}
    if "error" in obj:
        rec["skipped"] = obj["error"]  # failed run; values holds what was measured before
    print(json.dumps(rec))
    return 0


def spread(values):
    med = statistics.median(values)
    return 0.0 if len(values) < 2 or med == 0 else (max(values) - min(values)) / abs(med) * 100


def summarize(raw_path):
    """raw lines: {"meta": {...}} or {"group": g, "values": {k: v}, "skipped": why (optional)}"""
    samples, meta, skipped = {}, {}, {}
    with open(raw_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            if "meta" in rec:
                meta.update(rec["meta"])
                continue
            group = rec["group"]
            if "skipped" in rec:
                skipped[group] = rec["skipped"]
            for key, value in rec.get("values", {}).items():
                if isinstance(value, (int, float)):
                    samples.setdefault(f"{group}.{key}", []).append(float(value))
    metrics = {k: round(statistics.median(v), 3) for k, v in samples.items()}
    out = {
        "meta": meta,
        "metrics": metrics,
        "spread_pct": {k: round(spread(v), 1) for k, v in samples.items()},
        "samples": {k: len(v) for k, v in samples.items()},
    }
    if skipped:
        out["skipped"] = skipped
    print(json.dumps(out, sort_keys=True))


FPS_RE = re.compile(r"neutron: fps ([0-9.]+) worst ([0-9.]+)ms")


def pid_alive(pid):
    import os
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def cpu_seconds(pattern):
    """summed CPU time (user+sys) of the processes matching pattern, from ps"""
    import subprocess
    pids = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True).stdout.split()
    if not pids:
        return None
    out = subprocess.run(["ps", "-o", "cputime=", "-p", ",".join(pids)], capture_output=True, text=True).stdout
    total = 0.0
    for field in out.split():
        secs = 0.0
        for part in field.split(":"):
            secs = secs * 60 + float(part)
        total += secs
    return total


def fail_partial(why, times, proc_pattern):
    """error result that keeps what was measured, plus how busy the game is now
    (about 0 means it hangs, otherwise it is still working, e.g. loading)"""
    result = {"error": why}
    if times:
        result["first_fps_s"] = round(times[0], 1)
    c0 = cpu_seconds(proc_pattern)
    time.sleep(5)
    c1 = cpu_seconds(proc_pattern)
    if c0 is not None and c1 is not None:
        result["error"] += f", game process busy {(c1 - c0) / 5:.2f} cores"
    print(json.dumps(result))
    return 1


def watch(log, offset, pid, warmup, measure, first_timeout, proc_pattern, stall_timeout=150.0):
    """Follows the log from byte offset. The DXMT FPS log writes one line per second
    of rendering: the first `warmup` lines are skipped, the next `measure` lines are
    taken. Counting lines instead of wall time keeps loading stalls (no frames, no
    lines) out of both windows; a gap over stall_timeout ends the run as failed. The CPU time of the game process over the measure
    window gives the CPU cost per frame, which still shows gains when the FPS sits
    at the display refresh."""
    warmup, measure = int(warmup), int(measure)
    start = time.monotonic()
    deadline = start + first_timeout + 4 * (warmup + measure) + 120
    times, fps, worst = [], [], []
    cpu0 = cpu1 = t_cpu0 = t_cpu1 = None
    buf = b""
    pos = offset
    while True:
        with open(log, "rb") as f:
            f.seek(pos)
            chunk = f.read()
        pos += len(chunk)
        buf += chunk
        lines = buf.split(b"\n")
        buf = lines.pop()
        now = time.monotonic()
        for raw in lines:
            m = FPS_RE.search(raw.decode("utf-8", "replace"))
            if not m:
                continue
            times.append(now - start)
            if len(times) > warmup:
                fps.append(float(m.group(1)))
                worst.append(float(m.group(2)))
        if cpu0 is None and len(times) >= warmup:
            cpu0, t_cpu0 = cpu_seconds(proc_pattern), now
        if len(times) >= warmup + measure:
            cpu1, t_cpu1 = cpu_seconds(proc_pattern), now
            break
        if not times and now - start > first_timeout:
            print(json.dumps({"error": f"no FPS line after {first_timeout:.0f} s"}))
            return 1
        if now > deadline:
            return fail_partial(f"only {len(times)} FPS lines before the timeout", times, proc_pattern)
        if times and now - start - times[-1] > stall_timeout:
            return fail_partial(f"no frame for {stall_timeout:.0f} s after {len(times)} FPS lines",
                                times, proc_pattern)
        if not pid_alive(pid):
            return fail_partial("game exited", times, proc_pattern)
        time.sleep(0.2)
    # menu_s: start of the continuous run of FPS lines (gaps under 3 s) that the
    # measurement is part of, i.e. launch until the game renders steadily
    menu = times[0]
    for i in range(1, warmup + 1):
        if times[i] - times[i - 1] > 3:
            menu = times[i]
    result = {
        "first_fps_s": round(times[0], 1),
        "menu_s": round(menu, 1),
        "fps": round(statistics.median(fps), 1),
        "wall_ms_per_frame": round(1000 / statistics.median(fps), 3),
        "worst_ms": round(statistics.median(worst), 1),
        "lines": len(fps),
    }
    if cpu0 is not None and cpu1 is not None and cpu1 > cpu0:
        wall = t_cpu1 - t_cpu0
        frames = statistics.mean(fps) * wall
        result["cpu_per_frame_ms"] = round((cpu1 - cpu0) * 1000 / frames, 3)
        result["busy_cores"] = round((cpu1 - cpu0) / wall, 2)
    print(json.dumps(result))
    return 0


def baseline(paths):
    runs = []
    for p in paths:
        with open(p) as f:
            runs.append(json.load(f))
    names = sorted(set().union(*(r["metrics"] for r in runs)))
    out = {"meta": {"runs": len(runs), "sources": [r.get("meta", {}) for r in runs]}, "metrics": {}}
    for name in names:
        vals = [r["metrics"][name] for r in runs if name in r["metrics"]]
        med = statistics.median(vals)
        between = spread(vals) if len(vals) > 1 else None
        within = statistics.mean(r.get("spread_pct", {}).get(name, 0.0) for r in runs if name in r["metrics"])
        # noise: run-to-run difference, at least half the spread inside one run, at least 0.5%
        candidates = [within / 2, 0.5] + ([between] if between is not None else [])
        out["metrics"][name] = {
            "value": round(med, 3),
            "unit": unit_of(name),
            "higher_is_better": higher_is_better(name),
            "noise_pct": round(max(candidates), 1),
            "runs": len(vals),
        }
    print(json.dumps(out, indent=1, sort_keys=True))


def compare(base_path, result_path):
    with open(base_path) as f:
        base = json.load(f)["metrics"]
    with open(result_path) as f:
        res = json.load(f)
    rows = []
    for name, value in sorted(res["metrics"].items()):
        b = base.get(name)
        if not b:
            rows.append((name, "", f"{value:g}", "", "new"))
            continue
        delta = (value - b["value"]) / b["value"] * 100 if b["value"] else 0.0
        better = delta > 0 if b["higher_is_better"] else delta < 0
        verdict = "" if abs(delta) <= 2 * b["noise_pct"] else ("better" if better else "WORSE")
        rows.append((name, f"{b['value']:g}", f"{value:g}", f"{delta:+.1f}% (noise {b['noise_pct']}%)", verdict))
    width = max(len(r[0]) for r in rows) if rows else 10
    print(f"{'metric':<{width}}  {'baseline':>12}  {'now':>12}  change")
    for name, b, v, d, verdict in rows:
        print(f"{name:<{width}}  {b:>12}  {v:>12}  {d} {verdict}".rstrip())
    for group, why in res.get("skipped", {}).items():
        print(f"{group}: skipped ({why})")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "wrap":
        return wrap(argv[2], argv[3] if len(argv) > 3 else "", argv[4] if len(argv) > 4 else "")
    if cmd == "summarize":
        summarize(argv[2])
    elif cmd == "watch":
        log, offset, pid = argv[2], int(argv[3]), int(argv[4])
        warmup, measure, first_timeout = (float(x) for x in argv[5:8])
        return watch(log, offset, pid, warmup, measure, first_timeout, argv[8])
    elif cmd == "baseline":
        baseline(argv[2:])
    elif cmd == "compare":
        compare(argv[2], argv[3])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
