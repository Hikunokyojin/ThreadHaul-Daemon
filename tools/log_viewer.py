#!/usr/bin/env python3
"""Render a ThreadHaulDaemon JSON Lines log as graphs.

Reads the daemon log (and its rotated .1, .2 ... files) and draws:
  1. CPU% / pressure of each critical process, with the throttle, escalate
     and recovery thresholds (including adaptive adjustments) as step lines;
  2. the state of each batch process over time (normal / throttled /
     suspended), with manual overrides marked;
  3. the daemon's own CPU% and working set (self-monitoring).
It also prints a text summary of every state change.

Usage:
  python log_viewer.py <threadhaul.jsonl> [--out graph.png] [--last-minutes N]
  python log_viewer.py C:\\ProgramData\\ThreadHaulDaemon\\logs\\threadhaul.jsonl --out run.png

Requires matplotlib (pip install matplotlib).
"""

import argparse
import glob
import json
import os
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone

STATE_LEVEL = {"normal": 0, "throttled": 1, "suspended": 2}


def parse_ts(s):
    return datetime.strptime(s, "%Y-%m-%dT%H:%M:%S.%fZ").replace(tzinfo=timezone.utc)


def read_entries(path):
    files = [path] + sorted(glob.glob(path + ".*"), key=lambda p: -int(p.rsplit(".", 1)[1])
                            if p.rsplit(".", 1)[1].isdigit() else 0)
    entries = []
    for f in files:
        if not os.path.isfile(f):
            continue
        with open(f, encoding="utf-8", errors="replace") as fh:
            for n, line in enumerate(fh, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    e = json.loads(line)
                    e["_t"] = parse_ts(e["timestamp"])
                    entries.append(e)
                except (ValueError, KeyError):
                    print(f"skipping malformed line {f}:{n}", file=sys.stderr)
    entries.sort(key=lambda e: e["_t"])
    return entries


def summarize(entries):
    counts = defaultdict(int)
    for e in entries:
        counts[e["log_type"]] += 1
    print("Entries by type: " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    print()
    print(f"{'time (UTC)':<26}{'batch process':<32}{'transition':<26}{'cpu%':>7}  trigger")
    for e in entries:
        if e["log_type"] == "actuator_action":
            name = f"{e['batch_process_name']} ({e['batch_process_id']})"
            trans = f"{e['from_state']} -> {e['to_state']}"
            ok = "" if e.get("actuation_succeeded", True) else "  [OS call failed]"
            print(f"{e['timestamp']:<26}{name:<32}{trans:<26}{e['cpu_percent']:>7.1f}  "
                  f"{e.get('trigger', 'automatic')}{ok}")
    selfmon = [e for e in entries if e["log_type"] == "self_monitor"]
    if selfmon:
        cpu = [e["cpu_percent"] for e in selfmon]
        ws = [e["working_set_mb"] for e in selfmon]
        print(f"\nDaemon footprint: CPU avg {sum(cpu) / len(cpu):.2f}% max {max(cpu):.1f}%, "
              f"working set max {max(ws):.1f} MB over {len(selfmon)} samples")


def plot(entries, out):
    import matplotlib
    if out:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates

    t_end = entries[-1]["_t"]
    fig, axes = plt.subplots(3, 1, figsize=(12, 9), sharex=True,
                             gridspec_kw={"height_ratios": [3, 2, 1.3]})
    ax_cpu, ax_state, ax_self = axes

    # 1. Critical-process CPU / pressure.
    series = defaultdict(lambda: ([], [], []))
    for e in entries:
        if e["log_type"] == "sample":
            key = f"{e.get('process_name', 'pid')} ({e['process_id']})"
            ts, cpu, pressure = series[key]
            ts.append(e["_t"])
            cpu.append(e["cpu_percent"])
            pressure.append(e.get("pressure_percent", e["cpu_percent"]))
    for key, (ts, cpu, pressure) in series.items():
        ax_cpu.plot(ts, cpu, lw=1.6, label=f"{key} CPU%")
        if any(abs(p - c) > 0.05 for p, c in zip(pressure, cpu)):
            ax_cpu.plot(ts, pressure, lw=1, ls=":", label=f"{key} pressure%")

    # Threshold step lines from config reloads and adaptive adjustments.
    thr = {"high": [], "critical": [], "low": []}
    current = {"high": 85.0, "critical": 90.0, "low": 60.0}
    t0 = entries[0]["_t"]
    for k in thr:
        thr[k].append((t0, current[k]))
    for e in entries:
        if (e["log_type"] == "config_reload" and e.get("success")) or e["log_type"] == "adaptive_threshold":
            for k in thr:
                v = e.get(f"{k}_percent")
                if v is not None:
                    thr[k].append((e["_t"], v))
    styles = {"high": ("tab:orange", "throttle"), "critical": ("tab:red", "escalate"),
              "low": ("tab:green", "recover")}
    for k, pts in thr.items():
        pts.append((t_end, pts[-1][1]))
        color, label = styles[k]
        ax_cpu.step([p[0] for p in pts], [p[1] for p in pts], where="post", color=color,
                    lw=1, ls="--", label=f"{label} threshold")
    for e in entries:
        if e["log_type"] == "process_lifecycle" and e.get("role") == "critical" and \
                e["event"] in ("crashed", "restarted", "exited"):
            ax_cpu.axvline(e["_t"], color="grey", lw=0.8, ls="-.")
            ax_cpu.annotate(e["event"], (e["_t"], 0.03), xycoords=ax_cpu.get_xaxis_transform(),
                            fontsize=7, rotation=90, color="grey")
    ax_cpu.set_ylabel("CPU % (per config normalisation)")
    ax_cpu.set_title("Critical processes")
    ax_cpu.legend(loc="upper left", fontsize=7, ncol=2)
    ax_cpu.grid(alpha=0.3)

    # 2. Batch-process state timeline.
    states = defaultdict(list)
    for e in entries:
        if e["log_type"] == "actuator_action":
            key = f"{e['batch_process_name']} ({e['batch_process_id']})"
            if not states[key]:
                states[key].append((e["_t"], STATE_LEVEL[e["from_state"]], "automatic"))
            states[key].append((e["_t"], STATE_LEVEL[e["to_state"]], e.get("trigger", "automatic")))
    for i, (key, pts) in enumerate(states.items()):
        offset = i * 0.06
        xs = [p[0] for p in pts] + [t_end]
        ys = [p[1] + offset for p in pts] + [pts[-1][1] + offset]
        line, = ax_state.step(xs, ys, where="post", lw=1.8, label=key)
        manual = [p for p in pts if p[2] == "manual"]
        if manual:
            ax_state.scatter([p[0] for p in manual], [p[1] + offset for p in manual], marker="v",
                             color=line.get_color(), zorder=3)
    ax_state.set_yticks([0, 1, 2])
    ax_state.set_yticklabels(["normal", "throttled", "suspended"])
    ax_state.set_ylim(-0.4, 2.6)
    ax_state.set_title("Batch processes (triangles = manual override)")
    if states:
        ax_state.legend(loc="upper left", fontsize=7)
    ax_state.grid(alpha=0.3)

    # 3. Self-monitoring.
    selfmon = [e for e in entries if e["log_type"] == "self_monitor"]
    if selfmon:
        ts = [e["_t"] for e in selfmon]
        ax_self.plot(ts, [e["cpu_percent"] for e in selfmon], marker="o", ms=3, label="daemon CPU%")
        ax2 = ax_self.twinx()
        ax2.plot(ts, [e["working_set_mb"] for e in selfmon], color="tab:purple", marker="s", ms=3,
                 label="working set MB")
        ax2.set_ylabel("MB")
        ax_self.legend(loc="upper left", fontsize=7)
        ax2.legend(loc="upper right", fontsize=7)
    ax_self.set_ylabel("CPU %")
    ax_self.set_title("ThreadHaulDaemon self-monitoring")
    ax_self.grid(alpha=0.3)
    ax_self.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))
    ax_self.set_xlabel("time (UTC)")

    fig.tight_layout()
    if out:
        fig.savefig(out, dpi=120)
        print(f"\nGraph written to {out}")
    else:
        plt.show()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="path to threadhaul.jsonl")
    ap.add_argument("--out", help="write a PNG instead of opening a window")
    ap.add_argument("--last-minutes", type=float, help="only show the most recent N minutes")
    args = ap.parse_args()

    entries = read_entries(args.log)
    if not entries:
        sys.exit(f"no log entries found in {args.log}")
    if args.last_minutes:
        cutoff = entries[-1]["_t"] - timedelta(minutes=args.last_minutes)
        entries = [e for e in entries if e["_t"] >= cutoff]
    summarize(entries)
    plot(entries, args.out)


if __name__ == "__main__":
    main()
