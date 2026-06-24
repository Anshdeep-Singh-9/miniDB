# 📖 MiniDB Query Syntax Reference

> Supported SQL-like syntax for the MiniDB Engine.
> All keywords are **case-insensitive** unless stated otherwise.

---

## SHOW TABLES

Lists all tables currently registered in the database.

### Syntax

```sql
SHOW TABLES;
```

### Example

```sql
SHOW TABLES;
```

### Notes

- No arguments accepted. Any extra tokens will produce a syntax error.

---

## CREATE TABLE

Creates a new table with the specified columns and types.
The **first column must be of type `INT`** — it is used as the Primary Key for the B+ Tree index.

### Syntax

```sql
CREATE TABLE table_name (column1 TYPE, column2 TYPE(size), ...);
```

### Supported Column Types

| Type          | Description                        | Size Behavior                          |
|---------------|------------------------------------|----------------------------------------|
| `INT`         | 32-bit signed integer              | Fixed — always 4 bytes internally      |
| `VARCHAR`     | Variable-length string             | Defaults to engine `MAX_VARCHAR`       |
| `VARCHAR(n)`  | Variable-length string, capped     | Max `n` characters enforced on insert  |

### Primary Key Constraint

- The **first column must be `INT`**.
- It is automatically used as the Primary Key.
- Duplicate primary key values are **rejected** at insert time.
- Attempting to define the first column as `VARCHAR` will produce:
  ```
  ERROR: Primary Key Constraint Failed! The first column (col_name) must be of type INT.
  ```

### Examples

```sql
-- Minimal table
CREATE TABLE students (id INT, name VARCHAR);

-- With explicit VARCHAR sizes
CREATE TABLE employees (emp_id INT, email VARCHAR(100), dept VARCHAR(15));

-- Mixed columns
CREATE TABLE products (product_id INT, label VARCHAR(50), category VARCHAR(20));
```

### Errors

| Condition                         | Error Message                                              |
|-----------------------------------|------------------------------------------------------------|
| Table already exists              | `ERROR: Table 'name' already exists.`                      |
| No columns defined                | `ERROR: No columns defined in query.`                      |
| First column is not INT           | `ERROR: Primary Key Constraint Failed!`                    |
| Duplicate column name             | `ERROR: Duplicate column name 'col_name'.`                 |
| Unknown type                      | `ERROR: Unknown data type 'type' for column 'col_name'.`   |

---

## INSERT INTO

Inserts a single row into an existing table.
Values must be provided in the **same order** as the columns were defined.

### Syntax

```sql
INSERT INTO table_name VALUES (value1, value2, ...);
```

### Value Rules

- `INT` columns expect a plain integer: `42`, `-7`, `0`
- `VARCHAR` columns expect a **quoted string**: `"Aditya"` or `'Aditya'`
- The number of values must **exactly match** the number of columns.
- A `VARCHAR` value longer than the declared size is rejected.
- The primary key (first column) must be **unique** — duplicates are rejected.

### Examples

```sql
INSERT INTO students VALUES (1, "Aditya");

INSERT INTO employees VALUES (101, "aditya@example.com", "CSE");

INSERT INTO products VALUES (5, "Notebook", "Stationery");
```

### Errors

| Condition                          | Error Message                                              |
|------------------------------------|------------------------------------------------------------|
| Table does not exist               | `ERROR: Table 'name' does not exist or metadata could not be loaded.` |
| Wrong number of values             | `ERROR: Column count mismatch. Expected N values, received M.` |
| Duplicate primary key              | `Error: Primary Key N already exists.`                     |
| Invalid INT value                  | `ERROR: Invalid INT value for column 'col_name'.`          |
| String exceeds VARCHAR size        | `ERROR: Value too long for column 'col_name'. Max allowed size: N` |

---

## SELECT

Retrieves rows from an existing table, with optional column filtering and WHERE clause.

### Syntax

```sql
-- All columns
SELECT * FROM table_name;

-- Specific columns
SELECT column1, column2 FROM table_name;

-- With WHERE filter
SELECT * FROM table_name WHERE column = value;
SELECT column1, column2 FROM table_name WHERE column = value;
```

### Column Selection

- Use `*` to retrieve all columns.
- List specific column names (comma-separated) to retrieve a subset.
- Column names in the select list must exist in the table — unknown columns produce an error.

### WHERE Clause

- Only the **equality operator `=`** is supported.
- One condition at a time (no `AND` / `OR`).
- **Search strategy is automatically chosen:**

| WHERE column              | Strategy                    | Complexity  |
|---------------------------|-----------------------------|-------------|
| First column (INT PK)     | B+ Tree point lookup        | O(log n)    |
| Any other column          | Full linear scan            | O(n)        |

- String values in WHERE do **not** need quotes in the query (they are matched as-is after tokenization).
- Column name and string value comparisons are **case-sensitive**.

### Examples

```sql
-- Select all rows, all columns
SELECT * FROM students;

-- Select specific columns
SELECT name, dept FROM employees;

-- WHERE on primary key (uses B+ Tree — fast)
SELECT * FROM students WHERE id = 1;

-- WHERE on non-primary column (uses linear scan)
SELECT * FROM employees WHERE dept = CSE;
```

### Errors

| Condition                         | Error Message                                              |
|-----------------------------------|------------------------------------------------------------|
| Table does not exist              | `Table "name" doesn't exist`                               |
| Unknown column in SELECT list     | `Error: Column 'col' does not exist in table.`             |
| Unknown column in WHERE clause    | `Error: Column 'col' does not exist in table 'name'.`      |
| Non-integer WHERE value on PK     | `Error: WHERE value 'v' cannot be parsed as INT...`        |
| Missing FROM clause               | `Syntax Error: Missing FROM clause or table name.`         |
| Incomplete WHERE clause           | `Syntax Error: Incomplete WHERE clause.`                   |
| Unsupported WHERE operator        | `Syntax Error: Only '=' is supported in WHERE clause.`     |

---

## DROP TABLE

Permanently deletes a table and all its data.

### Syntax

```sql
DROP TABLE table_name;
```

### Example

```sql
DROP TABLE students;
```

### Notes

- Removes the table directory (`./table/table_name/`) and all data files.
- Unregisters the table from the central `table_list`.
- **This operation is irreversible.**
- Extra tokens after the table name produce a syntax error.

### Errors

| Condition               | Error Message                                  |
|-------------------------|------------------------------------------------|
| Table does not exist    | `ERROR: Table 'name' does not exist.`          |
| Registry update fails   | `ERROR: Could not update table registry.`      |

---

## ⚠️ Syntax Limitations

| Limitation                                  | Detail                                              |
|---------------------------------------------|-----------------------------------------------------|
| WHERE supports only `=`                     | `<`, `>`, `!=`, `LIKE` are not supported            |
| No compound WHERE conditions                | `AND` / `OR` are not supported                      |
| No `UPDATE` statement                       | Rows cannot be modified after insertion             |
| No `ALTER TABLE`                            | Schema cannot be changed after creation             |
| First column must always be `INT`           | Enforced as Primary Key for the B+ Tree index       |
| String WHERE values are unquoted in queries | `WHERE name = Aditya` (quotes not needed/supported) |
| Query buffer capped at 1024 characters      | Queries longer than this will be truncated          |

---

## Quick Reference

```sql
SHOW TABLES;

CREATE TABLE students (id INT, name VARCHAR(50), dept VARCHAR(20));

INSERT INTO students VALUES (1, "Aditya", "CSE");

SELECT * FROM students;
SELECT name, dept FROM students;

SELECT * FROM students WHERE id = 1;
SELECT * FROM students WHERE dept = CSE;

DROP TABLE students;
```

---

*Column names and string values in WHERE are case-sensitive. All SQL keywords are case-insensitive.*