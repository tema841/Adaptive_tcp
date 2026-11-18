# Verification report — Adaptive TCP v5.2.0

## Builds performed

The server, client and unit-test executable were compiled in the preparation environment with GCC C++17 using `-O2 -Wall -Wextra -Wpedantic -pthread`. The final build completed without compiler warnings.

The CMake project intentionally rejects non-Windows platforms. The preparation environment is Linux and has no MinGW-w64 cross-compiler, so the requested MSYS2 UCRT64/Windows CMake build was not run. It must still be verified on Windows 10/11.

## Tests passed

- Core SHA-256, transfer/resume and metrics unit tests.
- Native TCP sender adaptive block growth under controlled per-frame delay.
- TCP GET/PUT end-to-end integration.
- Dashboard API and static PNG asset delivery.
- Legacy HTTP upload compatibility, server-side SHA-256 and stored-file digest.
- Browser adaptive upload: actual chunk-size changes, final SHA-256, replacement of an existing destination, zero-byte file, corruption rejection and cancellation cleanup.
- Network delay effect and restore-normal profile.
- Pause/resume/cancel behavior in the HTTP integration suite.
- Destination reservation conflict rejection.
- 32 concurrent HTTP status requests through the bounded worker pool.
- Graceful shutdown with idle, partial HTTP and long-lived SSE connections.
- CSV export of retained telemetry samples.
- JavaScript syntax check, CSS parser and static HTML ID-reference checks.

## Not claimed as verified

- Windows MSYS2 UCRT64/MinGW build and execution.
- Visual behavior in a real Windows browser at desktop and mobile sizes. Chromium navigation was blocked by the administrator policy in the preparation environment; static HTML/CSS validation was completed instead.
- Stress testing with dozens of simultaneous large file transfers or multi-hour runs.
- True RTT measurement, packet-loss emulation, TLS/authentication, disk quotas and durable browser-upload resume across restarts.
