# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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

[Unreleased]: https://github.com/Hikunokyojin/ThreadHaul-Daemon/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Hikunokyojin/ThreadHaul-Daemon/releases/tag/v1.0.0
