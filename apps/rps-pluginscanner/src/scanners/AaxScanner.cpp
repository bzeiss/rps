#include <rps/scanner/AaxScanner.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cstdlib>

extern bool g_verbose;

namespace fs = boost::filesystem;

namespace {

// Trim leading/trailing whitespace
std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Count leading tabs in a line
int countLeadingTabs(const std::string& line) {
    int count = 0;
    for (char c : line) {
        if (c == '\t') count++;
        else break;
    }
    return count;
}

void logVerbose(const std::string& msg) {
    if (g_verbose) {
        std::cerr << "[AAX] " << msg << std::endl;
    }
}

} // anonymous namespace

namespace rps::scanner {

bool AaxScanner::canHandle(const boost::filesystem::path& pluginPath) const {
    std::string ext = pluginPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".aaxplugin" && fs::is_directory(pluginPath);
}

std::vector<boost::filesystem::path> AaxScanner::getCacheSearchPaths() const {
    std::vector<fs::path> paths;

#if defined(_WIN32)
    // PT 2023.12+ public cache
    paths.push_back("C:\\Users\\Public\\Pro Tools\\AAXPlugInCache");

    // Per-user Avid preferences
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        paths.push_back(fs::path(appdata) / "Avid" / "Pro Tools");
    }
#elif defined(__APPLE__)
    // PT 2023.12+ shared cache
    paths.push_back("/Users/Shared/Pro Tools/AAXPluginCache");

    // Per-user preferences
    const char* home = std::getenv("HOME");
    if (home) {
        paths.push_back(fs::path(home) / "Library" / "Preferences" / "Avid" / "Pro Tools");
    }
#endif

    return paths;
}

boost::filesystem::path AaxScanner::findCacheFile(const boost::filesystem::path& pluginPath) const {
    // Cache file naming convention: <pluginname>.aaxplugin_DAEPlugInRunner.plugincache.txt
    std::string pluginDirName = pluginPath.filename().string();
    std::string cacheFileName = pluginDirName + "_DAEPlugInRunner.plugincache.txt";

    logVerbose("Looking for cache file: " + cacheFileName);

    for (const auto& searchDir : getCacheSearchPaths()) {
        if (!fs::exists(searchDir) || !fs::is_directory(searchDir)) continue;

        fs::path candidate = searchDir / cacheFileName;
        if (fs::exists(candidate)) {
            logVerbose("Found cache file: " + candidate.string());
            return candidate;
        }

        // Some cache directories might use subdirectories; do a shallow search
        try {
            for (fs::directory_iterator it(searchDir), end; it != end; ++it) {
                if (it->path().filename().string() == cacheFileName) {
                    logVerbose("Found cache file: " + it->path().string());
                    return it->path();
                }
            }
        } catch (...) {
            // Permission errors, etc.
        }
    }

    logVerbose("No cache file found for: " + pluginDirName);
    return {};
}

std::pair<std::string, int64_t> AaxScanner::parseFourCCField(const std::string& value) {
    // Format: "Apsd (1097888612)" or just "(0)" for empty
    std::string trimmed = trim(value);
    std::string fourcc;
    int64_t num = 0;

    size_t parenOpen = trimmed.find('(');
    size_t parenClose = trimmed.find(')');

    if (parenOpen != std::string::npos && parenClose != std::string::npos && parenClose > parenOpen) {
        fourcc = trim(trimmed.substr(0, parenOpen));
        std::string numStr = trimmed.substr(parenOpen + 1, parenClose - parenOpen - 1);
        try { num = std::stoll(numStr); } catch (...) {}
    } else {
        fourcc = trimmed;
    }

    // Sanitize: some FourCC codes contain raw high bytes (e.g. 0xD2 0xC0 0xAA 0xA5)
    // that are invalid UTF-8. Replace non-ASCII bytes with hex representation
    // to keep the string JSON-safe. The numeric ID is what matters for PTSL.
    bool hasNonAscii = false;
    for (unsigned char c : fourcc) {
        if (c > 127) { hasNonAscii = true; break; }
    }
    if (hasNonAscii && !fourcc.empty()) {
        std::string hex = "0x";
        for (unsigned char c : fourcc) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02X", c);
            hex += buf;
        }
        fourcc = hex;
    }

    return {fourcc, num};
}

std::vector<AaxScanner::AaxPluginVariant> AaxScanner::parseCacheFile(const boost::filesystem::path& cacheFile) const {
    std::vector<AaxPluginVariant> variants;
    std::ifstream file(cacheFile.string());
    if (!file.is_open()) return variants;

    std::string line;
    AaxPluginVariant current;
    bool inPlugin = false;

    while (std::getline(file, line)) {
        int tabs = countLeadingTabs(line);
        std::string trimmedLine = trim(line);

        if (trimmedLine.empty()) continue;

        // Parse "key: value" pairs
        size_t colonPos = trimmedLine.find(':');
        if (colonPos == std::string::npos) continue;

        std::string key = trim(trimmedLine.substr(0, colonPos));
        std::string val = trim(trimmedLine.substr(colonPos + 1));

        // Detect plugin section start (tab level 1: "plugin: N")
        if (tabs == 1 && key == "plugin") {
            if (inPlugin) {
                variants.push_back(current);
            }
            current = AaxPluginVariant{};
            try { current.index = std::stoi(val); } catch (...) {}
            inPlugin = true;
            continue;
        }

        // Plugin-level fields (tab level 2)
        if (inPlugin && tabs == 2) {
            if (key == "name") {
                current.name = val;
            } else if (key == "PIName") {
                current.piName = val;
            } else if (key == "MfgName") {
                current.mfgName = val;
            } else if (key == "manufacturerID") {
                auto [s, n] = parseFourCCField(val);
                current.manufacturerId = s;
                current.manufacturerIdNum = n;
            } else if (key == "productID") {
                auto [s, n] = parseFourCCField(val);
                current.productId = s;
                current.productIdNum = n;
            } else if (key == "plugInID") {
                auto [s, n] = parseFourCCField(val);
                current.pluginId = s;
                current.pluginIdNum = n;
            } else if (key == "EffectID") {
                current.effectId = val;
            } else if (key == "PlugInType") {
                try { current.pluginType = std::stoi(val); } catch (...) {}
            } else if (key == "NumInputs") {
                try { current.numInputs = std::stoi(val); } catch (...) {}
            } else if (key == "NumOutputs") {
                try { current.numOutputs = std::stoi(val); } catch (...) {}
            } else if (key == "StemFormats.Input") {
                try { current.stemFormatInput = std::stoi(val); } catch (...) {}
            } else if (key == "StemFormats.Output") {
                try { current.stemFormatOutput = std::stoi(val); } catch (...) {}
            } else if (key == "StemFormats.SideChainIn") {
                try { current.stemFormatSidechain = std::stoi(val); } catch (...) {}
            }
        }
    }

    // Don't forget the last plugin section
    if (inPlugin) {
        variants.push_back(current);
    }

    logVerbose("Parsed " + std::to_string(variants.size()) + " variant(s) from cache");
    return variants;
}

const AaxScanner::AaxPluginVariant* AaxScanner::pickBestVariant(const std::vector<AaxPluginVariant>& variants) const {
    if (variants.empty()) return nullptr;

    // Priority: PlugInType=3 (Native) > PlugInType=1 (AudioSuite) > others
    // Within same type, prefer higher I/O count (stereo over mono)
    const AaxPluginVariant* best = nullptr;
    int bestScore = -1;

    for (const auto& v : variants) {
        int score = 0;
        if (v.pluginType == 3) score = 1000;       // Native
        else if (v.pluginType == 1) score = 500;    // AudioSuite
        else if (v.pluginType == 8) score = 100;    // DSP

        // Prefer stereo (2 channels) over mono (1)
        score += v.numInputs + v.numOutputs;

        if (score > bestScore) {
            bestScore = score;
            best = &v;
        }
    }

    return best;
}

rps::ipc::ScanResult AaxScanner::scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) {
    rps::ipc::ScanResult result;
    result.format = "aax";

    progressCb(10, "Searching for Pro Tools cache file...");

    fs::path cacheFile = findCacheFile(pluginPath);

    if (cacheFile.empty() || !fs::exists(cacheFile)) {
        // Fallback: basic metadata from directory name
        progressCb(50, "No cache file found, using fallback metadata");
        std::string pluginName = pluginPath.stem().string(); // "AGT" from "AGT.aaxplugin"
        result.name = pluginName;
        result.scanMethod = "fallback";
        result.uid = "";
        logVerbose("Fallback scan for: " + pluginName);
        progressCb(100, "Fallback scan complete");
        return result;
    }

    progressCb(30, "Parsing cache file...");
    auto variants = parseCacheFile(cacheFile);

    if (variants.empty()) {
        std::string pluginName = pluginPath.stem().string();
        result.name = pluginName;
        result.scanMethod = "fallback";
        progressCb(100, "Cache file empty, fallback complete");
        return result;
    }

    progressCb(60, "Selecting best variant...");

    // Pick the best variant for main plugin metadata
    const auto* best = pickBestVariant(variants);
    if (!best) best = &variants[0];

    // Populate main ScanResult from best variant
    result.name = best->piName.empty() ? best->name : best->piName;
    result.vendor = best->mfgName;
    result.uid = best->effectId;
    result.numInputs = static_cast<uint32_t>(best->numInputs);
    result.numOutputs = static_cast<uint32_t>(best->numOutputs);
    result.scanMethod = "protools-cache";

    logVerbose("Best variant: name=\"" + result.name + "\" vendor=\"" + result.vendor +
               "\" uid=\"" + result.uid + "\" type=" + std::to_string(best->pluginType) +
               " I/O=" + std::to_string(best->numInputs) + "/" + std::to_string(best->numOutputs));

    progressCb(80, "Packing variant data...");

    // Pack ALL variants into extraData for the aax_plugins table
    result.extraData["aax_variant_count"] = std::to_string(variants.size());

    for (size_t i = 0; i < variants.size(); ++i) {
        const auto& v = variants[i];
        std::string prefix = "aax_v" + std::to_string(i) + "_";

        result.extraData[prefix + "manufacturer_id"] = v.manufacturerId;
        result.extraData[prefix + "manufacturer_id_num"] = std::to_string(v.manufacturerIdNum);
        result.extraData[prefix + "product_id"] = v.productId;
        result.extraData[prefix + "product_id_num"] = std::to_string(v.productIdNum);
        result.extraData[prefix + "plugin_id"] = v.pluginId;
        result.extraData[prefix + "plugin_id_num"] = std::to_string(v.pluginIdNum);
        result.extraData[prefix + "effect_id"] = v.effectId;
        result.extraData[prefix + "plugin_type"] = std::to_string(v.pluginType);
        result.extraData[prefix + "stem_format_input"] = std::to_string(v.stemFormatInput);
        result.extraData[prefix + "stem_format_output"] = std::to_string(v.stemFormatOutput);
        result.extraData[prefix + "stem_format_sidechain"] = std::to_string(v.stemFormatSidechain);
    }

    progressCb(100, "AAX scan complete");
    return result;
}

} // namespace rps::scanner
