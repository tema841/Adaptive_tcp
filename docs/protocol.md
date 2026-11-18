# Current wire protocol

The current protocol is a small newline-delimited command header followed by
length-prefixed data blocks. It is intentionally simple and is not a replacement
for a standardized authenticated file-transfer protocol.

## GET

Client sends:

```text
GET <relative-path> 0 <offset> <chunk-size>\n
```

Server responds:

```text
OK <file-size> <accepted-offset> <sha256>\n
```

Each data block is encoded as an unsigned 64-bit big-endian byte length followed
by exactly that many bytes. The receiver computes SHA-256 and sends:

```text
VERIFY <sha256>\n
```

The server responds with `VERIFY OK` or `VERIFY FAIL`.

## PUT

Client sends:

```text
PUT <relative-path> <file-size> <offset> <chunk-size> <sha256>\n
```

Server responds with the accepted partial-file offset:

```text
OK <accepted-offset>\n
```

After all blocks, the server validates the digest, atomically renames the
`.part` file into place, and responds with `DONE <sha256>` or `ERR <reason>`.

## Limits and validation

- Chunk size: 4 KiB through 4 MiB.
- Data block length: non-zero, at most 4 MiB, and cannot exceed the declared
  remaining file length.
- Paths must be relative to the configured storage root.
- SHA-256 values for PUT must be 64 lowercase hexadecimal characters.

The protocol currently has no explicit version/magic field, authentication,
encryption, or durable transfer identifier. These are documented limitations.
