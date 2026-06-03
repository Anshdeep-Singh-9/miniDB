# MiniDB Final Interview Explanation

This document is the current project-state explanation for MiniDB.

It is written for interview use:

- what MiniDB is
- what we actually implemented
- what is still missing compared to PostgreSQL / InnoDB
- how the codebase is organized
- what to say honestly and confidently

---

## 1. What MiniDB Is

MiniDB is a relational database engine built from scratch in C++.

It is not a CRUD app using MySQL/PostgreSQL. It implements several internal DBMS layers itself:

- SQL-like parsing
- schema/catalog handling
- tuple serialization
- slotted page storage
- page-level disk I/O
- buffer pool caching
- primary-key B+ Tree indexing
- joins, filtering, update, delete
- WAL-style REDO recovery
- transactions with `BEGIN / COMMIT / ROLLBACK`
- RID-level Strict 2PL with deadlock detection
- authentication
- user-scoped and database-scoped storage namespaces

Interview one-liner:

> MiniDB is a from-scratch C++ relational DBMS where we implemented the core storage, indexing, execution, recovery, and transaction foundations ourselves instead of relying on an existing database engine.

---

## 2. Current Implemented Status

### Query Layer

MiniDB currently supports:

- `SHOW TABLES`
- `SHOW USERS`
- `SHOW DATABASES`
- `CREATE TABLE`
- `DROP TABLE`
- `INSERT INTO`
- `SELECT *`
- `SELECT specific_columns`
- `SELECT ... WHERE`
- `SELECT ... ORDER BY ...`
- `SELECT ... LIMIT ...`
- `COUNT(*)`
- `SUM / AVG / MIN / MAX`
- single-column `GROUP BY`
- `INNER JOIN`
- `LEFT JOIN`
- `UPDATE ... SET ... WHERE`
- `DELETE ... WHERE`
- `CREATE USER ... IDENTIFIED BY ...`
- `DROP USER`
- `CREATE DATABASE`
- `USE database_name`
- `DROP DATABASE`
- `BEGIN`
- `COMMIT`
- `ROLLBACK`

### Storage Layer

MiniDB uses:

- fixed-size 4KB pages
- slotted page layout
- tuple serialization into raw bytes
- `RID(page_id, slot_id)` row addressing
- one data file per table: `data.dat`
- one index file per table: `index.dat`
- one WAL file per table: `wal.log`
- table metadata file: `met`

### Memory Layer

MiniDB has:

- Buffer Pool Manager
- page caching in RAM
- dirty-page tracking
- `pin_count`
- LRU replacement
- page flushing and checkpoint-style behavior

### Index Layer

MiniDB has:

- primary-key B+ Tree index
- first column treated as primary key
- primary key must be `INT`
- primary-key lookup uses B+ Tree
- non-indexed predicates use linear scan

### Recovery + Transactions

MiniDB has:

- WAL-style REDO logging
- page after-image redo records
- crash recovery at startup
- transaction context tagging in WAL
- `BEGIN / COMMIT / ROLLBACK`
- snapshot-based rollback
- table-qualified RID-level Strict 2PL
- shared row locks for reads
- exclusive row locks for writes
- waits-for graph deadlock detection
- lock timeout fallback
- single active writer transaction gate

### Auth + Namespacing

MiniDB now has:

- global auth storage
- CLI login with password hashing
- user creation and deletion
- user-scoped storage roots
- multiple named databases per user
- active database switching with `USE`

Current storage shape is effectively:

```text
system/auth/...

users/<username>/database_list
users/<username>/databases/<database>/table/
users/<username>/databases/<database>/system/
```

That means:

- auth is global
- each user has isolated storage
- each user can own multiple named databases
- each database has its own tables, indexes, WAL, and snapshot space

### Deployment

MiniDB also has:

- Docker CLI packaging
- public Docker image support

---

## 3. MiniDB Now Has

- SQL-like parser
- basic executor with scans, filters, joins, update, delete
- `ORDER BY`
- `LIMIT`
- aggregation: `COUNT(*)`, `SUM`, `AVG`, `MIN`, `MAX`
- single-column `GROUP BY`
- page-based storage
- slotted pages
- tuple serialization
- RID addressing
- buffer pool with LRU and dirty tracking
- primary-key B+ Tree index
- WAL-style REDO recovery
- `BEGIN / COMMIT / ROLLBACK`
- snapshot rollback
- table-qualified RID-level Strict 2PL
- waits-for graph deadlock detection
- lock timeout fallback
- single active writer gate for rollback safety
- basic authentication
- `CREATE USER / SHOW USERS / DROP USER`
- per-user isolated storage
- multiple named databases per user
- `CREATE DATABASE / SHOW DATABASES / USE / DROP DATABASE`
- Docker CLI packaging

---

## 4. MiniDB Still Does Not Have

- full SQL grammar
- AST-based planner
- cost-based optimizer
- secondary indexes
- multi-column `GROUP BY`
- `HAVING`
- aggregate-over-join planning
- subqueries
- aliases / richer SQL expressions
- foreign keys
- check constraints
- undo logging
- ARIES recovery
- MVCC
- true concurrent independent writers
- phantom prevention
- predicate locks / range locks
- multi-granularity intent locking
- full production isolation-level support
- replication
- backup / restore tooling
- role-based permissions
- monitoring / tuning tools
- true multi-client server session isolation for the HTTP API

Important honesty line:

> MiniDB has meaningful DBMS internals implemented, but it is still an educational engine, not a production-grade PostgreSQL/InnoDB replacement.

---

## 5. Best Interview Closing Line

> MiniDB is not production-grade yet, but it now covers many real DBMS internals: SQL-like parsing, page storage, slotted pages, tuple serialization, buffer pool caching, B+ Tree indexing, joins, ordering, aggregation, WAL-style recovery, transactions, RID-level Strict 2PL with deadlock detection, authentication, and per-user multi-database storage namespaces. The next major production-facing steps would be a real planner/optimizer, secondary indexes, UNDO logging, stronger recovery, and eventually MVCC or full multi-writer transaction support.

---

## 6. MiniDB vs Real Databases

### What PostgreSQL / InnoDB Have

Production databases typically have:

- full SQL grammar
- query planner and optimizer
- multiple access paths
- multiple index types
- MVCC
- richer isolation levels
- undo + redo machinery
- stronger recovery model
- better concurrency control
- replication and backup
- server process model
- observability and tuning

### What MiniDB Implements Today

MiniDB already implements simplified but real versions of:

- page-based storage
- slotted pages
- buffer pool
- B+ Tree index
- parser and executor
- joins
- updates and deletes
- WAL-style REDO
- startup recovery
- transactions
- row-level locking
- deadlock detection
- authentication
- user/database namespacing

### Honest Positioning

Good interview phrasing:

> We implemented the core mechanisms in simplified form. The goal was not to compete with PostgreSQL, but to understand why systems like PostgreSQL and InnoDB need pages, buffer pools, indexes, WAL, recovery, and concurrency control in the first place.

---

## 7. High-Level Architecture

```text
User
 |
 v
main.cpp
 |
 |-- AuthManager
 |-- CLI login
 |-- active user / active database selection
 |-- startup recovery
 |-- query console
 |
 v
parser.cpp
 |
 |-- tokenize SQL-like query
 |-- detect command type
 |-- apply statement-level read/write lock policy
 |-- dispatch to executor
 |
 v
Execution Layer
 |
 |-- create.cpp
 |-- insert.cpp
 |-- display.cpp
 |-- where.cpp
 |-- update.cpp
 |-- delete.cpp
 |
 v
Storage / Index / Recovery Layer
 |
 |-- tuple_serializer.cpp
 |-- data_page.cpp
 |-- disk_manager.cpp
 |-- buffer_pool_manager.cpp
 |-- BPtree.cpp
 |-- recovery_manager.cpp
 |-- transaction_manager.cpp
 |-- lock_manager.cpp
 |
 v
Files on disk
 |
 |-- system/auth/...
 |-- users/<user>/databases/<db>/table/<table>/met
 |-- users/<user>/databases/<db>/table/<table>/data.dat
 |-- users/<user>/databases/<db>/table/<table>/index.dat
 |-- users/<user>/databases/<db>/table/<table>/wal.log
```

---

## 8. Core DBMS Concepts We Implemented

### Page-Based Storage

Rows are not stored as one file per row. They are packed into 4KB pages.

Why it matters:

- efficient disk I/O
- fixed-size page addressing
- buffer pool compatibility

### Slotted Pages

Each page stores:

- page header
- slot directory
- tuple bytes
- free space

Why it matters:

- supports variable-length rows
- slot id stays stable
- enables RID addressing

### Tuple Serialization

Logical typed values like:

```text
1, "Aryan", "CSE"
```

are converted into raw bytes before storage.

Why it matters:

- disk stores bytes, not C++ objects
- rows need deterministic physical format

### RID Addressing

Rows are identified as:

```text
RID(page_id, slot_id)
```

Why it matters:

- B+ Tree points to rows using RID
- update/delete/select can jump directly to location

### Buffer Pool

Pages are cached in RAM before disk access.

Why it matters:

- disk is slower than RAM
- avoids repeated reads
- supports dirty-page delayed flush

### B+ Tree Index

Primary key is mapped to RID:

```text
primary_key -> RID
```

Why it matters:

- primary-key lookup becomes direct
- avoids full table scan for indexed search

### WAL-Style REDO Recovery

MiniDB writes redo information before final page state is treated as durable.

Why it matters:

- crash after partial write can be repaired
- committed changes can be replayed

### Transactions

MiniDB supports:

- `BEGIN`
- `COMMIT`
- `ROLLBACK`

Current rollback is snapshot-based.

Why it matters:

- all-or-nothing behavior
- consistent user-visible state

### Strict 2PL

MiniDB uses table-qualified RID-level Strict 2PL:

- shared lock for row reads
- exclusive lock for row writes
- row locks qualified by `(table, RID)`
- waits-for graph deadlock detection
- timeout fallback

Why the table qualifier matters:

> `RID(0,1)` in `students` is not the same row as `RID(0,1)` in `courses`, so lock identity must be `(table_name, page_id, slot_id)`.

### Single Active Writer Gate

Even though row-level locks exist, MiniDB still keeps one active writer transaction gate.

Why:

- rollback is still snapshot-based
- multiple independent writers would need stronger undo/version infrastructure

### User + Multi-Database Namespace

MiniDB now has:

- global auth
- per-user isolated storage
- multiple named databases per user
- database switching through `USE`

Why it matters:

- better logical separation
- closer to real DBMS organization
- enables cleaner demos and testing

---

## 9. Join Status

Join support is implemented, but in limited educational form.

What works:

- `INNER JOIN`
- `LEFT JOIN`
- equality join predicate
- optional simple `WHERE` after join

Execution model:

- nested-loop join

Important limitations:

- no optimizer
- no hash join
- no merge join
- no index-assisted join planning
- no complex join expressions
- no real SQL `NULL` semantics on unmatched right side

Interview-safe line:

> MiniDB supports INNER JOIN and LEFT JOIN using a nested-loop join executor. It is functionally useful for basic equality joins, but it is not optimizer-driven like PostgreSQL or MySQL.

---

## 10. Parser Status

The parser is implemented in `src/parser.cpp`.

It is a custom SQL-like parser, not a full SQL grammar engine.

It supports:

- DDL routing
- DML routing
- transaction commands
- join query tokenization
- aggregate / order / group / limit parsing
- user/database management commands

What it does not yet do:

- build a real AST
- produce costed plans
- optimize joins
- support full SQL syntax

Interview-safe line:

> MiniDB has a handwritten SQL-like parser that tokenizes supported commands, validates project-specific syntax, applies locking policy, and dispatches to the correct executor, but it is not yet a full AST-based SQL compiler or optimizer.

---

## 11. Authentication Status

MiniDB authentication is implemented internally, not delegated to an external DB.

It uses:

- user records stored in MiniDB-managed files
- SHA-256 password hashing
- login verification against stored hash
- user creation and deletion

Important scope:

- auth catalog is global
- data namespaces are per-user
- HTTP API session isolation is still not fully per-user/per-database scoped

Interview-safe line:

> Even the auth layer reuses our own storage concepts. User metadata is stored using MiniDB-managed files and password hashes, rather than being hardcoded or delegated to another database.

---

## 12. What I Would Say Was My Main Contribution

Strong version:

> My main contribution was on the storage and reliability side of the engine. I worked on buffer-pool-oriented page handling, tuple/page storage flow, WAL-style recovery, transaction behavior, and concurrency control. That included moving the project toward DBMS-style page management and strict row-level locking with deadlock detection instead of simple file operations.

If you want to mention newer work too:

> I also helped push the project outward from storage internals into system behavior by improving authentication flow, user management, per-user storage isolation, and multiple named databases per account.

---

## 13. How To Explain The Difference From a CRUD Project

Use this:

> A normal CRUD project uses an existing database. MiniDB is the database. We had to define how rows become bytes, how bytes are packed into slotted pages, how pages are cached in RAM, how indexes point to row locations, how crashes are recovered using WAL-style logging, and how transactions and row locks behave internally.

---

## 14. Honest Limitation Statement

Use this if asked where the project is still weaker:

> The project now covers many core DBMS internals, but it is still simplified in important ways. Recovery is not ARIES-style, rollback is snapshot-based, there is no MVCC, the parser is not a full SQL compiler, the optimizer is minimal, and the API layer is not yet a true multi-session server with isolated request-scoped database contexts.

---

## 15. Best Final Summary

> MiniDB is a learning-focused relational database engine where we implemented many real DBMS internals in working simplified form: SQL-like parsing, slotted-page storage, tuple serialization, buffer pool caching, B+ Tree indexing, joins, WAL-style recovery, transactions, RID-level Strict 2PL with deadlock detection, authentication, and per-user multi-database storage namespaces. It is not production-grade yet, but it demonstrates the architecture and reasoning behind real systems like PostgreSQL and InnoDB.
