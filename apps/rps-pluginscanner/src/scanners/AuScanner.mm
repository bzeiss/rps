#include <rps/scanner/AuScanner.hpp>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <boost/filesystem.hpp>

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#endif

extern bool g_verbose;

namespace rps::scanner {

bool AuScanner::canHandle(const boost::filesystem::path& pluginPath) const {
#ifdef __APPLE__
    std::string ext = pluginPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return (ext == ".component" || ext == ".app" || ext == ".appex") && boost::filesystem::is_directory(pluginPath);
#else
    (void)pluginPath;
    return false;
#endif
}

rps::ipc::ScanResult AuScanner::scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) {
    rps::ipc::ScanResult result;
    result.format = "au";
    
#ifdef __APPLE__
    auto logStage = [&](const std::string& stage) {
        if (g_verbose) {
            std::cerr << "[au] " << pluginPath.filename().string() << ": " << stage << std::endl;
        }
    };

    progressCb(10, "Loading bundle...");
    
    @autoreleasepool {
        NSString* bundlePath = [NSString stringWithUTF8String:pluginPath.string().c_str()];
        NSBundle* bundle = [NSBundle bundleWithPath:bundlePath];
        if (!bundle) {
            throw std::runtime_error("Failed to load NSBundle from path: " + pluginPath.string());
        }

        progressCb(20, "Reading Info.plist...");
        NSDictionary* infoDict = [bundle infoDictionary];
        if (!infoDict) {
            throw std::runtime_error("No Info.plist found in bundle.");
        }

        NSArray* audioComponents = infoDict[@"AudioComponents"];
        if (!audioComponents || [audioComponents count] == 0) {
            throw std::runtime_error("No AudioComponents array found in Info.plist.");
        }

        // We scan the first component found
        NSDictionary* componentDict = audioComponents[0];
        NSString* typeStr = componentDict[@"type"];
        NSString* subtypeStr = componentDict[@"subtype"];
        NSString* manufacturerStr = componentDict[@"manufacturer"];
        NSString* nameStr = componentDict[@"name"];
        NSString* versionStr = componentDict[@"version"];

        auto fourccToUInt32 = [](NSString* str) -> UInt32 {
            if (!str || [str length] != 4) return 0;
            const char* cstr = [str UTF8String];
            return (UInt32(cstr[0]) << 24) | (UInt32(cstr[1]) << 16) | (UInt32(cstr[2]) << 8) | UInt32(cstr[3]);
        };
        
        UInt32 type = fourccToUInt32(typeStr);
        UInt32 subtype = fourccToUInt32(subtypeStr);
        UInt32 manufacturer = fourccToUInt32(manufacturerStr);

        result.extraData["au_type"] = typeStr ? [typeStr UTF8String] : "";
        result.extraData["au_subtype"] = subtypeStr ? [subtypeStr UTF8String] : "";
        result.extraData["au_manufacturer"] = manufacturerStr ? [manufacturerStr UTF8String] : "";
        
        result.uid = result.extraData["au_type"] + "-" + result.extraData["au_subtype"] + "-" + result.extraData["au_manufacturer"];
        
        if (nameStr) {
            // "Manufacturer: PluginName"
            NSArray* parts = [nameStr componentsSeparatedByString:@": "];
            if ([parts count] == 2) {
                result.vendor = [parts[0] UTF8String];
                result.name = [parts[1] UTF8String];
            } else {
                result.name = [nameStr UTF8String];
                result.vendor = "Unknown Vendor";
            }
        } else {
            result.name = pluginPath.stem().string();
            result.vendor = "Unknown Vendor";
        }

        if (versionStr) {
            result.version = [versionStr UTF8String];
        } else {
            result.version = "1.0.0";
        }
        
        // Category heuristic based on AU type
        if (result.extraData["au_type"] == "aumu") result.category = "Instrument";
        else if (result.extraData["au_type"] == "aufx") result.category = "Effect";
        else if (result.extraData["au_type"] == "aumf") result.category = "Effect|Instrument";
        else result.category = "Effect";

        logStage("Component details: type=" + result.extraData["au_type"] + " subtype=" + result.extraData["au_subtype"] + " manufacturer=" + result.extraData["au_manufacturer"]);

        AudioComponentDescription desc = {};
        desc.componentType = type;
        desc.componentSubType = subtype;
        desc.componentManufacturer = manufacturer;
        desc.componentFlags = 0;
        desc.componentFlagsMask = 0;

        progressCb(30, "Finding AudioComponent...");
        AudioComponent comp = AudioComponentFindNext(NULL, &desc);
        if (!comp) {
            // Sometimes it's not registered or we're running without AudioComponent scanning, try fallback
            logStage("AudioComponentFindNext could not find the component (is it registered?) -> Fallback to Info.plist metadata");
            result.scanMethod = "plist";
            return result;
        }

        UInt32 compVersion = 0;
        if (AudioComponentGetVersion(comp, &compVersion) == noErr) {
            int major = (compVersion & 0xFFFF0000) >> 16;
            int minor = (compVersion & 0x0000FF00) >> 8;
            int dot = (compVersion & 0x000000FF);
            result.version = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(dot);
        }

        progressCb(50, "Instantiating AudioUnit...");
        AudioUnit au = nullptr;
        OSStatus err = AudioComponentInstanceNew(comp, &au);
        if (err != noErr || !au) {
            logStage("Failed to instantiate AudioUnit (err " + std::to_string(err) + "). Proceeding with basic Info.plist metadata.");
            result.scanMethod = "plist";
            return result;
        }

        result.scanMethod = "audiounit";

        progressCb(70, "Querying I/O and parameters...");
        // Get Inputs
        UInt32 busCount = 0;
        UInt32 size = sizeof(busCount);
        if (AudioUnitGetProperty(au, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &busCount, &size) == noErr) {
            result.numInputs = busCount * 2; // Assuming stereo per bus as a simple heuristic if formats fail
            
            // Try to get actual stream formats
            UInt32 totalInputChannels = 0;
            for (UInt32 i = 0; i < busCount; ++i) {
                AudioStreamBasicDescription format;
                UInt32 formatSize = sizeof(format);
                if (AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, i, &format, &formatSize) == noErr) {
                    totalInputChannels += format.mChannelsPerFrame;
                } else {
                    totalInputChannels += 2; // fallback
                }
            }
            if (totalInputChannels > 0) result.numInputs = totalInputChannels;
        }

        // Get Outputs
        size = sizeof(busCount);
        if (AudioUnitGetProperty(au, kAudioUnitProperty_ElementCount, kAudioUnitScope_Output, 0, &busCount, &size) == noErr) {
            result.numOutputs = busCount * 2; 

            UInt32 totalOutputChannels = 0;
            for (UInt32 i = 0; i < busCount; ++i) {
                AudioStreamBasicDescription format;
                UInt32 formatSize = sizeof(format);
                if (AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, i, &format, &formatSize) == noErr) {
                    totalOutputChannels += format.mChannelsPerFrame;
                } else {
                    totalOutputChannels += 2; // fallback
                }
            }
            if (totalOutputChannels > 0) result.numOutputs = totalOutputChannels;
        }

        // Get Parameters
        AudioUnitParameterID* paramIDs = nullptr;
        UInt32 paramListSize = 0;
        if (AudioUnitGetPropertyInfo(au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &paramListSize, nullptr) == noErr && paramListSize > 0) {
            paramIDs = (AudioUnitParameterID*)malloc(paramListSize);
            if (AudioUnitGetProperty(au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, paramIDs, &paramListSize) == noErr) {
                int numParams = paramListSize / sizeof(AudioUnitParameterID);
                logStage("Found " + std::to_string(numParams) + " parameters.");
                
                for (int i = 0; i < numParams; ++i) {
                    AudioUnitParameterInfo paramInfo = {};
                    UInt32 infoSize = sizeof(paramInfo);
                    if (AudioUnitGetProperty(au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, paramIDs[i], &paramInfo, &infoSize) == noErr) {
                        std::string paramName;
                        if (paramInfo.cfNameString) {
                            paramName = [(NSString*)paramInfo.cfNameString UTF8String];
                            CFRelease(paramInfo.cfNameString);
                        } else {
                            paramName = "Param " + std::to_string(paramIDs[i]);
                        }
                        
                        result.parameters.push_back({
                            static_cast<uint32_t>(paramIDs[i]),
                            paramName,
                            static_cast<double>(paramInfo.defaultValue)
                        });
                    }
                }
            }
            free(paramIDs);
        }

        progressCb(90, "Metadata extraction complete.");
        logStage("Scan complete. Skipping AudioComponentInstanceDispose to prevent crashes.");
        // AudioComponentInstanceDispose(au); // skip, similar to VST3
    }
#else
    (void)pluginPath;
    (void)progressCb;
    throw std::runtime_error("AU scanner only supported on Apple platforms");
#endif

    progressCb(100, "Done.");
    return result;
}

} // namespace rps::scanner