# Adaptive browser upload — v5.2

The dashboard no longer sends the complete file as one HTTP request with a fixed 64 KiB server-side read loop. It uses a sequence of HTTP POST requests, each carrying one application data block.

## Lifecycle

1. `POST /api/upload/init` creates a transfer registry entry, reserves the destination path, and creates a unique temporary file named with the transfer ID.
2. The browser computes SHA-256 incrementally before sending the file.
3. The browser sends each block to `POST /api/upload/chunk` with `X-Transfer-ID`, `X-Chunk-Offset`, `X-Chunk-Size`, and `X-SHA256` headers.
4. The server validates the transfer, exact next offset, maximum block size, and destination reservation. It appends the block to the transfer-specific temporary file.
5. The server records the actual received block size and measured server-side throughput in the transfer registry. These samples feed the telemetry and adaptation charts.
6. When all bytes have arrived, the server computes SHA-256. A mismatch discards the temporary file; a match publishes it using replace semantics.

## Adaptation

The browser starts at 64 KiB and keeps the next block size within 4 KiB–4 MiB. It compares measured throughput windows. A material increase can double the block size; a material decrease reduces it, except when a long request window indicates per-request latency dominates, in which case the block is cautiously increased to amortize that overhead. During stable windows the sender probes upward a limited number of times and resets the probe budget when throughput improves.

The native TCP sender uses the same general stable-window probing principle. On the receive side, telemetry records the actual size of the last received protocol frame; the receiver no longer invents a local next-block size that it cannot control.

## Controls and integrity

Pause is observed between browser block requests and again by the server before accepting a block. Resume restores the active state. Cancel removes the transfer-specific temporary file and releases the destination reservation. The byte-corruption experiment modifies the received block before it is written; the final SHA-256 comparison rejects the file.

Zero-byte files are finalized with an empty block request and the standard SHA-256 digest for empty content.

## Export

`GET /api/export/metrics.csv` exports the retained transfer records and their server-side samples, including actual block sizes and measured throughput. The CSV includes a UTF-8 BOM for spreadsheet compatibility.

## Boundaries

- Browser uploads do not yet persist a resumable manifest across browser or server restarts.
- Each application block is a separate HTTP request, so this path has request overhead; the adaptive block growth is intended to reduce it on latency-dominated links.
- The legacy `/api/upload` endpoint remains for compatibility tests and older clients. The dashboard uses `/api/upload/chunk`.
- Network Lab delay and jitter are application-level emulation hooks, not a physical packet-loss emulator.
