# Market Data Engine -- Environment Setup

Reproduces the full toolchain from a clean Windows 11 machine.

## Prerequisites

### 1. Visual Studio 2022 Build Tools (MSVC C++ compiler)

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools `
  --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Installs to: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\`

Verify:
```
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\<version>\bin\Hostx64\x64\cl.exe"
```
Should print MSVC version info.

### 2. CMake

```powershell
winget install Kitware.CMake --scope machine
```

Installs to: `C:\Program Files\CMake\bin\cmake.exe`

Verify: `cmake --version`

### 3. MySQL 8 Server

```powershell
winget install Oracle.MySQL
```

**Note on version:** the original setup used MySQL Server 8.4
(service `MySQL84`). That install's data directory was removed during a
reinstall on 2026-08-06 (Program Files/ProgramData cleanup), and the
`winget` package now resolves to **MySQL Server 8.0** (service `MySQL80`)
instead. Both service names may exist side by side on a machine that's been
through this once (`MySQL84` stopped/orphaned, `MySQL80` running) -- check
`Get-Service -Name "MySQL*"` to see what's actually present before assuming
either path below. The install path and service name below match the
current 8.0 install; adjust if a future reinstall lands on a different
version again.

Installs to: `C:\Program Files\MySQL\MySQL Server 8.0\`

After install, initialize and start the server:

```powershell
# Initialize data directory (requires admin)
Start-Process -FilePath "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysqld.exe" `
  -ArgumentList "--initialize-insecure" -Verb RunAs -Wait

# Install as Windows service
Start-Process -FilePath "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysqld.exe" `
  -ArgumentList "--install","MySQL80" -Verb RunAs -Wait

# Start the service
Start-Process -FilePath "net" -ArgumentList "start","MySQL80" -Verb RunAs -Wait
```

Then create the database, user, **and** the isolated test database
(`market_data_engine_test` -- required by `tests/test_persistence.cpp`,
which drops/recreates tables on every test and must never share a schema
with real loaded data; see that file's top-of-file comment):

```sql
-- Connect: & "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -u root -p
CREATE DATABASE IF NOT EXISTS market_data_engine;
CREATE USER IF NOT EXISTS 'mde_user'@'localhost' IDENTIFIED BY '<your-mde-password>';
GRANT ALL PRIVILEGES ON market_data_engine.* TO 'mde_user'@'localhost';

CREATE DATABASE IF NOT EXISTS market_data_engine_test;
GRANT ALL PRIVILEGES ON market_data_engine_test.* TO 'mde_user'@'localhost';

FLUSH PRIVILEGES;
```

The engine connects as `mde_user` on `localhost:33060` (X Protocol). The
password is read from the `MDE_DB_PASSWORD` environment variable at runtime
(never hardcoded in source).

### 4. Set `MDE_DB_PASSWORD` environment variable

The engine reads the MySQL app-user password from the `MDE_DB_PASSWORD`
environment variable. This must be set before running any executable.

**Current session only (PowerShell):**
```powershell
$env:MDE_DB_PASSWORD = "<your-mde-password>"
```

**Current session only (cmd.exe):**
```cmd
set MDE_DB_PASSWORD=<your-mde-password>
```

**Persist across sessions (PowerShell, requires restart of shell):**
```powershell
setx MDE_DB_PASSWORD "<your-mde-password>"
```

**Persist via GUI:** System Properties > Advanced > Environment Variables >
User variables > New > Name: `MDE_DB_PASSWORD`, Value: `<your-mde-password>`.

### 5. vcpkg (C++ package manager)

```bash
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

Set environment variable: `VCPKG_ROOT=C:\vcpkg`

No separate `vcpkg install` step is needed. The project uses manifest mode
(`vcpkg.json` at the project root), so CMake configure automatically
installs dependencies into a project-local `vcpkg_installed/` directory.
Expect ~19 minutes on the first configure (compiles OpenSSL, protobuf,
mysql-connector-cpp, etc.).

## Build and Run

```bash
# Configure (from project root)
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release

# Set the database password (if not already persisted -- see step 4 above)
# PowerShell:  $env:MDE_DB_PASSWORD = "<your-mde-password>"
# cmd.exe:     set MDE_DB_PASSWORD=<your-mde-password>

# Run the test suite (45 tests need no DB; 16 need MySQL + MDE_DB_PASSWORD)
.\build\Release\mde_tests.exe
```

## Versions

| Component              | Version       |
|------------------------|---------------|
| MSVC (cl.exe)          | 19.44.35228   |
| CMake                  | 4.4.1         |
| MySQL Server           | 8.4.9 originally; **8.0.46** after the 2026-08-06 reinstall |
| mysql-connector-cpp    | 9.7.0         |
| vcpkg                  | 2026-07-27    |
| Windows SDK            | 10.0.26100.0  |
