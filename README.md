# RPS - Reliable Plugin Scanner

RPS is a modern, cross-platform audio plugin scanner designed from the ground up for extreme robustness and reliability. It supports scanning VST2, VST3, CLAP, AAX, AU, and LV2 formats on Windows, macOS, and Linux.

## Why RPS?

Scanning audio plugins is notoriously unreliable. Many plugins contain bugs, fail iLok license checks, or enter infinite loops during initialization. When a DAW or host application attempts to scan these plugins directly in its main process, a single bad plugin can crash the entire application.

RPS solves this by using a **multi-process architecture**:
- **`rps-pluginscanorchestrator`**: The coordinator. It manages a pool of worker processes, handles watchdogs/timeouts, and aggregates the results into a central SQLite database. If a plugin crashes, only the worker dies—the orchestrator logs the failure and moves on.
- **`rps-pluginscanner`**: The worker. It isolates the unsafe, third-party plugin code from the rest of your system.

## Project Axioms
1. **Primary Objective**: Robustness (Crash and stall isolation).
2. **Secondary Objective**: Performance / Speed (Parallel scanning).
3. **Third Objective**: Ease of use via central SQLite database (external tools can simply query the DB).
4. **Language**: Strictly Modern C++23.
5. **Dependencies**: Restricted to STL, Boost, SQLite, and Plugin SDKs.

## Documentation

For developers looking to contribute, understand the architecture, or build the project from source, please read the [Developer Guide](DEVELOPMENT.md).

## License

*(License details go here)*
