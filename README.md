# pg_query_stats

**pg_query_stats** is a lightweight PostgreSQL extension that collects and exposes execution statistics for SQL queries. It helps developers and DBAs monitor query performance with minimal overhead and simple architecture.

## 🚀 Features

- Tracks per-query execution statistics:
  - `calls`, `total_time`, `min_time`, `max_time`
- Optional filtering by minimum execution duration
- In-memory data structure (shared memory, no disk writes)
- Simple list-based implementation (no hash tables for timing)
- Low overhead, works across parallel backends
- SQL-accessible interface (`pg_query_stats()`, `pg_query_stats_reset()`)

## 📂 File Structure

This repository includes:

- `pg_query_stats.c` – Core extension source code
- `Makefile` – For building with `pg_config`

## ⚙️ Installation

### 1. Build & Install

```bash
make
sudo make install
