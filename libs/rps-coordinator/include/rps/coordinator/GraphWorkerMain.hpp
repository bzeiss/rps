#pragma once

#include <rps/gui/IPluginGuiHost.hpp>
#include <rps/coordinator/Graph.hpp>
#include <rps/coordinator/GraphExecutor.hpp>

#include <functional>
#include <memory>
#include <string>

namespace rps::coordinator {

/// Factory function type: creates an IPluginGuiHost for a given format.
using HostFactory = std::function<
    std::unique_ptr<rps::gui::IPluginGuiHost>(const std::string& format)>;

/// Entry point for graph-based multi-plugin processing mode.
/// This is the graph equivalent of GuiWorkerMain::run().
///
/// Usage:
///   rps-pluginhost --graph '{"graph": {...}}' [--audio-shm <name>]
///
/// The graph JSON follows the GraphSerializer format.
/// Each PluginNode in the graph will be instantiated using the factory.
class GraphWorkerMain {
public:
    /// Run the graph worker.
    /// @param argc/argv  Same command-line arguments as the process.
    /// @param factory    Creates format-specific plugin hosts.
    /// @return  Process exit code (0 = success).
    static int run(int argc, char* argv[], HostFactory factory);
};

} // namespace rps::coordinator
