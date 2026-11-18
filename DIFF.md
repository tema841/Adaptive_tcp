# Adaptive TCP v5.2.0 change map

- Preserves the existing C++ TCP GET/PUT protocol and Windows 10/11 target.
- Supports both MSVC and MinGW-w64/GCC (including MSYS2 UCRT64) through CMake; MSVC is not mandatory.
- Browser upload now uses adaptive HTTP/TCP application chunks from 4 KiB to 4 MiB and records actual server-side block sizes.
- Native TCP sender uses stable-window probing; the receiver reports actual received frame sizes.
- Adds cumulative byte/completion counters independent of the bounded 100-transfer history.
- Adds destination reservations, per-transfer HTTP temporary files, SHA-256 validation and replace-style publication.
- Replaces detached connection threads with a bounded 16-worker pool and a 256-task queue.
- Adds coordinated shutdown and tests for idle, partial HTTP and SSE connections.
- Removes the oversized dashboard hero and System Profile; makes telemetry horizontally scrollable and transfer/event/integrity areas vertically scrollable with themed scrollbars.
- Standardizes all four clipped corners with shared CSS variables and one eight-point geometry.
- Adds adaptive upload, empty-file, corruption, cancellation, destination-conflict, concurrent-request and graceful-shutdown regression tests.

## Verified in the preparation environment

- GCC C++17 server/client/test builds with `-Wall -Wextra -Wpedantic`: passed without compiler warnings in the final build.
- Core unit tests, including cumulative counters and native adaptive chunk growth: passed.
- TCP GET/PUT end-to-end integration: passed.
- HTTP dashboard integration: passed.
- Adaptive browser upload: block-size changes, SHA-256, overwrite, empty file, corruption rejection and cancellation cleanup: passed.
- Bounded worker pool: 32 concurrent status requests and same-destination conflict test: passed.
- Graceful shutdown: idle, partial HTTP and SSE connections: passed.
- JavaScript syntax, CSS parser and static HTML ID-reference checks: passed.

## Not verified on the target Windows machine

The MSYS2 UCRT64/MinGW build and browser behavior on Windows 10/11 still need to be run on the target machine. A browser-based visual test could not be completed in the preparation environment because Chromium navigation was blocked by its administrator policy. The responsive CSS and static markup were checked, but this is not a substitute for a real Windows browser run.

## Remaining engineering limitations

TLS/authentication, complete elimination of filesystem TOCTOU races, durable browser-upload resume manifests, real RTT measurement, packet-loss emulation, disk/user quotas, and long-duration multi-client stress/fuzz testing are not implemented. The worker pool has been exercised with concurrent status requests, not with dozens of simultaneous large transfers.
