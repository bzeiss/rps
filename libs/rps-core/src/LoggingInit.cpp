#include <rps/core/LoggingInit.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cstdlib>
#include <iostream>

namespace rps::core {

void initLogging(const std::string& prefix, const std::string& logFileName,
                 const std::string& logDir) {
    // Read RPS_{PREFIX}_LOGGING (default: false)
    std::string envLogging = "RPS_" + prefix + "_LOGGING";
    std::string envLogLevel = "RPS_" + prefix + "_LOGLEVEL";

    bool loggingEnabled = false;
    std::string logLevel = "info";

    if (const char* val = std::getenv(envLogging.c_str())) {
        std::string sval(val);
        loggingEnabled = (sval == "true" || sval == "1" || sval == "yes");
    }

    if (const char* val = std::getenv(envLogLevel.c_str())) {
        logLevel = val;
    }

    try {
        if (loggingEnabled) {
            // Build log file path — use logDir param, or RPS_LOG_DIR env, or current directory
            std::string effectiveLogDir = logDir;
            if (effectiveLogDir.empty()) {
                if (const char* envDir = std::getenv("RPS_LOG_DIR")) {
                    effectiveLogDir = envDir;
                }
            }

            std::string logPath = logFileName;
            if (!effectiveLogDir.empty()) {
                logPath = effectiveLogDir + "/" + logFileName;
            }

            // Create stderr + file sinks
            auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, false);
            auto logger = std::make_shared<spdlog::logger>("rps",
                spdlog::sinks_init_list{stderrSink, fileSink});
            spdlog::set_default_logger(logger);
            spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

            // Parse level
            if (logLevel == "trace") spdlog::set_level(spdlog::level::trace);
            else if (logLevel == "debug") spdlog::set_level(spdlog::level::debug);
            else if (logLevel == "info") spdlog::set_level(spdlog::level::info);
            else if (logLevel == "warn") spdlog::set_level(spdlog::level::warn);
            else if (logLevel == "error") spdlog::set_level(spdlog::level::err);
            else spdlog::set_level(spdlog::level::info);

            // Flush on every info-level (or above) message immediately —
            // critical for short-lived processes (scanner) that exit before
            // the periodic flush fires.
            spdlog::flush_on(spdlog::level::info);
            // Also flush periodically for debug/trace messages
            spdlog::flush_every(std::chrono::seconds(3));

            spdlog::info("=====================================================================");
            spdlog::info("Logging initialized: file={}, level={}", logFileName, logLevel);
        } else {
            // Logging disabled: set level to off so all spdlog calls are no-ops
            spdlog::set_level(spdlog::level::off);
        }
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log init failed: " << ex.what() << std::endl;
    }
}

} // namespace rps::core
