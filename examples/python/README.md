# RPS Python TUI Client

A Python command-line client for the RPS Plugin Scanner gRPC server, featuring a rich terminal UI with per-worker progress bars.

## Setup

```bash
cd examples/python

# Create virtual environment
python -m venv .venv
source .venv/bin/activate  # or .venv\Scripts\activate on Windows

# Install dependencies
pip install -e .

# Generate gRPC stubs (generates and patches imports)
python generate_proto.py
```

## Usage

The client auto-spawns and kills `rps-server` unless `--server` is provided.

### Scan (auto-managed server)

```bash
# Incremental scan of all formats (default)
python -m rps_client scan

# Full scan, specific formats, limited
python -m rps_client scan --mode full --formats vst3,clap --limit 50

# Single plugin scan
python -m rps_client scan --scan /path/to/plugin.vst3

# Custom scan directories
python -m rps_client scan --scan-dir /path/to/plugins --jobs 8

# Verbose debug output
python -m rps_client scan --verbose
```

### Connect to an existing server

```bash
python -m rps_client --server localhost:50051 scan --formats vst3
python -m rps_client --server localhost:50051 status
python -m rps_client --server localhost:50051 shutdown
```

### Options

```
python -m rps_client --help
python -m rps_client scan --help
```

| Option | Description |
|---|---|
| `--server HOST:PORT` | Connect to existing server (skip auto-spawn) |
| `--server-bin PATH` | Path to `rps-server` binary |
| `--port PORT` | Server port (default: 50051) |
| `--db PATH` | Database file (default: rps-plugins.db) |

### Scan options

| Option | Description |
|---|---|
| `--formats`, `-f` | Comma-separated formats or `all` (default: `all`) |
| `--mode`, `-m` | `full` or `incremental` (default: `incremental`) |
| `--jobs`, `-j` | Parallel workers (default: 6) |
| `--retries`, `-r` | Max retries per plugin (default: 3) |
| `--timeout`, `-t` | Per-plugin timeout in ms (default: 120000) |
| `--limit`, `-l` | Max plugins to scan, 0 = unlimited |
| `--filter` | Filename substring filter |
| `--scan` | Single plugin file to scan |
| `--scan-dir` | Directories to scan (repeatable) |
| `--verbose`, `-v` | Enable debug logging |
