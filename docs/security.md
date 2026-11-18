# Security notes (v3)

## Implemented in this revision

- The server binds both listeners to `127.0.0.1` by default. Exposing either listener
  requires an explicit `-bind` / `-web-bind` option.
- Mutating HTTP API requests (`POST /api/...`) are restricted to IPv4 loopback.
  The dashboard can still be used for read-only status when deliberately exposed.
- Storage and static-web paths reject absolute paths, `.` / `..` components,
  existing symbolic-link components, and canonical paths outside the configured root.
- Numeric protocol fields reject signs and malformed values.
- PUT requests require a 64-character lowercase hexadecimal SHA-256 digest.
- The receiver rejects offsets that exceed the partial-file length and truncates
  stale suffix bytes when resuming from an earlier valid offset.
- Client success now requires the exact server acknowledgement (`VERIFY OK` for
  GET and `DONE <sha256>` for PUT).
- The Windows deployment image runs as an unprivileged user and enables `no-new-privileges`.

## Important limitations — do not expose this as an internet-facing service

This is still an educational TCP application, not a hardened production file server.

- The transfer protocol does **not** provide authentication or encryption. Do not
  expose the data listener to untrusted networks. Use a trusted private network
  or a VPN/tunnel until TLS and authenticated sessions are implemented.
- The HTTP dashboard has no user accounts. Mutating endpoints are limited to
  loopback, but anyone able to access the host locally can control demo transfers.
- Path validation rejects symlinks but still uses path-based filesystem operations.
  A hostile local user able to modify the storage tree concurrently may exploit
  a time-of-check/time-of-use race. A production implementation should use
  descriptor-relative operations (platform-specific descriptor-relative operations) throughout.
- Connections are dispatched through a bounded pool of 16 workers and a queue
  capped at 256 pending tasks. Excess connections are closed rather than spawning
  unbounded threads. On shutdown, listeners and tracked client sockets are
  interrupted, worker handlers are joined, and the pool is drained.
- A process-wide destination reservation prevents concurrent HTTP/TCP uploads
  from writing to the same destination path. Browser uploads use per-transfer
  temporary files. Verified file publication uses replacement semantics.
- This does not eliminate every filesystem TOCTOU race: path validation and
  subsequent file operations are still path-based rather than descriptor-relative.
- TLS, authentication, rate limiting, per-user authorization, durable transfer
  manifests, and disk quotas are not implemented in this revision.

## Safe local run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/server -p 4040 -web-port 8080
```

Both listeners bind to loopback by default. To deliberately expose the listeners
on a private network, pass explicit bind addresses and apply firewall rules.
