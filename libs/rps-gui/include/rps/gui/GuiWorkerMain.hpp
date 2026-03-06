#pragma once

#include <rps/gui/IPluginGuiHost.hpp>
#include <string>
#include <memory>

namespace rps::gui {

/// Shared main logic for all plugin host worker binaries.
/// Handles CLI parsing, IPC connection, and the GUI event loop.
class GuiWorkerMain {
public:
    /// Run the worker. This is the entry point for all rps-pluginhost-* binaries.
    /// @param argc argc from main()
    /// @param argv argv from main()
    /// @param host The format-specific GUI host implementation. Ownership is taken.
    /// @return Exit code (0 = success).
    static int run(int argc, char* argv[], std::unique_ptr<IPluginGuiHost> host);
};

} // namespace rps::gui
