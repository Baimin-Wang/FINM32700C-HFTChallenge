# HFT Matrix Challenge — Group 19

## Group Name

Group 19

## Build

**Option A:**
```bash
./build.sh
```

**Option B:**
```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

Binaries are placed in `build/bin/`.

## Run

**Reference server:**
```bash
./build/bin/hftserver2026
```

**Competition client:**
```bash
./build/bin/hftclient2026 <host> <port> <team_name>
```

Example:
```bash
./build/bin/hftclient2026 127.0.0.1 12345 Group19
```

**Concurrent client (multi-threaded):**
```bash
./build/bin/hftclient_concurrent <host> <port> <team_name>
```

**Blast stress-test server:**
```bash
./build/bin/blast_server --rate 50 --window 30 --size 128 --mode 1
```

## Optimizations

**O(N²) checksum shortcut**
The sum of all entries of C = A×B satisfies:

```
sum(C) = sum_k [ colSum_A(k) * rowSum_B(k) ]
```

This avoids the full O(N³) matrix multiplication, reducing 2M operations to ~16K for N=128.

**Thread pool**
A pool of `hardware_concurrency` worker threads dequeues parsed challenges, computes the checksum, and sends the answer concurrently. The main thread is dedicated to recv and parsing only, so it never blocks on computation.

**TCP_NODELAY**
Nagle's algorithm is disabled so small reply packets (~8 bytes) are sent immediately without buffering.

**Compiler optimizations**
Built with `-O3 -march=native` for full compiler optimization tuned to the host CPU.

**Stack-allocated reply buffer**
The reply string is formatted with `snprintf` into a 32-byte stack buffer, avoiding heap allocation per challenge.
