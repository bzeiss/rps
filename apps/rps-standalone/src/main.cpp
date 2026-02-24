#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <iostream>
#include <string>
#include <vector>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <rps/engine/ScanEngine.hpp>
#include <rps/engine/ConsoleScanObserver.hpp>

int main(int argc, char* argv[]) {
    namespace po = boost::program_options;
    
    po::options_description desc("RPS Standalone Options");
    desc.add_options()
        ("help,h", "Produce help message")
        ("scan-dir,d", po::value<std::vector<std::string>>()->multitoken(), "Directories to recursively scan for plugins")
        ("scan,s", po::value<std::string>(), "Single file to scan")
        ("scanner-bin,b", po::value<std::string>()->default_value(
#ifdef _WIN32
            "rps-pluginscanner.exe"
#else
            "rps-pluginscanner"
#endif
        ), "Path to the scanner binary")
        ("timeout,t", po::value<int>()->default_value(120000), "Timeout in ms per plugin (0 = no timeout, default: 2 min)")
        ("jobs,j", po::value<size_t>()->default_value(6), "Number of parallel scanner workers (default: 6)")
        ("formats,f", po::value<std::string>()->default_value("all"), "Comma-separated list of formats to scan (e.g. vst3,clap) or 'all'")
        ("filter", po::value<std::string>(), "Only scan plugins whose filename contains this string")
        ("limit,l", po::value<size_t>()->default_value(0), "Maximum number of plugins to scan (0 = unlimited)")
        ("verbose,v", "Enable verbose scanner output (plugin debug logs)")
        ("retries,r", po::value<size_t>()->default_value(3), "Number of retries for failed plugins (0 = no retries)")
        ("db", po::value<std::string>()->default_value("rps-plugins.db"), "Path to the output SQLite database file")
        ("mode,m", po::value<std::string>()->default_value("incremental"), "Scan mode: 'full' (rescan everything) or 'incremental' (skip unchanged)");
        
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Error parsing command line: " << e.what() << std::endl;
        return 1;
    }

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }

#ifdef _WIN32
    // Enable ANSI/VT100 escape code processing so plugin output renders correctly
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD outMode = 0, errMode = 0;
    if (hOut != INVALID_HANDLE_VALUE) { GetConsoleMode(hOut, &outMode); SetConsoleMode(hOut, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
    if (hErr != INVALID_HANDLE_VALUE) { GetConsoleMode(hErr, &errMode); SetConsoleMode(hErr, errMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
#endif

    // --- Build ScanConfig from CLI args ---
    rps::engine::ScanConfig config;
    config.verbose = vm.count("verbose") > 0;
    config.formats = vm["formats"].as<std::string>();
    config.scannerBin = vm["scanner-bin"].as<std::string>();
    config.dbPath = vm["db"].as<std::string>();
    config.mode = vm["mode"].as<std::string>();
    config.timeoutMs = vm["timeout"].as<int>();
    config.jobs = vm["jobs"].as<size_t>();
    config.retries = vm["retries"].as<size_t>();
    config.limit = vm["limit"].as<size_t>();

    if (vm.count("scan-dir")) {
        config.scanDirs = vm["scan-dir"].as<std::vector<std::string>>();
    }
    if (vm.count("scan")) {
        config.singlePlugin = vm["scan"].as<std::string>();
    }
    if (vm.count("filter")) {
        config.filter = vm["filter"].as<std::string>();
    }

    if (config.mode != "full" && config.mode != "incremental") {
        std::cerr << "Invalid --mode value: '" << config.mode << "'. Use 'full' or 'incremental'.\n";
        return 1;
    }
    if (config.jobs == 0) config.jobs = 1;

    std::cout << "RPS Standalone Scanner starting...\n";
    std::cout << "Output database: " << config.dbPath << "\n";

    rps::engine::ConsoleScanObserver observer(config.verbose);
    rps::engine::ScanEngine engine;

    auto summary = engine.runScan(config, &observer);

    // The observer already prints the completion summary via onScanCompleted.
    // Return non-zero if there were failures.
    return (summary.fail + summary.crash + summary.timeout > 0) ? 1 : 0;
}
