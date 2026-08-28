#pragma once
#include <string>
#include <vector>

struct TweakOptions {
    std::vector<std::string> blacklistedApps;
    std::vector<std::string> frameworkDependencies;
    std::vector<std::string> processWhitelist;
};

struct TweakData {
    std::string name;
    bool disabled = false;
};

std::string tweaksDir();
std::vector<TweakData> scanTweaks();
bool toggleTweak(const std::string& name);
bool hasDeveloperTools();
bool packageTweak(const std::string& name);
bool installTweakFromDialog();
TweakOptions loadTweakOptions(const std::string& name);
bool saveTweakOptions(const std::string& name, const TweakOptions& opts);
bool ensurePermissions();

enum class SipStatus {
    Unknown = 0,
    Enabled = 1,
    Disabled = 2,
    PartiallyDisabled = 3,
};

SipStatus checkSipStatus();
