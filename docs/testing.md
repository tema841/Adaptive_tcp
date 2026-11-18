# Testing

The CTest suite contains:

- SHA-256 known-answer tests for `abc` and the empty file.
- JSON escaping tests for quotes, backslashes, newlines, tabs, and control characters.
- A local socket-pair transfer test that resumes from a non-zero offset and checks
  that stale suffix bytes are truncated.
- A real end-to-end test that starts the server, performs PUT and GET through the
  compiled CLI client, and compares SHA-256 digests.
- A dashboard HTTP test that starts the server and verifies static asset delivery,
  browser-style uploads (small and 8 MiB), server-side SHA-256, corruption rejection,
  active pause/resume/cancel, delay impact, network profile restoration, and events.

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Sanitizers

Use separate build directories:

```bash
cmake -S . -B build-asan -DTCP_ENABLE_ASAN_UBSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DTCP_ENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

ThreadSanitizer support depends on the compiler and runtime platform. ASan/UBSan
and TSan are intentionally mutually exclusive.

## v5.2 adaptive browser upload

`tests/adaptive_upload.py` starts the server on temporary local ports and exercises the new chunk endpoint. It verifies that block sizes change under a configured delay, the stored file matches the source SHA-256, empty files are accepted, byte corruption is rejected, and cancellation removes the temporary upload file.

Additional v5.2 checks:

- `tests.cpp` verifies that cumulative byte and completion counters survive eviction from the 100-item transfer history.
- On POSIX test hosts, the socket-pair native transfer test applies a controlled per-frame delay and verifies that the sender grows beyond the 64 KiB initial block while preserving the SHA-256 digest.
- `tests/adaptive_upload.py` verifies adaptive browser block growth, replacement of an existing verified destination, zero-byte uploads, corruption rejection, cancellation cleanup, same-destination conflict rejection, and 32 concurrent status requests through the bounded pool.
- `tests/server_shutdown.py` verifies coordinated shutdown with no client, a partial HTTP request, and a long-lived SSE connection.
