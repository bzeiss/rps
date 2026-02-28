# RPS Java Client Example

This is a professional Java client for the Reliable Plugin Scanner (RPS) gRPC service. It demonstrates how to connect to the server, start a scan, and handle streaming scan events using a modern Terminal UI (TUI).

## Prerequisites

- **Java 25+** (OpenJDK)
- **Maven** (mvn)

## Features

- **Fancy TUI**: Powered by JLine 3, featuring an overall progress bar and individual real-time worker status rows.
- **Auto-Spawning**: Automatically locates and starts the `rps-server` if it's not already running.
- **Modern gRPC**: Uses the latest stable gRPC and Protobuf libraries.
- **JDK 25 Support**: Specifically configured to run cleanly on modern Java versions without "Unsafe" warnings.

## Building the Client

To build the client and generate the gRPC stubs from the `rps.proto` definition:

```bash
cd examples/java
mvn clean compile
```

The `rps.proto` file is automatically copied from the project root into the Java source tree during the build process.

## Running the Client

To run the client, use the `exec:exec` goal. This ensures the correct JVM flags are passed to provide a clean, warning-free output:

```bash
mvn exec:exec
```

The client will:
1. Check if an `rps-server` is running (and start one if needed).
2. Connect to the server at `127.0.0.1:50051`.
3. Start an incremental scan of all plugin formats.
4. Display real-time progress using the colored terminal UI.
