"""Check config/config.schema.json against the example config and known-bad edits.

Used by CI (lint job). Run locally with:
    python -m pip install jsonschema
    python tools/check_config_schema.py
Exits non-zero if the schema is invalid, rejects the example, or accepts a
config the service would refuse.
"""
import copy
import json
import pathlib
import sys

import jsonschema

ROOT = pathlib.Path(__file__).resolve().parent.parent
schema = json.loads((ROOT / "config" / "config.schema.json").read_text(encoding="utf-8"))
example = json.loads((ROOT / "config" / "config.example.json").read_text(encoding="utf-8"))

jsonschema.Draft202012Validator.check_schema(schema)
validator = jsonschema.Draft202012Validator(schema)
failures = 0


def expect(config, valid, label):
    global failures
    errors = list(validator.iter_errors(config))
    ok = (not errors) == valid
    failures += not ok
    detail = "" if ok else f"  ({errors[0].message if errors else 'accepted'})"
    print(f"{'PASS' if ok else 'FAIL'}  {label}{detail}")


def edited(change):
    c = copy.deepcopy(example)
    change(c)
    return c


expect(example, True, "example config is valid")
expect(edited(lambda c: c["polling"].update(interval_ms=50)), False, "interval_ms below 100")
expect(edited(lambda c: c["processes"]["critical"].append(r"C:\Apps\a.exe")), False, "path in a process list")
expect(edited(lambda c: c["telemetry"].update(cpu_normalization="cores")), False, "unknown cpu_normalization")
expect(edited(lambda c: c.update(identity={"a.exe": {"path": r"rel\a.exe"}})), False, "relative identity path")
expect(edited(lambda c: c.update(identity={"notexe": {"path": r"C:\a.exe"}})), False, "identity key not an .exe")
expect(edited(lambda c: c["alerts"].update(webhook_url="ftp://host/hook")), False, "non-HTTP webhook")
expect(edited(lambda c: c["thresholds"]["high"].update(percent=0)), False, "threshold of 0%")
expect(edited(lambda c: c.pop("logging")), False, "missing logging section")
expect(edited(lambda c: c.update(metrics={"enabled": True, "token": "short"})), False, "metrics token under 16 chars")
expect(edited(lambda c: c.update(signals=[{"type": "gpu", "limit": 90}, {"type": "network", "limit": 100},
                                          {"type": "counter", "name": "sql_waits", "limit": 50,
                                           "counter": r"\SQLServer:Wait Statistics(*)\Waits in progress",
                                           "aggregate": "max"}])), True, "built-in and custom signals")
expect(edited(lambda c: c.update(signals=[{"type": "counter", "limit": 1}])), False, "counter signal without name/counter")
expect(edited(lambda c: c.update(signals=[{"type": "gpu", "limit": 1, "scale": 2}])), False, "scale on a built-in signal")
expect(edited(lambda c: c.update(signals=[{"type": "gpu"}])), False, "signal without a limit")
expect(edited(lambda c: c.update(actions={"throttle": {"priority": "below_normal", "io_priority": "very_low",
                                                       "affinity_cores": 2, "cpu_rate_percent": 25},
                                          "suspend": {"mode": "cpu_rate", "cpu_rate_percent": 2}})), True, "all actions")
expect(edited(lambda c: c.update(actions={"throttle": {"priority": "realtime"}})), False, "realtime priority")
expect(edited(lambda c: c["processes"].update(
    critical=["a.exe", {"name": "java.exe", "command_line_contains": "api.jar"}],
    batch=[{"name": "java.exe", "user": "svc"}, {"name": "svchost.exe", "service": "wuauserv"}])), True, "match rules")
expect(edited(lambda c: c["processes"].update(batch=[{"name": "java.exe"}])), False, "rule without a condition")
expect(edited(lambda c: c.update(groups=[{"name": "reports", "critical": ["sqlservr.exe"], "batch": ["etl.exe"],
                                          "thresholds": {"high": {"percent": 95, "consecutive_samples": 5}},
                                          "actions": {"suspend": {"mode": "cpu_rate"}}}])), True, "a group")
expect(edited(lambda c: c.update(groups=[{"name": "default", "critical": ["a.exe"], "batch": ["b.exe"]}])), False,
       "group named default")
expect(edited(lambda c: c.update(groups=[{"name": "g", "critical": [], "batch": ["b.exe"]}])), False, "group without critical")
expect(edited(lambda c: c.update(schedules=[
    {"name": "business", "days": ["mon", "fri"], "from": "08:00", "to": "18:00",
     "thresholds": {"high": {"percent": 70, "consecutive_samples": 3}}},
    {"name": "nightly", "days": ["mon"], "from": "22:00", "to": "06:00", "mode": "off"},
    {"name": "allday", "days": ["sat"], "from": "00:00", "to": "24:00", "actions": {"suspend": {"mode": "cpu_rate"}}}])),
    True, "schedules")
expect(edited(lambda c: c.update(schedules=[{"name": "s", "days": ["mon"], "from": "08:00", "to": "09:00"}])), False,
       "schedule that changes nothing")
expect(edited(lambda c: c.update(schedules=[{"name": "s", "days": ["monday"], "from": "08:00", "to": "09:00",
                                             "mode": "off"}])), False, "bad weekday")
expect(edited(lambda c: c.update(schedules=[{"name": "s", "days": ["mon"], "from": "24:00", "to": "09:00",
                                             "mode": "off"}])), False, "from 24:00")
expect(edited(lambda c: c["processes"].update(protected=[{"name": "a.exe", "user": "x"}])), False, "rule in protected")
expect(edited(lambda c: c.update(actions={"suspend": {"mode": "kill"}})), False, "unknown suspend mode")
expect(edited(lambda c: c.update(metrics={"enabled": True, "token": "scrape-token-0123456789"})), True, "metrics with a token")
expect(edited(lambda c: c.update(
    identity={"dispatch_api.exe": {"path": r"C:\Apps\d.exe", "publisher": "Contoso"},
              "b.exe": {"path": r"\\server\share\b.exe"}},
    dashboard={"enabled": True, "port": 8787},
    alerts={"popup": True, "webhook_url": "dpapi:AAAA"})), True, "identity, UNC path, dpapi webhook, dashboard")

print(f"\n{failures} failure(s)")
sys.exit(1 if failures else 0)
