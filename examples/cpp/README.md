# RPS C++ gRPC Example

This example auto-spawns `rps-server` unless `--server` is provided.

On Windows, auto-spawned server processes are attached to a Job Object so they are reliably cleaned up when the C++ client exits.

Set `RPS_DEBUG_PROCESS_LIFECYCLE=1` to print spawn/attach/stop lifecycle logs.
