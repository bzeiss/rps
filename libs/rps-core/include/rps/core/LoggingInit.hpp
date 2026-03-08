#pragma once

#include <string>

namespace rps::core {

/// Initialize spdlog from environment variables.
/// 
/// Reads:
///   RPS_{PREFIX}_LOGGING   = "true" | "false" (default: false)
///   RPS_{PREFIX}_LOGLEVEL   = "trace"|"debug"|"info"|"warn"|"error" (default: info)
///
/// When logging is enabled, creates a file sink writing to `logFileName`.
/// When disabled, sets spdlog level to off (all calls become no-ops).
/// Always writes to stderr as well so ProcessPool drainer captures output.
///
/// @param prefix      Environment variable prefix, e.g. "SERVER", "PLUGINSCANNER", "PLUGINHOST"
/// @param logFileName File to write, e.g. "rps-server.log", "rps-pluginscanner.worker_3.log"
/// @param logDir      Optional directory to place the log file in (default: current directory)
void initLogging(const std::string& prefix, const std::string& logFileName,
                 const std::string& logDir = "");

} // namespace rps::core
