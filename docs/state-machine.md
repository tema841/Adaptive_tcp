# State machine

```mermaid
stateDiagram-v2
 [*] --> Active
 Active --> Paused: dashboard pause
 Paused --> Active: dashboard resume
 Active --> Interrupted: connection breaks / timeout
 Active --> Cancelled: dashboard cancel
 Active --> ChecksumFailed: SHA-256 mismatch
 Active --> Completed: bytes + SHA-256 verified
 Interrupted --> Active: client retries with resume
```

`Completed` допускается только после подтверждения полного размера и SHA-256.
Незавершённые данные остаются во временном `.part` файле и не должны считаться
готовым целевым файлом.
