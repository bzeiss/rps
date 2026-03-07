# RPS - TODO List

This document tracks identified architectural improvements and known bugs across the project, ordered by priority.

## 🔴 Critical Priority (Bugs & Stability)

### 1. Fix `EXC_BAD_ACCESS` (Double-Free) in AudioUnit Scanner
- **File:** `apps/rps-pluginscanner/src/scanners/AuScanner.mm`
- **Description:** Around line 191, the code calls `CFRelease(paramInfo.cfNameString);`. Under CoreAudio's memory management rules (the "Get Rule"), the caller does not own this string reference. Releasing it causes an over-release, which can lead to hard crashes when scanning well-behaved Audio Units.
- **Action:** Remove the `CFRelease` call.

## 🟠 High Priority (Architecture & Performance)

### 2. Replace Internal IPC Mechanism (JSON/Named Queues -> Protobuf/Anonymous Pipes)
- **Files:** `libs/rps-ipc/src/Connection.cpp`, `libs/rps-ipc/include/rps/ipc/Connection.hpp`
- **Description:** Currently, communication between the Orchestrator and the Worker processes uses `boost::interprocess::message_queue` combined with Boost.JSON serialization. 
  - **Issue 1:** Named queues can remain orphaned in the OS if the orchestrator is hard-killed (e.g., `SIGKILL`), causing conflicts on subsequent runs.
  - **Issue 2:** JSON serialization is relatively slow and memory-intensive for large plugins with thousands of parameters.
- **Action:** Since the project already uses Protobuf for the external gRPC API, switch the internal IPC to also use Protobuf (or FlatBuffers). Migrate the transport layer from named queues to anonymous pipes (Standard I/O streams using `boost::process`) to guarantee OS-level cleanup upon process termination.

## 🟡 Medium Priority (Enhancements)

### 3. Exhaustive AudioUnit Parameter Extraction
- **File:** `apps/rps-pluginscanner/src/scanners/AuScanner.mm`
- **Description:** The AU scanner currently only queries `kAudioUnitScope_Global` for parameters. Many plugins (especially multi-bus effects and synths) register automatable parameters under `kAudioUnitScope_Input` or `kAudioUnitScope_Output`.
- **Action:** Iterate over input/output scopes and their respective buses to ensure all plugin parameters are captured.

### 4. Dynamic AudioUnit Registration for Arbitrary Paths
- **File:** `apps/rps-pluginscanner/src/scanners/AuScanner.mm`
- **Description:** `AudioComponentFindNext` only searches for components that have already been registered by macOS (typically residing in `/Library/Audio/Plug-Ins/Components`). If a user attempts to scan an unregistered `.component` bundle located elsewhere (e.g., `~/Downloads/`), the instantiation will fail.
- **Action:** If `AudioComponentFindNext` fails, dynamically register the bundle for the duration of the process using `AudioComponentRegister()` before attempting to find it again.

### 5. FourCC Parsing Fallback in AU `Info.plist`
- **File:** `apps/rps-pluginscanner/src/scanners/AuScanner.mm`
- **Description:** The `fourccToUInt32` lambda expects the string representation of a type/subtype to be exactly 4 characters long. Some legacy or poorly-formed plugins provide these codes as raw base-10 integers in the `Info.plist`. In these cases, the string conversion yields a numeric string (e.g., `"1096107074"`), which fails the length check.
- **Action:** Add a fallback mechanism to check if the string can be parsed as a raw integer (e.g., `[str longLongValue]`) if the length is not 4.

## 🔵 Epic / Long-Term Vision

### 6. Expand to Out-of-Process Plugin Engine (RPE)
- **Description:** Evolve the scanner into a full execution engine. The core GUI hosting infrastructure is already in place — `rps-pluginhost-vst3` and `rps-pluginhost-clap` can remotely instantiate plugins, embed native GUIs in SDL3 windows with ImGui preset sidebars, stream parameter changes, manage presets, and save/restore state via gRPC. The Python TUI (`open-gui` command) provides an interactive client with fuzzy plugin selection.
- **Remaining Work:**
  - **Audio Data Plane:** Add real-time audio buffer streaming. Do not use gRPC for this due to jitter/latency. Establish a Shared Memory Ring Buffer (local) or fast UDP socket (network) negotiated via gRPC.
  - **Plugin Chains:** Support hosting entire effects chains (e.g., EQ → Compressor) within a single worker process to prevent IPC context-switching overhead during the audio loop.
  - **Network Streaming:** Enable remote plugin hosting over the network (audio via UDP, control via gRPC).
  - **macOS/Linux GUI Hosting:** Extend the SDL3-based GUI hosting to macOS (Cocoa embedding) and Linux (X11/Wayland embedding).

