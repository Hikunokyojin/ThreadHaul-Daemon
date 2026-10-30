# Contributing to ThreadHaul

Thanks for your interest in improving ThreadHaul. Bug reports, ideas and pull requests are all welcome.

## Getting started

1. Install Visual Studio 2022 or later with the **Desktop development with C++** workload. It includes CMake.
2. Clone the repository and build:

   ```powershell
   cmake --preset windows-msvc
   cmake --build --preset release
   ctest --preset release
   ```

   Without CMake, `build.cmd test` builds everything with MSVC directly and runs the tests.
3. Try your change in console mode (no installation needed):

   ```powershell
   .\out\build\bin\Release\ThreadHaulDaemon.exe --console --allow-unelevated
   ```

## Pull requests

- Keep each pull request focused on one change.
- Add or update unit tests in `tests/` for any behaviour you change. The suites are plain C++ with no framework: a check is one `Check(condition, test, message)` call.
- The build must stay warning-free (`/W3`), and CI must pass.
- If you change a command, configuration key, log field or API endpoint, update `README.md` and add a line under **Unreleased** in `CHANGELOG.md`.
- Follow the existing style: C++17, the Win32 API, RAII for handles where practical, `snake_case` JSON field names, and no new third-party dependencies without discussion first.

## Code layout

| Folder | Contents |
|---|---|
| `src/actuator` | Per-process state machine; priority and thread control |
| `src/telemetry` | Process discovery, CPU/memory/I-O sampling, safety classification, predictive and auto-protect policies |
| `src/config` | `config.json` parsing and validation |
| `src/logging` | JSON Lines logger and Windows Event Log writer |
| `src/service` | Service core: poll loop, control pipe, dashboard server, alerts, coordination, auto-protect |
| `src/controller` | Central controller for multi-server deployments |
| `web/dashboard` | Dashboard front end (no build step) |
| `tests` | Unit tests |
| `tools` | Test workloads and the log viewer |
| `scripts` | Installer and uninstaller |

## Reporting security issues

Please don't open a public issue for security problems. See [SECURITY.md](SECURITY.md).
