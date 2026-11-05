# Security policy

ThreadHaul runs as a privileged Windows service and can change the priority of, and suspend, other processes. Security reports are taken seriously.

## Reporting a vulnerability

Please report vulnerabilities **privately** through GitHub: go to the **Security** tab, then **Report a vulnerability**, or use this link:
<https://github.com/Hikunokyojin/ThreadHaul-Daemon/security/advisories/new>.

Please include the version, your Windows version, the steps to reproduce, and the impact you observed. You should get an acknowledgement within a week.

## Scope

In scope:
- the service;
- the control pipe;
- the dashboard HTTP server and its API;
- the controller protocol;
- the installer scripts.

The [README](README.md#security-model) describes the security model, including the dashboard's loopback-only binding, per-session access key, Host and Origin checks, and the administrator-only control pipe.

## Threat model

ThreadHaul is a privileged service that can lower the priority of, and suspend, other processes. The main risks are someone misusing that power, and someone tampering with what it records.

**Trusted:** local Administrators and SYSTEM. Anyone in these groups can already control every process, so ThreadHaul does not try to defend against them.

**Not trusted:** ordinary local users, other machines on the network, programs running on the machine, and anything read from disk or the network.

| Threat | Defence | Known limit |
|---|---|---|
| A network attacker reads or forges controller traffic, or replays commands | Pre-shared-key mutual authentication, AES-256-GCM per direction, sequence-number nonces, 1 MiB frame cap, 60 s lockout after 5 failed handshakes | Security depends on the shared token: use a long random value, at least 16 characters (24+ for a remote controller) |
| A web page or local user drives the dashboard | Binds to 127.0.0.1 only; Host and Origin checks block DNS rebinding and cross-site requests; per-session key that expires after 30 min idle or 12 h; sign-out; lockout after 10 bad keys in a minute | Anyone who copies a live dashboard link can use it until it expires |
| A local user changes the config | Installer restricts writes to SYSTEM and Administrators; the file's SHA-256 is logged on every load; with `security.require_protected_config` on, the service refuses a config that ordinary users can write | — |
| A copied config or backup leaks secrets | `shared_token` and `webhook_url` can be stored DPAPI-encrypted (`protect-secret`), bound to this machine | Anyone who can run code as an Administrator on this machine can decrypt them |
| A renamed or copied program claims a critical or batch role | The `identity` section pins names to a full path and/or Authenticode publisher; impostors get no role and are logged as `identity_mismatch` | Opt-in. Catalog-signed files (most Windows components) have no embedded signature, so pin them by path |
| Someone edits the activity log to hide what happened | Every line is hash-chained to the line before it; `verify-log` reports the first edited, inserted or removed line | The chain has no key: an Administrator can rebuild it, and cutting off the newest lines cannot be detected from the file alone. Forward the Windows Event Log to a central collector for stronger guarantees |
| A malformed config or network message crashes the service | Parsers return errors instead of throwing; fuzz tests on every CI run; MSVC `/analyze` and CodeQL scans | — |
| A bug in ThreadHaul is exploited | Privileges are reduced to SeDebugPrivilege and SeChangeNotifyPrivilege; optional virtual service account (`install.ps1 -Account Virtual`); essential Windows processes can never be suspended | Under LocalSystem with SeDebugPrivilege, a compromise still reaches every process |

**Out of scope:** physical access, a compromised Administrator account, kernel-level malware, and denial of service by a local Administrator.

## Checking an installation

Run this from an Administrator PowerShell:

```powershell
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" selfcheck
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" verify-log
```

`selfcheck` reports PASS, WARN or FAIL for each of these:
- the config is valid, and only administrators can change it;
- secrets are encrypted;
- the controller link is encrypted and its token is strong enough;
- the dashboard is exposed only locally;
- critical processes are pinned to an identity;
- the log folder is protected and the log chain is intact;
- which account the service runs as.

It exits with 1 if any check fails.

## Supported versions

Only the latest release receives security fixes.
