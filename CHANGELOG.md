# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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

[Unreleased]: https://github.com/Hikunokyojin/ThreadHaul-Daemon/compare/v2.0.0...HEAD
[2.0.0]: https://github.com/Hikunokyojin/ThreadHaul-Daemon/compare/v1.0.0...v2.0.0
[1.0.0]: https://github.com/Hikunokyojin/ThreadHaul-Daemon/releases/tag/v1.0.0
