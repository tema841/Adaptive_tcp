# Adaptive TCP File Transfer — v5.2.2 (Windows)

Учебная система передачи файлов по TCP с адаптацией размера блока, возобновлением передачи, SHA-256 и веб-мониторингом. Целевая платформа — **Windows 10/11 x64**. Поддерживаются Visual Studio 2022 / MSVC и MinGW-w64 (включая MSYS2 UCRT64) с CMake и Ninja.

> **Важно:** это учебный проект, не production-ready файловый сервер. Протокол пока не использует TLS и аутентификацию. По умолчанию TCP и веб-панель привязаны только к `127.0.0.1`. Не открывайте порты в недоверенную сеть.

## Возможности

- CLI-клиент и TCP-сервер; GET и PUT.
- Нативный TCP GET/PUT с адаптивным размером блоков 4 KiB–4 MiB.
- Браузерная отправка отдельными HTTP/TCP-запросами адаптивного размера 4 KiB–4 MiB; сервер записывает фактические размеры блоков и скорость в телеметрию.
- Возобновление через `.part` и проверка SHA-256.
- Веб-панель, JSON API и SSE.
- Управление активной передачей: пауза, продолжение, отмена.
- Network Lab: искусственная задержка/jitter, возврат нормального профиля и инъекция повреждённого байта.
- История измерений скорости и фактического размера блока; список передач хранит последние 100 записей, накопительные счётчики работают независимо от этой истории.
- Экспорт фактических измерений и истории передач в CSV для обработки экспериментальных результатов.
- Ограниченный пул соединений: 16 рабочих потоков, очередь до 256 задач; coordinated shutdown.
- Резервирование целевого пути предотвращает одновременную запись двух передач в один файл.
- CTest: модульные, TCP end-to-end, HTTP dashboard, adaptive browser upload и graceful-shutdown тесты.

## Требования

- Windows 10/11 x64
- Один из компиляторов: Visual Studio 2022 с компонентом **Desktop development with C++** либо MinGW-w64 (например, MSYS2 UCRT64)
- CMake 3.20 или новее
- Ninja (для сборки через MSYS2 UCRT64)
- Python 3 (для end-to-end теста)

## Сборка в PowerShell

Из корня проекта в PowerShell с MSYS2 UCRT64 в `PATH`:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Для Visual Studio 2022 / MSVC вместо этого используйте:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

При Ninja готовые файлы находятся в `build\server.exe` и `build\client.exe`; при Visual Studio обычно в `build\Release\`.

## Запуск сервера

Для сборки **Ninja + MSYS2 UCRT64**:

```powershell
.\build\server.exe -p 4040 -web-port 8080 -root storage -web web
```

Для сборки **Visual Studio / MSVC**:

```powershell
.\build\Release\server.exe -p 4040 -web-port 8080 -root storage -web web
```

Команду выполняйте из корня проекта. Откройте `http://127.0.0.1:8080` в браузере. Файлы, загруженные через панель, сохраняются в каталоге `storage`.

## Клиент

Скачать файл с сервера:

```powershell
.\build\Release\client.exe -p 4040 -src 127.0.0.1:example.bin -dst .\example.bin
```

Отправить файл на сервер:

```powershell
.\build\Release\client.exe -p 4040 -src .\example.bin -dst 127.0.0.1:example.bin
```

Дополнительные параметры: `-chunk 65536` задаёт начальный размер чанка; `-no-resume` отключает возобновление.

## API панели

- `GET /api/status` — передачи, статистика и выборки графиков.
- `GET /api/events` — SSE-события.
- `GET /api/export/metrics.csv` — экспорт сохранённых передач и телеметрических выборок в CSV.
- `POST /api/transfer/<id>/pause|resume|cancel` — управление активной передачей.
- `POST /api/demo/network?delay=<ms>&unstable=0|1` — искусственная задержка и периодический jitter; отдельная вероятность потери пакетов не реализована.
- `POST /api/demo/corrupt` — инъекция изменённого байта в активную передачу для проверки SHA-256; при отсутствии активных передач возвращает HTTP 409.
- `POST /api/upload/init` — регистрация браузерной передачи.
- `POST /api/upload/chunk` — приём одного адаптивного блока с `X-Transfer-ID`, `X-Chunk-Offset`, `X-Chunk-Size` и `X-SHA256`.

Управляющие POST endpoints разрешены только с IPv4 localhost.

## Известные ограничения и статус проверки

v5.2 — Windows-ориентированная версия. В текущем архиве проверены локальные unit-тесты и end-to-end TCP GET/PUT в доступном окружении; **сборка MSYS2 UCRT64/MinGW и запуск на Windows должны быть подтверждены на целевой машине**. Браузерный API, адаптивные блоки и контрольные действия проверены интеграционными тестами в доступной среде; целевая сборка и визуальная проверка интерфейса именно на Windows 10/11 остаются обязательным последним этапом.

Сервер использует ограниченный пул из 16 рабочих потоков и очередь до 256 соединений; при остановке сокеты прерываются, обработчики завершаются, рабочие потоки присоединяются. Для записи в один целевой путь действует резервирование, а публикация проверенного файла использует замену без предварительного удаления.

Остаются ограничения: полное устранение TOCTOU, TLS/аутентификация, долговременные манифесты возобновления, настоящее измерение RTT, отдельная эмуляция потери пакетов, квоты диска/пользователей и длительное fuzz/stress-тестирование. Проверка многоклиентского пула пока ограничена параллельными HTTP status-запросами; полноценная нагрузка десятками одновременных передач ещё не подтверждена.

## Browser upload (v5.2)

The dashboard hashes the selected file incrementally in the browser, then sends it through `/api/upload/chunk`. Each HTTP request carries one actual data block. The initial block is 64 KiB; the client adjusts subsequent blocks between 4 KiB and 4 MiB using measured request throughput, trend comparison and cautious probing during stable windows. The server validates offsets, appends each block to a transfer-specific temporary file, records the actual block size and measured server-side throughput, verifies SHA-256, and only then publishes the destination file. Empty files are supported. Pause/resume/cancel apply between block requests. Browser-side durable resume after a browser/server restart is not implemented.

The legacy `/api/upload` endpoint remains for compatibility and older integration tests; the dashboard no longer uses it. The native `client.exe` continues to use the custom TCP GET/PUT protocol on the data port.

## Browser upload and network controls

A browser upload session is registered before the browser computes its SHA-256 preflight. Pause/resume/cancel are available during hashing and between adaptive chunk requests. Network delay and jitter are applied before each browser chunk and before native transfer frames. Byte corruption changes received data and must cause a server-side SHA-256 mismatch; the corrupted destination is not published.

The browser uses `/api/upload/chunk` and real adaptive application blocks. The old `/api/upload` endpoint remains only for compatibility with older clients and regression tests.

## Dashboard refresh (v5.2.0)

The dashboard uses the original paint-stroke background asset at `web/assets/paint-strokes.png`. All four panel corners now use one eight-point geometry with a 2 px cream outline and a 16 px cut. The oversized hero and System Profile panels were removed. The speed telemetry chart has a horizontally scrollable time axis; transfer, event and integrity lists use themed scrollbars. Integrity history is scrollable and constrained to approximately three visible records. Browser upload charts now use actual server-side samples of adaptive block sizes.
