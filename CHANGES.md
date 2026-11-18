# Changes — Adaptive TCP v5.1.0

- Windows 10/11 x64 is now the only supported build target.
- CMake rejects non-Windows and non-MSVC builds.
- CI now builds and tests on Windows only.
- Removed Docker deployment files.
- Added timestamped sample data to the dashboard status response.
- Dashboard charts include retained samples from completed transfers.
- Transfer list is scrollable and long names wrap safely.
- URL-encoded file names are decoded for display when possible.
- Added a dedicated Network Lab action to restore normal network settings.
- Dashboard control actions now check HTTP errors before refreshing.

See README.md for verified behavior and remaining limitations.


## Build compatibility correction
- Removed the accidental MSVC-only compiler restriction. Windows builds now accept MSVC and MinGW-w64/GCC (including MSYS2 UCRT64).
- Documented the Ninja + GCC build commands used by the MSYS2 UCRT64 environment.


## v5 browser upload/control fixes
- Browser uploads are registered as active transfers and support Pause / Resume / Cancel.
- Browser upload SHA-256 is computed incrementally in the browser and verified by the server before publishing.
- Delay and jitter are applied during browser uploads and native TCP transfers; jitter no longer deliberately drops the connection.
- Byte corruption is detected as a checksum failure; corrupted content is not published as the destination file.


## v5.1 browser transfer lifecycle
- Browser upload sessions are registered before SHA-256 preflight, so the dashboard shows them immediately.
- Pause/resume/cancel are polled during browser-side hashing and honored by the server during body transfer.
- Network delay/jitter is explicitly applied to active body transfer, not the local preflight hash calculation.
- The upload request reuses the pre-registered transfer ID so dashboard controls address the correct session.


## v5.1.0 dashboard redesign
- Rebuilt the dashboard layout and visual system without replacing the existing API or transfer flow.
- Added the supplied paint-stroke background as `web/assets/paint-strokes.png`.
- Standardized panel outlines, black surfaces, diagonal corners, typography, and cyan/cream status colors.
- Reworked responsive layout, telemetry charts, transfer list, Network Lab, event log, and SHA-256 panel.
- Added upload throughput display and server version/online state.
- Added correct MIME types for PNG/JPEG/WebP/SVG assets served by the embedded HTTP server.
- The corruption endpoint now reports HTTP 409 when no active transfer is available.
- Added a dashboard HTTP integration test covering small/large browser uploads, checksum verification, corruption, pause/resume/cancel, delay impact, and event logging.
- Packet-loss probability remains unsupported by the existing Network Lab API and is explicitly disclosed in the UI and README.


# v5.2.0 — adaptive browser transfer and dashboard refinement
- Replaced the dashboard's fixed 64 KiB single-body upload path with adaptive HTTP/TCP chunk requests (4 KiB–4 MiB).
- Added server-side chunk offset validation, per-transfer temporary files, actual chunk-size telemetry, server-side SHA-256 verification and verified-file publication.
- Added support for empty browser uploads and cleanup of temporary files on cancellation.
- Updated the browser and native TCP sender adaptation strategy with stable-window probing and latency-aware response to slow chunk windows.
- Receiver-side TCP telemetry now reports the actual received frame size rather than a locally guessed next size.
- Added explicit paused/cancelling transfer states and state-specific controls.
- Made transferred-byte and completed-transfer counters cumulative and independent of the 100-record transfer history.
- Removed the oversized hero dashboard and System Profile block; added scrollable telemetry timeline, three-record-height integrity viewport and themed scrollbars.
- Standardized all four clipped panel corners to the same eight-point polygon and shared cut/stroke variables.
- Removed the unmeasured RTT value from the transfer list rather than presenting a misleading zero.
- Added adaptive browser upload integration tests for chunk-size changes, empty files, corruption rejection and cancellation cleanup.
- Replaced delete-then-rename file publication with platform-specific replace semantics (MoveFileExW on Windows, atomic rename on POSIX).

- Replaced detached per-connection threads with a bounded 16-worker connection pool and a 256-task queue.
- Added coordinated shutdown tests for idle, partial HTTP and SSE connections; the server now shuts down listeners and tracked sockets before joining workers.
- Added per-destination write reservations shared by HTTP and native TCP uploads to reject concurrent writes to the same target.
- Added regression tests for destination conflicts and concurrent status requests.
- Cleaned socket send/receive routines and eliminated the prior misleading-indentation/unused-parameter compiler warnings in the touched build units.

- Added a CSV export endpoint and dashboard action for real transfer and telemetry samples, suitable for coursework experiment analysis.
