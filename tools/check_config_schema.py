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
expect(edited(lambda c: c.update(metrics={"enabled": True, "token": "scrape-token-0123456789"})), True, "metrics with a token")
expect(edited(lambda c: c.update(
    identity={"dispatch_api.exe": {"path": r"C:\Apps\d.exe", "publisher": "Contoso"},
              "b.exe": {"path": r"\\server\share\b.exe"}},
    dashboard={"enabled": True, "port": 8787},
    alerts={"popup": True, "webhook_url": "dpapi:AAAA"})), True, "identity, UNC path, dpapi webhook, dashboard")

print(f"\n{failures} failure(s)")
sys.exit(1 if failures else 0)
