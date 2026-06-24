<div align="center">

```
███╗   ███╗██╗███╗   ██╗██╗██████╗ ██████╗
████╗ ████║██║████╗  ██║██║██╔══██╗██╔══██╗
██╔████╔██║██║██╔██╗ ██║██║██║  ██║██████╔╝
██║╚██╔╝██║██║██║╚██╗██║██║██║  ██║██╔══██╗
██║ ╚═╝ ██║██║██║ ╚████║██║██████╔╝██████╔╝
╚═╝     ╚═╝╚═╝╚═╝  ╚═══╝╚═╝╚═════╝ ╚═════╝
```

**A relational database engine — built from scratch in C++17**

*Zero dependencies on SQLite, MySQL, or any existing database.*

---

[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=c%2B%2B&logoColor=white)](https://isocpp.org/)
[![Build](https://img.shields.io/badge/Build-Make%20%2F%20GCC-F57C00?style=flat-square)](https://www.gnu.org/software/make/)
[![License](https://img.shields.io/badge/License-MIT-22C55E?style=flat-square)](LICENSE)
[![Status](https://img.shields.io/badge/Status-Active%20Development-8B5CF6?style=flat-square)]()
[![Docker](https://img.shields.io/badge/Docker-Compose%20Ready-2496ED?style=flat-square&logo=docker&logoColor=white)](https://www.docker.com/)

</div>

---

## What Is MiniDB?

MiniDB is a ground-up relational database engine — not a wrapper, not a toy embedded lib, but an actual DBMS implemented piece by piece in C++17. Every subsystem you'd find in a production database exists here in its own form:

| What real databases do | What MiniDB implements |
|---|---|
| Store rows on disk in pages | 4 KB slotted pages via `DiskManager` |
| Cache pages in RAM | LRU `BufferPoolManager` |
| Index rows by primary key | Persistent B+ Tree on `index.dat` |
| Parse SQL | Custom tokenizer + query router |
| Isolate concurrent writes | Lock manager with strict 2PL + deadlock detection |
| Survive crashes | Write-ahead redo logging + recovery on startup |
| Serve external clients | REST API via Crow + React dashboard |

---

## Feature Overview

<details>
<summary><b>Storage Engine</b></summary>

- Fixed-size **4 KB page I/O** — every table's rows live in `data.dat`, paged like real DBMSs
- **Slotted page layout** — header + slot directory from front, tuples packed from back; rows addressed by `RID(page_id, slot_id)`
- **Variable-length tuple serialization** — INTs as 4 bytes, VARCHARs with `uint16_t` length prefix (no padding waste)
- **Eager flush** + background dirty-page checkpointing
- **Page compaction / vacuum** helpers to reclaim space after deletes and large updates

</details>

<details>
<summary><b>B+ Tree Index</b></summary>

- Multi-level B+ Tree on the primary key, fully persisted to `index.dat`
- `O(log n)` point lookup returning `RID(page_id, slot_id)`
- Automatic node splitting at `MAX_KEYS` capacity
- **Doubly-linked leaf nodes** (`next_page_id` / `prev_page_id`) for range scan support
- Page 0 of `index.dat` stores root metadata; tree survives process restarts

</details>

<details>
<summary><b>Buffer Pool Manager</b></summary>

- In-memory **LRU page cache** between execution layer and disk
- Pages are **pinned** on fetch, **unpinned** after use; dirty pages written back on eviction or flush
- Background flush triggers when dirty page ratio exceeds 50% of pool
- Tracks cache hits, misses, dirty evictions, clean evictions, and checkpoint counts via `BufferPoolStats`

</details>

<details>
<summary><b>Query Engine</b></summary>

- Custom **SQL tokenizer** — keywords lowercased, identifiers and string values case-preserved
- `SELECT *`, projected column select, `WHERE`, `ORDER BY`, `LIMIT`
- Aggregate functions: `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`
- `GROUP BY` with multi-column support and `HAVING` filters
- `JOIN` and `LEFT JOIN` for two-table queries
- B+ Tree point lookup on PK `WHERE`; linear page scan on non-PK `WHERE`

</details>

<details>
<summary><b>Transactions & Locking</b></summary>

- `BEGIN` / `COMMIT` / `ROLLBACK` with snapshot-based rollback
- **Strict 2PL lock manager** — shared and exclusive row/page/table locks
- **Waits-for graph** with DFS-based deadlock detection; youngest transaction aborted as victim
- Lock wait timeout (default 5 seconds) before auto-abort
- Lock upgrade path (`S → X`) for read-then-write scenarios
- Short-duration latches separate from long-duration transaction locks

</details>

<details>
<summary><b>Recovery</b></summary>

- **Write-ahead redo logging** — WAL records written before page changes hit disk
- Recovery manager replays committed WAL entries on startup
- Uncommitted/partial writes are ignored; only committed page images applied

</details>

<details>
<summary><b>Auth & Multi-Tenancy</b></summary>

- First-run user creation with **SHA-256 password hashing**
- Per-user **database namespace** — each user's tables are isolated
- `CREATE DATABASE`, `USE`, `SHOW DATABASES`, `DROP DATABASE`
- `CREATE USER ... IDENTIFIED BY`, `DROP USER`, `SHOW USERS`
- Session token–based auth for the REST API

</details>

<details>
<summary><b>REST API & Frontend</b></summary>

- REST API built with [Crow](https://github.com/CrowCpp/Crow), listening on `:18080`
- Token-based auth via `X-Session-Token` header
- Endpoints for table listing, row fetch, metadata, create, insert, bulk insert, raw SQL query
- **React + TypeScript + Vite** dashboard with login, table browsing, SQL console, insert/delete UI, and seed data support
- Docker + Docker Compose ready for one-command deployment

</details>

---

## Architecture

```
  ┌────────────────────────────────────────────────────────────┐
  │          CLI Terminal / REST API / React Dashboard          │
  └────────────────────────────┬───────────────────────────────┘
                               │
                    ┌──────────▼──────────┐
                    │    Query Parser      │  tokenize → route
                    │    parser.cpp        │  preserve quoted values
                    └──────┬──────────────┘
                           │
         ┌─────────────────┼──────────────────────┐
         ▼                 ▼                       ▼
   DDL Execution     DML Execution           SELECT / SHOW
  create / alter   insert / update          display / where
                   / delete                 GROUP BY / JOIN
         │                 │                       │
         └────────┬─────────────────────┬──────────┘
                  │                     │
       ┌──────────▼──────┐   ┌──────────▼──────────┐
       │ Transaction Mgr  │   │    Lock Manager       │
       │ snapshot + WAL   │   │ 2PL + deadlock DFS    │
       └──────────┬───────┘   └─────────────────────-┘
                  │
       ┌──────────▼──────────────────┐
       │      Buffer Pool Manager     │  LRU in-memory page cache
       └──────────┬───────────────────┘
                  │
       ┌──────────▼──────────────────┐
       │       Disk Manager           │  4 KB fixed-page file I/O
       └──────────┬───────────────────┘
                  │
       ┌──────────▼──────────────────┐
       │   Data Pages + Tuple Codec  │  slotted layout + serialize
       └──────────┬───────────────────┘
                  │
       ┌──────────▼──────────────────┐
       │     B+ Tree Primary Index    │  RID lookup, leaf link chain
       └─────────────────────────────┘
```

### Core Source Files

| Area | File(s) |
|---|---|
| Entry point + terminal UI | `src/main.cpp` |
| Query parser + router | `src/parser.cpp` |
| DDL — create / alter | `src/create.cpp`, `src/alter.cpp` |
| DML — insert / update / delete | `src/insert.cpp`, `src/update.cpp`, `src/delete.cpp` |
| SELECT, JOIN, GROUP BY, ORDER BY | `src/display.cpp`, `src/where.cpp` |
| Slotted data pages | `src/data_page.cpp` |
| Disk I/O | `src/disk_manager.cpp` |
| LRU buffer pool | `src/buffer_pool_manager.cpp` |
| B+ Tree index | `src/BPtree.cpp` |
| Tuple encode / decode | `src/tuple_serializer.cpp` |
| Catalog + namespaces | `src/file_handler.cpp` |
| Auth + SHA-256 | `src/auth.cpp`, `src/sha256.cpp` |
| Transaction + lock | `src/transaction_manager.cpp`, `src/lock_manager.cpp` |
| WAL + recovery + vacuum | `src/recovery_manager.cpp`, `src/vacuum.cpp` |
| REST API | `api/server.cpp` |
| Frontend | `frontend/src/App.tsx` |

---

## Build & Run

### Prerequisites

- GCC with C++17 support
- `make`
- _(Optional for API)_ Crow headers, Boost
- _(Optional for frontend)_ Node.js 18+

### Terminal Engine

```bash
# Build
make

# First run — creates the initial user
./miniDB -u admin -p

# Subsequent runs — prompts for password
./miniDB -u admin -p
```

### REST API Server

```bash
make api
./server        # listens on :18080
```

### Frontend Dashboard

```bash
cd frontend
npm install
npm run dev
```

Configure your web server / proxy so `/api` routes reach the MiniDB server.

### Docker (one command)

```bash
docker compose up --build
```

---

## CLI Menu

After login, MiniDB opens a numbered terminal interface:

```
╔══════════════════════════════╗
║        MiniDB Engine         ║
╠══════════════════════════════╣
║  1.  Query Console           ║
║  2.  Search table            ║
║  3.  Print table metadata    ║
║  4.  Help                    ║
║  5.  Quit                    ║
╚══════════════════════════════╝
```

Type `BACK` or `EXIT` inside the Query Console to return to the menu.

---

## Query Syntax Reference

```sql
-- ── Database management ─────────────────────────────────────
SHOW DATABASES;
CREATE DATABASE semester4;
USE semester4;
DROP DATABASE semester4;

-- ── User management ─────────────────────────────────────────
SHOW USERS;
CREATE USER administrator IDENTIFIED BY "pass123";
DROP USER administrator;

-- ── Table management ────────────────────────────────────────
SHOW TABLES;
CREATE TABLE students (id INT, name VARCHAR(50), dept VARCHAR(20));
ALTER TABLE students ADD COLUMN age INT DEFAULT 18;
DROP TABLE students;

-- ── Insert ──────────────────────────────────────────────────
INSERT INTO students VALUES (1, "Aditya", "CSE");

-- ── Select ──────────────────────────────────────────────────
SELECT * FROM students;
SELECT name, dept FROM students;
SELECT * FROM students WHERE id = 1;          -- B+ Tree lookup
SELECT * FROM students WHERE dept = CSE;      -- linear scan
SELECT * FROM students ORDER BY name;
SELECT * FROM students LIMIT 5;

-- ── Aggregates ──────────────────────────────────────────────
SELECT COUNT(*) FROM students;
SELECT SUM(id), AVG(id), MIN(name), MAX(name) FROM students;
SELECT dept, COUNT(*) FROM students GROUP BY dept;
SELECT dept, COUNT(*) FROM students GROUP BY dept HAVING COUNT(*) > 1;
SELECT dept, name, COUNT(*) FROM students GROUP BY dept, name;

-- ── Joins ───────────────────────────────────────────────────
SELECT students.name, departments.hod
FROM students JOIN departments ON students.dept = departments.code;

SELECT students.name, departments.hod
FROM students LEFT JOIN departments ON students.dept = departments.code;

-- ── Update / Delete ─────────────────────────────────────────
UPDATE students SET dept = ECE WHERE id = 1;
DELETE FROM students WHERE dept = CSE;

-- ── Transactions ────────────────────────────────────────────
BEGIN;
COMMIT;
ROLLBACK;
```

**Query notes**

- SQL keywords are **case-insensitive**; table/column names are **case-sensitive**
- First column of every table **must be `INT`** — it becomes the B+ Tree primary key
- Primary keys are unique and cannot be updated
- Accepted string types: `VARCHAR`, `VARCHAR(n)`, `TEXT`, `STRING`
- `WHERE` currently supports equality conditions
- Aggregate functions and `GROUP BY` / `HAVING` over joins are not yet supported

---

## REST API

The API server lives in `api/server.cpp` and uses token-based auth.

**Authentication**

```bash
# Register / login
curl -X POST http://localhost:18080/register \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"pass"}'

curl -X POST http://localhost:18080/login \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"pass"}'
# → { "token": "<session-token>" }

# All authenticated endpoints require:
# X-Session-Token: <token>
```

**Endpoints**

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/health` | Health check |
| `POST` | `/register` | Create a new user |
| `POST` | `/login` | Authenticate, get session token |
| `POST` | `/logout` | Invalidate session token |
| `GET` | `/tables` | List all tables |
| `GET` | `/table/<name>` | Fetch all rows as JSON |
| `GET` | `/meta/<name>` | Fetch table schema/metadata |
| `POST` | `/create` | Create table from JSON schema |
| `POST` | `/insert/<name>` | Insert one row |
| `POST` | `/bulk_insert/<name>` | Insert many rows |
| `POST` | `/query` | Execute any SQL-like query |

See [`Documentation/api.md`](Documentation/api.md) for full request/response examples.

---

## Storage Layout

```
table/
└── <table_name>/
    ├── data.dat      ← slotted 4 KB pages holding row tuples
    ├── index.dat     ← persistent B+ Tree nodes
    ├── met           ← binary table metadata (schema)
    └── wal.log       ← redo records (present during active transactions)

system/
    ├── auth          ← hashed credentials
    ├── databases     ← database registry
    ├── sessions      ← active API session tokens
    ├── snapshots     ← transaction rollback snapshots
    └── catalog       ← cross-namespace catalog data
```

---

## Data Types

| Type | Accepted forms | Stored as |
|---|---|---|
| Integer | `INT`, `INTEGER` | `int32_t` — 4 bytes |
| String | `VARCHAR`, `VARCHAR(n)`, `TEXT`, `STRING` | `uint16_t` length + actual bytes |

---

## Testing

Tests live in `tests/` and cover storage, select features, schema changes, user/database namespaces, vacuum, and strict 2PL lock behavior.

```bash
# Build then compile a single test
make
g++ -std=c++17 -Wall -Wextra -g -I include \
  tests/test_select_features.cpp \
  $(find src -name '*.cpp' ! -name 'main.cpp') \
  -o test_select_features

./test_select_features
```

---

## Repository Layout

```
miniDB/
├── api/                    ← Crow REST API server
├── Documentation/          ← api.md, auth.md, utilities.md, design notes
├── frontend/               ← React / Vite dashboard
├── include/                ← C++ headers
├── src/                    ← Core engine implementation
├── tests/                  ← C++ and JS tests
├── Dockerfile
├── docker-compose.yml
├── Makefile
└── README.md

── Created at runtime ──────────────────────────────────────────
table/                      ← per-user table data
system/                     ← auth, catalog, session, snapshot data
docker-data/                ← Docker-mounted storage
build/                      ← compiled objects + binaries
miniDB                      ← symlink → build/bin/miniDB
server                      ← symlink → build/bin/server
```

---

## Documentation

- [API Reference](Documentation/api.md)
- [Auth & User Management](Documentation/auth.md)
- [Utilities](Documentation/utilities.md)
- [Design Notes & Syntax Reference](Documentation/notes/)

---

<div align="center">

*Built from scratch · No database dependencies · C++17 · Terminal + REST + React*

</div>