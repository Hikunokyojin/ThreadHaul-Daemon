# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [2.1.0] - 2026-11-08

Expandability release. Existing configs keep working unchanged: every new section is optional, and the defaults reproduce 2.0.

### Added

- **Config schema.** `config/config.schema.json` (JSON Schema) gives editors autocomplete, descriptions and inline errors for every setting. The example config points at it with `$schema`, and CI checks it against the example and known-bad configs.
- **Versioned API.**
  - The dashboard API lives under `/api/v1`, and responses carry `API-Version: 1`.
  - `GET /api/v1/openapi.json` serves an OpenAPI 3.1 description, which CI validates.
  - The 2.0 paths (`/api/status` and so on) still work as aliases, answering with `Deprecation: true` and a `Link` to their v1 path.
- **Prometheus metrics.** `metrics.enabled` serves `/metrics` on the dashboard port, loopback only, with an optional bearer `metrics.token` (DPAPI allowed). It covers:
  - pressure, thresholds, per-instance critical CPU and batch states;
  - auto-protect, system CPU and memory, and the controller link;
  - counters of state changes and log entries;
  - signal, group and schedule figures.
- **Performance-counter signals.** `signals` adds pressure sources from Windows performance counters, read by English name:
  - built-in `gpu`, `disk_queue` and `network`;
  - a generic `counter` type for any counter, with sum/max over wildcard instances, an instance filter and a scale.

  No third-party code is loaded.
- **Configurable actions.** `actions` sets what each stage does, and recovery undoes exactly what was changed.
  - Throttle can combine Idle or Below Normal priority, low or very low I/O priority, confinement to N cores, and a Job Object hard CPU cap.
  - Suspend can freeze threads, or squeeze the process to a small CPU share so it stays alive.
- **Process matching rules.** Entries in `processes.critical` and `processes.batch` may be objects with `command_line_contains`, `user` and/or `service`, so instances of one executable (such as `java.exe` or `svchost.exe`) get different roles.
- **Policy groups.** `groups` defines independent policies, each with its own critical and batch processes and optional thresholds and actions. A group's batch processes respond only to its own critical processes.
- **Schedules.** `schedules` changes a group's thresholds or actions in local-time windows, including past-midnight windows. `mode: "off"` pauses throttling.
- **New log types:** `signal_sample`, `signal_error`, `action_error` and `schedule_change`. Samples and threshold events carry `group` when groups are configured.

### Fixed

- The dashboard's role editor let an out-of-range number in the config escape as an uncaught exception instead of reporting a config error.

## [2.0.0] - 2026-11-05

Security release. **Breaking:** the controller protocol has changed, so upgrade every agent and the controller together. The controller now requires `--token`, and `coordination.shared_token` must be at least 16 characters.

### Security

- **Encrypted, mutually authenticated controller link.**
  - Agent and controller prove to each other that they know the shared token, without sending it.
  - All traffic is then AES-256-GCM encrypted with per-direction keys.
  - Tampered, replayed and oversized messages are rejected.
  - A peer is locked out for 60 seconds after 5 failed handshakes.
- **Dashboard sessions.**
  - The access key expires after 30 minutes idle or 12 hours in total.
  - A **Sign out** button and `POST /api/logout` revoke it.
  - 10 bad keys within a minute lock the API for a minute (HTTP 429).
- **Tamper-evident log.**
  - Every JSON Lines entry carries `prev_hash`, the SHA-256 of the line before it.
  - `ThreadHaulDaemon.exe verify-log` reports the first edited, inserted or removed line, across rotated files.
- **Encrypted secrets.**
  - `coordination.shared_token` and `alerts.webhook_url` accept machine-bound `dpapi:` values.
  - `ThreadHaulDaemon.exe protect-secret` creates them from hidden input.
- **Process identity pinning.**
  - The new `identity` config section ties an executable name to a full path and/or a signing publisher.
  - Impostors get no role and are logged as `identity_mismatch`.
- **Virtual service account.** `install.ps1 -Account Virtual` runs the service as `NT SERVICE\ThreadHaulDaemon`.
- **`ThreadHaulDaemon.exe selfcheck`** audits an installation: config validity and permissions, plain-text secrets, controller link, dashboard exposure, unpinned critical processes, log permissions and integrity, and the service account.
- **Threat model** in [SECURITY.md](SECURITY.md#threat-model).
- **CI hardening.** Every change now gets fuzz tests of the config, log, secret and network parsers, plus MSVC `/analyze` (warnings are errors) and CodeQL scans.

### Fixed

- A config containing an out-of-range number (for example `1e309`) crashed the service and CLI instead of reporting a config error.
- A failing system random number generator now stops the process, instead of silently producing predictable keys, nonces or dashboard tokens.
- Unchecked hash, file-read and network-start results are now handled.

## [1.0.0] - 2026-10-30

First public release.

### Added

- **Windows Service** that watches critical processes once per second and protects them from CPU starvation:
  - batch processes are lowered to Idle priority after 3 samples at or above 85%;
  - they are suspended after 5 more samples at or above 90%;
  - they are restored after 5 samples below 60%.
- **Automatic CPU protection.** When total system CPU stays overloaded, the heaviest ordinary applications are lowered to Below Normal, then Idle, and later restored to their exact original priority. On by default.
- **Web dashboard** on `127.0.0.1:8787`:
  - live statistics and a five-minute chart;
  - a process picker with roles (critical, batch, protected);
  - control buttons;
  - a searchable, filterable activity log with raw-JSON view and CSV export.
- **Process safety levels.** Essential Windows processes are never managed. Other system processes and services need explicit confirmation, which the service enforces.
- **Predictive thresholds** (least-squares trend), plus memory and I/O triggers.
- **Live configuration reload** with full validation. An invalid edit is rejected and the last good configuration is kept.
- **Command-line override** over an administrator-only named pipe: `throttle`, `suspend`, `resume`, `release`, `exempt`, `status`, `dashboard`, `stop`.
- **Logging:**
  - structured JSON Lines log with size-based rotation;
  - Windows Event Log entries;
  - pop-up and webhook alerts;
  - self-monitoring.
- **Optional central controller** for multi-server deployments, with a cluster-wide threshold bias.
- **Tooling:**
  - installer and uninstaller scripts (least-privilege service, locked-down configuration);
  - test workloads;
  - a log-graphing script.
- **Builds:** CMake and `build.cmd`, with 231 unit-test checks, GitHub Actions CI and release packaging.

[Unreleased]: https://github.com/Hikunokyojin/ThreadHaul-Daemon/compare/v2.1.0...HEAD
[2.1.0]: https://github.com/Hikunokyojin/ThreadHaul-Daemon/compare/v2.0.0...v2.1.0
[2.0.0]: https://github.com/Hikunokyojin/ThreadHaul-Daemon/compare/v1.0.0...v2.0.0
[1.0.0]: https://github.com/Hikunokyojin/ThreadHaul-Daemon/releases/tag/v1.0.0
