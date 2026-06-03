# MiniDB Strict 2PL Locking Notes

MiniDB currently uses RID-level Strict Two-Phase Locking for row access.

Implemented behavior:

```text
LockKey = table_name + page_id + slot_id
RID = page_id + slot_id, unique only inside one table data file
S lock = shared/read row lock
X lock = exclusive/write row lock
```

Compatibility:

```text
S + S -> allowed
S + X -> blocked
X + S -> blocked
X + X -> blocked
```

Transaction behavior:

```text
BEGIN keeps a single active writer transaction gate because rollback is snapshot-based.
SELECT row reads acquire S locks.
UPDATE/DELETE row writes acquire X locks.
INSERT acquires X lock for the newly created RID.
Transaction locks are held until COMMIT or ROLLBACK.
Autocommit statements use short locks released at statement end.
```

Deadlock handling:

```text
LockManager maintains a waits-for graph.
When a transaction waits, edges are added:
waiting_txn -> holding_txn
DFS checks for a cycle.
If a cycle exists, the youngest transaction in the cycle is selected as victim.
The victim is marked aborted and its locks are released.
```

Timeout fallback:

```text
LOCK_TIMEOUT_MS = 5000
If a transaction waits too long, it is aborted with:
"Transaction aborted due to lock wait timeout."
```

Current limitations:

```text
No MVCC.
No dirty reads.
No multiple isolation levels.
No predicate locks.
No next-key locks.
No table intent locks.
No full phantom prevention for range predicates.
No concurrent writer transactions while rollback uses whole-file snapshots.
```

Future work:

```text
MVCC would require version chains, visibility rules, and garbage collection/vacuum.
Full range-query serializability would require predicate/range locks or next-key locks.
Full multi-granularity locking would require table/page/row intent locks.
Production rollback would require undo logging instead of whole-file snapshots.
Concurrent writer transactions would require replacing snapshot rollback with undo logging.
```

## Demo Scenarios

### Case A: Shared Locks Compatible

```text
T1 acquires S lock on RID(1,1)
T2 acquires S lock on RID(1,1)
Both proceed.
```

### Case B: Write Blocks Read

```text
T1 acquires X lock on RID(1,1)
T2 requests S lock on RID(1,1)
T2 waits until T1 commits or rolls back.
```

### Case C: Write Blocks Write

```text
T1 acquires X lock on RID(1,1)
T2 requests X lock on RID(1,1)
T2 waits until T1 commits or rolls back.
```

### Case D: Deadlock Detection

```text
T1 locks RID A
T2 locks RID B
T1 requests RID B
T2 requests RID A
waits-for graph has cycle T1 -> T2 -> T1
youngest transaction is aborted
other transaction continues
```

### Case E: Lock Timeout

```text
T1 holds X lock on RID(1,1)
T2 waits for RID(1,1)
If T2 waits longer than 5000 ms, T2 is aborted/fails cleanly.
```
