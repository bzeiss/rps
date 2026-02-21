#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <rps/core/PluginDiscovery.hpp>
#include <rps/orchestrator/ProcessPool.hpp>

int main(int argc, char* argv[]) {
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;
    
    po::options_description desc("Orchestrator Options");
    desc.add_options()
        ("help,h", "Produce help message")
        ("scan-dir,d", po::value<std::vector<std::string>>()->multitoken(), "Directories to recursively scan for plugins")
        ("scan,s", po::value<std::string>(), "Single file to scan")
        ("scanner-bin,b", po::value<std::string>()->default_value("rps-pluginscanner.exe"), "Path to the scanner binary")
        ("timeout,t", po::value<int>()->default_value(10000), "Timeout in milliseconds for the scanner to respond")
        ("jobs,j", po::value<size_t>()->default_value(std::thread::hardware_concurrency()), "Number of parallel workers");
        
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

    std::vector<fs::path> pluginsToScan;

    if (vm.count("scan-dir")) {
        auto dirs = vm["scan-dir"].as<std::vector<std::string>>();
        auto found = rps::core::PluginDiscovery::findPlugins(dirs);
        pluginsToScan.insert(pluginsToScan.end(), found.begin(), found.end());
    }

    if (vm.count("scan")) {
        fs::path p(vm["scan"].as<std::string>());
        // If it's a dummy test string like CRASH_ME, allow it through
        if (p.string() == "CRASH_ME" || p.string() == "HANG_ME" || fs::exists(p)) {
            pluginsToScan.push_back(p);
        } else {
            std::cerr << "Warning: Single scan file does not exist: " << p << "\n";
        }
    }

    if (pluginsToScan.empty()) {
        std::cerr << "No plugins to scan. Provide --scan-dir or --scan.\n";
        return 1;
    }

    std::string scannerBin = vm["scanner-bin"].as<std::string>();
    int timeoutMs = vm["timeout"].as<int>();
    size_t numWorkers = vm["jobs"].as<size_t>();

    if (numWorkers == 0) numWorkers = 1;

    std::cout << "RPS Plugin Scan Orchestrator starting...\n";
    std::cout << "Discovered " << pluginsToScan.size() << " plugins.\n";
    std::cout << "Starting process pool with " << numWorkers << " workers (timeout: " << timeoutMs << "ms)...\n";
    std::cout << "--------------------------------------------------------\n";

    std::vector<rps::orchestrator::ScanJob> jobs;
    for (const auto& p : pluginsToScan) {
        jobs.push_back({ p, scannerBin, timeoutMs });
    }

    rps::orchestrator::ProcessPool pool(numWorkers);
    pool.runJobs(jobs);

    std::cout << "--------------------------------------------------------\n";
    std::cout << "Orchestrator shutdown complete.\n";
    return 0;
}

