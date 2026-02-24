#include <rps/scanner/Lv2Scanner.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <map>
#include <vector>
#include <cstring>
#include <cstdio>
#include <lv2/core/lv2.h>

extern bool g_verbose;

namespace fs = boost::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Lightweight Turtle (TTL) metadata extractor for LV2 bundles
// ---------------------------------------------------------------------------

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Read an entire file into a string
std::string readFileContents(const fs::path& filePath) {
    std::ifstream f(filePath.string());
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Extract a quoted string: "value" or 'value'
std::string extractQuoted(const std::string& s, size_t startPos = 0) {
    size_t q1 = s.find('"', startPos);
    if (q1 == std::string::npos) {
        q1 = s.find('\'', startPos);
        if (q1 == std::string::npos) return "";
        size_t q2 = s.find('\'', q1 + 1);
        if (q2 == std::string::npos) return "";
        return s.substr(q1 + 1, q2 - q1 - 1);
    }
    size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return s.substr(q1 + 1, q2 - q1 - 1);
}

// Decode percent-encoded URI (e.g. %20 -> space)
std::string percentDecode(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            unsigned int val = 0;
            if (std::sscanf(s.c_str() + i + 1, "%2x", &val) == 1) {
                result += static_cast<char>(val);
                i += 2;
                continue;
            }
        }
        result += s[i];
    }
    return result;
}

// Extract angle-bracketed URI: <value>
std::string extractAngleBracket(const std::string& s, size_t startPos = 0) {
    size_t a1 = s.find('<', startPos);
    if (a1 == std::string::npos) return "";
    size_t a2 = s.find('>', a1 + 1);
    if (a2 == std::string::npos) return "";
    return s.substr(a1 + 1, a2 - a1 - 1);
}

// Strip the LV2 class prefix to get a short category name
std::string classToCategory(const std::string& cls) {
    // Map LV2 plugin classes to human-readable categories
    static const std::map<std::string, std::string> classMap = {
        {"InstrumentPlugin", "Instrument"},
        {"GeneratorPlugin", "Generator"},
        {"OscillatorPlugin", "Generator"},
        {"AnalyserPlugin", "Analyzer"},
        {"AmplifierPlugin", "Amplifier"},
        {"CompressorPlugin", "Compressor"},
        {"ExpanderPlugin", "Dynamics"},
        {"GatePlugin", "Dynamics"},
        {"LimiterPlugin", "Dynamics"},
        {"DynamicsPlugin", "Dynamics"},
        {"DelayPlugin", "Delay"},
        {"ReverbPlugin", "Reverb"},
        {"ChorusPlugin", "Modulation"},
        {"FlangerPlugin", "Modulation"},
        {"PhaserPlugin", "Modulation"},
        {"ModulatorPlugin", "Modulation"},
        {"FilterPlugin", "Filter"},
        {"AllpassPlugin", "Filter"},
        {"BandpassPlugin", "Filter"},
        {"BandstopPlugin", "Filter"},
        {"HighpassPlugin", "Filter"},
        {"LowpassPlugin", "Filter"},
        {"EQPlugin", "EQ"},
        {"MultiEQPlugin", "EQ"},
        {"ParaEQPlugin", "EQ"},
        {"DistortionPlugin", "Distortion"},
        {"WaveshaperPlugin", "Distortion"},
        {"SpatialPlugin", "Spatial"},
        {"SpectralPlugin", "Spectral"},
        {"PitchPlugin", "Pitch"},
        {"UtilityPlugin", "Utility"},
        {"ConverterPlugin", "Utility"},
        {"FunctionPlugin", "Utility"},
        {"MixerPlugin", "Mixer"},
        {"SimulatorPlugin", "Simulator"},
        {"EnvelopePlugin", "Envelope"},
        {"ConstantPlugin", "Utility"},
        {"CombPlugin", "Filter"},
    };

    // Extract the class name from a full URI or prefixed form
    std::string name = cls;

    // Handle full URI: http://lv2plug.in/ns/lv2core#InstrumentPlugin
    size_t hash = name.rfind('#');
    if (hash != std::string::npos) {
        name = name.substr(hash + 1);
    }

    // Handle prefixed form: lv2:InstrumentPlugin
    size_t colon = name.find(':');
    if (colon != std::string::npos) {
        name = name.substr(colon + 1);
    }

    auto it = classMap.find(name);
    if (it != classMap.end()) return it->second;

    // Strip "Plugin" suffix as fallback
    if (name.size() > 6 && name.substr(name.size() - 6) == "Plugin") {
        return name.substr(0, name.size() - 6);
    }

    return name;
}

// ---------------------------------------------------------------------------
// Manifest parser: extract plugin URIs, binary paths, seeAlso references
// ---------------------------------------------------------------------------
struct ManifestEntry {
    std::string pluginUri;
    std::string binary;     // relative path to .so
    std::string seeAlso;    // relative path to .ttl with full metadata
};

std::vector<ManifestEntry> parseManifest(const fs::path& bundlePath) {
    std::vector<ManifestEntry> entries;
    fs::path manifestPath = bundlePath / "manifest.ttl";
    if (!fs::exists(manifestPath)) return entries;

    std::string content = readFileContents(manifestPath);
    if (content.empty()) return entries;

    // Resolve prefixes
    std::map<std::string, std::string> prefixes;
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.rfind("@prefix", 0) == 0) {
            // @prefix lv2: <http://lv2plug.in/ns/lv2core#> .
            size_t nameStart = 7; // after "@prefix"
            while (nameStart < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[nameStart]))) nameStart++;
            size_t nameEnd = trimmed.find(':', nameStart);
            if (nameEnd != std::string::npos) {
                std::string prefixName = trimmed.substr(nameStart, nameEnd - nameStart);
                std::string uri = extractAngleBracket(trimmed, nameEnd);
                prefixes[prefixName] = uri;
            }
        }
    }

    // Now parse subject blocks to find lv2:Plugin entries
    // We do a second pass, tracking the current subject
    stream.clear();
    stream.str(content);

    std::string currentSubject;
    ManifestEntry currentEntry;
    bool isPlugin = false;

    auto resolveUri = [&](const std::string& term) -> std::string {
        if (term.empty()) return "";
        if (term[0] == '<' && term.back() == '>') {
            return term.substr(1, term.size() - 2);
        }
        size_t colon = term.find(':');
        if (colon != std::string::npos) {
            std::string prefix = term.substr(0, colon);
            std::string local = term.substr(colon + 1);
            auto it = prefixes.find(prefix);
            if (it != prefixes.end()) {
                return it->second + local;
            }
        }
        return term;
    };

    auto commitEntry = [&]() {
        if (isPlugin && !currentEntry.pluginUri.empty()) {
            entries.push_back(currentEntry);
        }
        currentEntry = ManifestEntry{};
        isPlugin = false;
    };

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.rfind("@prefix", 0) == 0 || trimmed[0] == '#') continue;

        // Detect new subject (starts with < or a prefixed name at column 0, not whitespace)
        if (!std::isspace(static_cast<unsigned char>(line[0])) && !line.empty()) {
            // Check if this is a new subject
            std::string potentialSubject;
            if (trimmed[0] == '<') {
                potentialSubject = extractAngleBracket(trimmed);
            } else if (trimmed.find(':') != std::string::npos && trimmed[0] != '@') {
                // Could be prefixed:name (with or without predicates on same line)
                size_t spacePos = trimmed.find_first_of(" \t");
                std::string term = (spacePos != std::string::npos)
                    ? trimmed.substr(0, spacePos) : trimmed;
                potentialSubject = resolveUri(term);
            }
            if (!potentialSubject.empty() && potentialSubject != currentSubject) {
                commitEntry();
                currentSubject = potentialSubject;
                currentEntry.pluginUri = currentSubject;
            }
        }

        // Check predicates
        if (trimmed.find("a ") != std::string::npos || trimmed.find("a\t") != std::string::npos) {
            if (trimmed.find("lv2:Plugin") != std::string::npos ||
                trimmed.find(LV2_CORE__Plugin) != std::string::npos) {
                isPlugin = true;
            }
        }

        if (trimmed.find("lv2:binary") != std::string::npos) {
            std::string val = extractAngleBracket(trimmed, static_cast<size_t>(trimmed.find("lv2:binary")));
            if (!val.empty()) currentEntry.binary = percentDecode(val);
        }

        if (trimmed.find("rdfs:seeAlso") != std::string::npos) {
            std::string val = extractAngleBracket(trimmed, static_cast<size_t>(trimmed.find("rdfs:seeAlso")));
            if (!val.empty()) currentEntry.seeAlso = percentDecode(val);
        }
    }
    commitEntry();

    return entries;
}

// ---------------------------------------------------------------------------
// Plugin TTL parser: extract metadata from the plugin description file
// ---------------------------------------------------------------------------
struct PortInfo {
    uint32_t index = 0;
    std::string name;
    std::string symbol;
    bool isInput = false;
    bool isOutput = false;
    bool isAudio = false;
    bool isControl = false;
    double defaultValue = 0.0;
};

struct PluginMetadata {
    std::string uri;
    std::string name;
    std::string vendor;
    std::string description;
    std::string version;
    std::string category;
    int minorVersion = -1;
    int microVersion = -1;
    std::vector<PortInfo> ports;
};

PluginMetadata parsePluginTtl(const fs::path& ttlPath, const std::string& pluginUri) {
    PluginMetadata meta;
    meta.uri = pluginUri;

    if (!fs::exists(ttlPath)) return meta;

    std::string content = readFileContents(ttlPath);
    if (content.empty()) return meta;

    // We need to find the block for our plugin URI and extract metadata.
    // Strategy: scan line by line, detect when we're in the plugin's subject block,
    // and extract properties. Also handle inline port blocks with [ ... ].

    std::istringstream stream(content);
    std::string line;

    // Track state
    bool inPluginBlock = false;
    bool inPortBlock = false;
    bool inMaintainerBlock = false;
    bool inReleaseBlock = false;
    int bracketDepth = 0;
    PortInfo currentPort;

    // Helper to check if a line's subject matches our plugin URI
    auto isPluginSubject = [&](const std::string& trimmed) -> bool {
        // Check for <full-uri>
        if (trimmed.find(pluginUri) != std::string::npos) {
            // Make sure it's actually the subject (at the start, in angle brackets)
            std::string check = "<" + pluginUri + ">";
            if (trimmed.find(check) == 0) return true;
        }
        return false;
    };

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        if (trimmed.rfind("@prefix", 0) == 0) continue;

        // Track bracket depth for inline blank nodes
        for (char c : trimmed) {
            if (c == '[') bracketDepth++;
            else if (c == ']') bracketDepth--;
        }

        // Detect plugin subject block start
        if (!inPluginBlock && isPluginSubject(trimmed)) {
            inPluginBlock = true;
            continue;
        }

        // Detect end of plugin block (line starts with a new subject at col 0, or a terminal '.')
        if (inPluginBlock && bracketDepth <= 0) {
            // A line that starts a new subject (non-whitespace, not a predicate continuation)
            if (!std::isspace(static_cast<unsigned char>(line[0])) && !line.empty() &&
                line[0] != ']' && trimmed.back() != ';' && trimmed.back() != ',') {
                // Could be end of block if line ends with '.'
                if (trimmed == ".") {
                    inPluginBlock = false;
                    continue;
                }
                // A new subject starts — end our block
                if (trimmed[0] == '<' || (std::isalpha(static_cast<unsigned char>(trimmed[0])) && trimmed.find(':') != std::string::npos)) {
                    if (!isPluginSubject(trimmed)) {
                        inPluginBlock = false;
                        continue;
                    }
                }
            }
        }

        if (!inPluginBlock) continue;

        // --- Extract metadata from the plugin block ---

        // Plugin classes (category): "a lv2:Plugin, lv2:InstrumentPlugin"
        if ((trimmed.find("a ") == 0 || trimmed.find("a\t") == 0) && meta.category.empty()) {
            // Check for specific plugin classes
            for (const char* cls : {"InstrumentPlugin", "GeneratorPlugin", "OscillatorPlugin",
                "AnalyserPlugin", "AmplifierPlugin", "CompressorPlugin", "ExpanderPlugin",
                "GatePlugin", "LimiterPlugin", "DynamicsPlugin", "DelayPlugin", "ReverbPlugin",
                "ChorusPlugin", "FlangerPlugin", "PhaserPlugin", "ModulatorPlugin",
                "FilterPlugin", "AllpassPlugin", "BandpassPlugin", "BandstopPlugin",
                "HighpassPlugin", "LowpassPlugin", "EQPlugin", "MultiEQPlugin", "ParaEQPlugin",
                "DistortionPlugin", "WaveshaperPlugin", "SpatialPlugin", "SpectralPlugin",
                "PitchPlugin", "UtilityPlugin", "ConverterPlugin", "FunctionPlugin",
                "MixerPlugin", "SimulatorPlugin", "EnvelopePlugin", "ConstantPlugin", "CombPlugin"}) {
                if (trimmed.find(cls) != std::string::npos) {
                    meta.category = classToCategory(cls);
                    break;
                }
            }
        }

        // doap:name "Plugin Name"
        if (trimmed.find("doap:name") != std::string::npos && meta.name.empty()) {
            meta.name = extractQuoted(trimmed, static_cast<size_t>(trimmed.find("doap:name")));
        }

        // doap:description "..."
        if (trimmed.find("doap:description") != std::string::npos && meta.description.empty()) {
            meta.description = extractQuoted(trimmed, static_cast<size_t>(trimmed.find("doap:description")));
        }

        // doap:maintainer [ ... foaf:name "Vendor" ... ]
        if (trimmed.find("doap:maintainer") != std::string::npos) {
            inMaintainerBlock = true;
        }
        if (inMaintainerBlock && trimmed.find("foaf:name") != std::string::npos && meta.vendor.empty()) {
            meta.vendor = extractQuoted(trimmed, static_cast<size_t>(trimmed.find("foaf:name")));
        }
        if (inMaintainerBlock && trimmed.find(']') != std::string::npos) {
            inMaintainerBlock = false;
        }

        // doap:release [ doap:revision "1.2.3" ]
        if (trimmed.find("doap:release") != std::string::npos) {
            inReleaseBlock = true;
        }
        if (inReleaseBlock && trimmed.find("doap:revision") != std::string::npos && meta.version.empty()) {
            meta.version = extractQuoted(trimmed, static_cast<size_t>(trimmed.find("doap:revision")));
        }
        if (inReleaseBlock && trimmed.find(']') != std::string::npos) {
            inReleaseBlock = false;
        }

        // lv2:minorVersion / lv2:microVersion
        if (trimmed.find("lv2:minorVersion") != std::string::npos) {
            std::string val = trimmed.substr(trimmed.find("lv2:minorVersion") + 16);
            val = trim(val);
            // Remove trailing ; or .
            while (!val.empty() && (val.back() == ';' || val.back() == '.')) val.pop_back();
            val = trim(val);
            try { meta.minorVersion = std::stoi(val); } catch (...) {}
        }
        if (trimmed.find("lv2:microVersion") != std::string::npos) {
            std::string val = trimmed.substr(trimmed.find("lv2:microVersion") + 16);
            val = trim(val);
            while (!val.empty() && (val.back() == ';' || val.back() == '.')) val.pop_back();
            val = trim(val);
            try { meta.microVersion = std::stoi(val); } catch (...) {}
        }

        // --- Port parsing ---
        // Port blocks start with [ and contain "a lv2:InputPort, lv2:AudioPort" etc.
        // They are nested inside the plugin block under "lv2:port"

        // Detect port block start
        if (trimmed.find('[') != std::string::npos &&
            (inPortBlock || trimmed.find("lv2:port") != std::string::npos ||
             (trimmed[0] == ']' && trimmed.find('[') != std::string::npos))) {
            // If we were already in a port block and hit "] , [" that means new port
            if (inPortBlock && trimmed.find(']') != std::string::npos) {
                // Commit previous port
                meta.ports.push_back(currentPort);
                currentPort = PortInfo{};
            }
            inPortBlock = true;
        }

        if (inPortBlock) {
            // Port types
            if (trimmed.find("lv2:InputPort") != std::string::npos ||
                trimmed.find("InputPort") != std::string::npos) {
                currentPort.isInput = true;
            }
            if (trimmed.find("lv2:OutputPort") != std::string::npos ||
                trimmed.find("OutputPort") != std::string::npos) {
                currentPort.isOutput = true;
            }
            if (trimmed.find("lv2:AudioPort") != std::string::npos ||
                trimmed.find("AudioPort") != std::string::npos) {
                currentPort.isAudio = true;
            }
            if (trimmed.find("lv2:ControlPort") != std::string::npos ||
                trimmed.find("ControlPort") != std::string::npos) {
                currentPort.isControl = true;
            }

            // lv2:index N
            if (trimmed.find("lv2:index") != std::string::npos) {
                std::string val = trimmed.substr(trimmed.find("lv2:index") + 9);
                val = trim(val);
                while (!val.empty() && (val.back() == ';' || val.back() == '.')) val.pop_back();
                val = trim(val);
                try { currentPort.index = static_cast<uint32_t>(std::stoul(val)); } catch (...) {}
            }

            // lv2:name "Port Name"
            if (trimmed.find("lv2:name") != std::string::npos) {
                currentPort.name = extractQuoted(trimmed, static_cast<size_t>(trimmed.find("lv2:name")));
            }

            // lv2:symbol "port_symbol"
            if (trimmed.find("lv2:symbol") != std::string::npos) {
                currentPort.symbol = extractQuoted(trimmed, static_cast<size_t>(trimmed.find("lv2:symbol")));
            }

            // lv2:default N
            if (trimmed.find("lv2:default") != std::string::npos) {
                std::string val = trimmed.substr(trimmed.find("lv2:default") + 11);
                val = trim(val);
                while (!val.empty() && (val.back() == ';' || val.back() == '.')) val.pop_back();
                val = trim(val);
                try { currentPort.defaultValue = std::stod(val); } catch (...) {}
            }

            // End of last port block (closing ] followed by . at bracket depth 0)
            if (bracketDepth <= 0 && trimmed.find(']') != std::string::npos) {
                meta.ports.push_back(currentPort);
                currentPort = PortInfo{};
                inPortBlock = false;
            }
        }
    }

    // Build version string from minor/micro if doap:revision wasn't found
    if (meta.version.empty() && meta.minorVersion >= 0) {
        meta.version = std::to_string(meta.minorVersion);
        if (meta.microVersion >= 0) {
            meta.version += "." + std::to_string(meta.microVersion);
        }
    }

    return meta;
}

} // anonymous namespace

namespace rps::scanner {

bool Lv2Scanner::canHandle(const boost::filesystem::path& pluginPath) const {
    std::string ext = pluginPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".lv2" && fs::is_directory(pluginPath);
}

rps::ipc::ScanResult Lv2Scanner::scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) {
    auto logStage = [&](const std::string& stage) {
        if (g_verbose) {
            std::cerr << "[lv2] " << pluginPath.filename().string() << ": " << stage << std::endl;
        }
    };

    progressCb(5, "Parsing LV2 manifest...");
    logStage("Parsing manifest.ttl...");

    auto manifestEntries = parseManifest(pluginPath);
    if (manifestEntries.empty()) {
        throw std::runtime_error("SKIP: No LV2 plugin entries found in manifest.ttl: " + pluginPath.string());
    }

    logStage("Found " + std::to_string(manifestEntries.size()) + " plugin(s) in manifest.");

    // Use the first plugin entry (most bundles contain one plugin)
    const auto& entry = manifestEntries[0];
    logStage("Plugin URI: " + entry.pluginUri);
    if (!entry.binary.empty()) logStage("Binary: " + entry.binary);
    if (!entry.seeAlso.empty()) logStage("seeAlso: " + entry.seeAlso);

    progressCb(20, "Checking binary exists...");

    // Verify the binary exists
    if (!entry.binary.empty()) {
        fs::path binaryPath = pluginPath / entry.binary;
        if (!fs::exists(binaryPath)) {
            throw std::runtime_error("SKIP: LV2 binary not found: " + binaryPath.string());
        }
        logStage("Binary exists: " + binaryPath.string());
    }

    progressCb(30, "Parsing plugin metadata...");

    // Parse the plugin's TTL file for detailed metadata
    PluginMetadata meta;
    meta.uri = entry.pluginUri;

    if (!entry.seeAlso.empty()) {
        fs::path ttlPath = pluginPath / entry.seeAlso;
        logStage("Parsing " + entry.seeAlso + "...");
        meta = parsePluginTtl(ttlPath, entry.pluginUri);
    }

    // If seeAlso didn't provide metadata, try parsing manifest.ttl itself
    // (some simple plugins put everything in manifest.ttl)
    if (meta.name.empty()) {
        logStage("No metadata from seeAlso, trying manifest.ttl...");
        auto manifestMeta = parsePluginTtl(pluginPath / "manifest.ttl", entry.pluginUri);
        if (!manifestMeta.name.empty()) meta = manifestMeta;
    }

    progressCb(70, "Building scan result...");

    // Build the ScanResult
    rps::ipc::ScanResult result;
    result.format = "lv2";
    result.scanMethod = "ttl";

    // Name: prefer doap:name, fallback to bundle directory name
    result.name = meta.name.empty() ? pluginPath.stem().string() : meta.name;
    result.vendor = meta.vendor;
    result.version = meta.version;
    result.description = meta.description;
    result.uid = meta.uri; // LV2 uses URIs as unique identifiers
    result.category = meta.category;

    logStage("Name: \"" + result.name + "\"");
    logStage("Vendor: \"" + result.vendor + "\"");
    logStage("Version: \"" + result.version + "\"");
    logStage("Category: \"" + result.category + "\"");
    logStage("URI: " + result.uid);

    // Count audio I/O and extract control parameters
    uint32_t audioInputs = 0;
    uint32_t audioOutputs = 0;

    for (const auto& port : meta.ports) {
        if (port.isAudio && port.isInput) audioInputs++;
        if (port.isAudio && port.isOutput) audioOutputs++;
        if (port.isControl && port.isInput) {
            result.parameters.push_back({
                port.index,
                port.name.empty() ? port.symbol : port.name,
                port.defaultValue
            });
        }
    }

    result.numInputs = audioInputs;
    result.numOutputs = audioOutputs;

    logStage("Audio I/O: " + std::to_string(audioInputs) + " in, " + std::to_string(audioOutputs) + " out");
    logStage("Control parameters: " + std::to_string(result.parameters.size()));

    // Store extra LV2-specific data
    result.extraData["lv2_uri"] = meta.uri;
    if (!entry.binary.empty()) {
        result.extraData["lv2_binary"] = entry.binary;
    }
    if (manifestEntries.size() > 1) {
        result.extraData["lv2_plugin_count"] = std::to_string(manifestEntries.size());
    }

    progressCb(100, "Done.");
    return result;
}

} // namespace rps::scanner
