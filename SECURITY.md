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

## Supported versions

Only the latest release receives security fixes.
