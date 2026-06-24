# MiniDB REST API Documentation

The `api/` folder contains the source code for the MiniDB REST API server. This server acts as a bridge, allowing external applications (written in languages like Node.js, Python, or Go) to interact with MiniDB using standard HTTP requests and JSON data.

---

## 🚀 Features

- **Language Agnostic**: Use MiniDB from any language that supports HTTP.
- **JSON Support**: Data is returned in a structured, easy-to-parse JSON format.
- **Multithreaded**: Built on the Crow microframework for high-performance concurrent handling.
- **Automatic Setup**: Dependencies are managed automatically via the Makefile.

---

## 🛠️ Compilation & Setup

The API depends on the **Crow** and **Asio** libraries. The project's Makefile is configured to download these automatically if they are missing.

### Compile the API Server
Run the following command from the project root:
```bash
make api
```
This will:
1. Build the core MiniDB object files.
2. Download Crow and Asio headers to `include/` (if not present).
3. Create an executable named `server` in the root directory.

---

## 🏃 Running the Server

Start the API server by running:
```bash
./server
```
By default, the server runs on **port 18080**.

---

## 📡 API Endpoints

### 1. Health Check
Verify if the API is running.

- **URL**: `/health`
- **Method**: `GET`
- **Success Response**: `MiniDB API is running!`

### 2. Get Table Contents
Retrieve all records from a specific table in JSON format.

- **URL**: `/table/<table_name>`
- **Method**: `GET`
- **Success Response**: A JSON array of objects, where each object represents a row.
- **Error Response**: `{"error": "Table not found"}` or `{"error": "Could not open data file"}`

### 3. Bulk Insert Data
Insert multiple records into a specific table at once.

- **URL**: `/bulk_insert/<table_name>`
- **Method**: `POST`
- **Body**: A JSON array of objects, where each object represents a row to be inserted.
- **Success Response**: `Bulk insertion triggered (check logs for success/failure)`
- **Error Response**: `Invalid JSON or empty array` or `Table not found`

---

## 💻 Example Usage

### Using cURL
```bash
curl http://localhost:18080/table/Students
```

### Using Node.js
A helper script is provided in `test/client_test.js`. To use it:
```bash
node test/client_test.js Students
```

---

## 📂 Directory Structure

- `api/server.cpp`: The main entry point for the REST API.
- `include/crow_all.h`: The Crow microframework (downloaded automatically).
- `include/asio/`: The Asio networking library (downloaded automatically).

---

## 🛠️ Summary of Recent Changes

### 🆕 Created Files
- **`include/sha256.h` & `src/sha256.cpp`**
  - `SHA256::hash()`: Main entry point for hashing strings.
  - `SHA256::transform()`, `init()`, `update()`, `final()`: Internal SHA-256 logic.
- **`include/auth.h` & `src/auth.cpp`**
  - `AuthManager::init()`: Initializes the authentication system and internal tables.
  - `AuthManager::register_user()`: Hashes and stores new user credentials.
  - `AuthManager::authenticate()`: Validates credentials against the stored hashes.
  - `AuthManager::create_session()`: Generates a 32-character random session token.
  - `AuthManager::validate_session()`: Checks if an API token is active.
  - `AuthManager::end_session()`: Invalidates a session token (logout).
- **`Documentation/auth.md`**
  - Comprehensive documentation of the security architecture.

### 🆙 Updated Files
- **`api/server.cpp`**
  - Added Endpoint: `POST /bulk_insert/<table_name>`
  - Added Endpoint: `POST /login`
  - Added Endpoint: `POST /logout`
  - Added Internal Helper: `is_authenticated()` (session validation middleware).
- **`include/insert.h` & `src/insert.cpp`**
  - `bulk_insert_command()`: Efficiently inserts multiple rows by keeping pages pinned and reusing resources.
- **`include/file_handler.h` & `src/file_handler.cpp`**
  - `open_system_file()`: Opens/creates internal database files in the `system/` directory.
  - `open_system_file_read()`: Read-only access to system files.
  - `store_system_meta_data()` & `fetch_system_meta_data()`: Metadata management for internal tables.
  - Updated `system_check()`: Now automatically ensures the `system/` folder exists.
- **`src/main.cpp`**
  - Updated `main()`: Added first-run interactive user setup and hashed password login check.
  - Updated `start_system()`: Integrated `AuthManager` initialization.
- **`tests/client_test.js`**
  - Added automated test case for the Bulk Insertion API endpoint.

---

## 📝 Technical Note: Session Tokens
The API uses token-based sessions. Clients first call `POST /login` with a username and password. After a successful login, the server returns a session token that must be sent on protected requests using the `X-Session-Token` header.

Each request is still handled independently at the HTTP level. For example, when `/table/<name>` is requested, the server validates the token, locates the table metadata, reads the data pages from disk, serializes the rows to JSON, and returns the response.
