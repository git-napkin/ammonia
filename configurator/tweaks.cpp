#include "tweaks.h"
#include "file_utils.h"
#include "options.h"
#include "process_utils.h"
#include <copyfile.h>
#include <removefile.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syslimits.h>
#include <unordered_set>
#include <unistd.h>

static std::string tweaksDirFrom(const Options &opts) {
    return opts.useLegacyAmmonia ? "/private/var/ammonia/core/tweaks"
                                 : "/opt/pluginplayground/tweaks";
}

std::string tweaksDir() {
    return tweaksDirFrom(loadOptions());
}

static std::string tweakPath(const std::string &dir, const std::string &name) {
    return dir + "/" + name;
}

static std::string tweakOptionsPath(const std::string &name) {
    return tweaksDir() + "/" + name + ".options";
}

static bool endsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return false;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

std::vector<TweakData> scanTweaks() {
    Options opts = loadOptions();
    std::string dir = tweaksDirFrom(opts);
    std::vector<TweakData> result;

    DIR *d = opendir(dir.c_str());
    if (!d)
        return result;

    std::vector<std::string> disabled_markers;
    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name(entry->d_name);
        if (endsWith(name, ".dylib.disabled")) {
            disabled_markers.push_back(name);
            continue;
        }
        if (endsWith(name, ".dylib"))
            result.push_back({name, false});
    }
    closedir(d);

    if (!disabled_markers.empty()) {
        std::unordered_set<std::string> markers(disabled_markers.begin(),
                                                disabled_markers.end());
        opts.enabledTweaks.clear();
        for (const auto &t : result) {
            if (markers.count(t.name + ".disabled") == 0)
                opts.enabledTweaks.push_back(t.name);
        }
        for (const auto &name : disabled_markers)
            unlink(tweakPath(dir, name).c_str());
        saveOptions(opts);
    }

    std::unordered_set<std::string> enabled(opts.enabledTweaks.begin(),
                                            opts.enabledTweaks.end());
    for (auto &t : result)
        t.disabled = enabled.find(t.name) == enabled.end();
    return result;
}

static bool isTweakSafe(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
    if (st.st_uid != 0)
        return false;
    if (st.st_mode & (S_IWGRP | S_IWOTH))
        return false;
    return true;
}

bool toggleTweak(const std::string &name) {
    Options opts = loadOptions();
    auto it = std::find(opts.enabledTweaks.begin(), opts.enabledTweaks.end(), name);
    if (it != opts.enabledTweaks.end()) {
        opts.enabledTweaks.erase(it);
    } else {
        if (!isTweakSafe(tweakPath(tweaksDirFrom(opts), name)))
            return false;
        opts.enabledTweaks.push_back(name);
    }
    return saveOptions(opts);
}

bool hasDeveloperTools() {
    const char *argv[] = {"/usr/bin/xcode-select", "-p", nullptr};
    return runArgv("/usr/bin/xcode-select", argv);
}

static bool is_safe_tweak_name(const std::string &name) {
    if (name.empty() || name.size() > 255)
        return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' &&
            c != '_')
            return false;
    }
    return true;
}

bool packageTweak(const std::string &name) {
    if (!is_safe_tweak_name(name))
        return false;

    std::string dir = tweaksDir();
    std::string dylibPath = tweakPath(dir, name);
    if (access(dylibPath.c_str(), R_OK) != 0)
        return false;

    std::string staging = "/tmp/plugintweak_" + name;
    std::string tweakDest = staging + "/opt/pluginplayground/tweaks";
    if (!mkdir_p(tweakDest.c_str()))
        return false;

    std::string destFile = tweakDest + "/" + name;
    if (copyfile(dylibPath.c_str(), destFile.c_str(), nullptr, COPYFILE_ALL) != 0) {
        removefile(staging.c_str(), nullptr, REMOVEFILE_RECURSIVE);
        return false;
    }

    std::string pkgName = "/tmp/" + name + ".pkg";
    std::string pkgIdent = "com.pluginplayground.tweak." + name;
    const char *pkgbuildArgs[] = {
        "/usr/bin/pkgbuild", "--root", staging.c_str(),
        "--identifier", pkgIdent.c_str(),
        "--version", "1.0.0",
        "--install-location", "/",
        pkgName.c_str(), nullptr};
    bool ok = runArgv("/usr/bin/pkgbuild", pkgbuildArgs);
    removefile(staging.c_str(), nullptr, REMOVEFILE_RECURSIVE);
    return ok;
}

TweakOptions loadTweakOptions(const std::string &name) {
    TweakOptions opts;
    CFDataRef data = fileRead(tweakOptionsPath(name).c_str());
    if (!data)
        return opts;

    CFPropertyListRef plist = CFPropertyListCreateWithData(
        kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    if (!plist)
        return opts;
    if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        CFRelease(plist);
        return opts;
    }

    CFDictionaryRef dict = (CFDictionaryRef)plist;

    auto readArray = [&](CFStringRef key, std::vector<std::string> &out) {
        CFArrayRef arr = (CFArrayRef)CFDictionaryGetValue(dict, key);
        if (!arr || CFGetTypeID(arr) != CFArrayGetTypeID())
            return;
        CFIndex count = CFArrayGetCount(arr);
        for (CFIndex i = 0; i < count; i++) {
            CFStringRef s = (CFStringRef)CFArrayGetValueAtIndex(arr, i);
            if (s && CFGetTypeID(s) == CFStringGetTypeID())
                out.push_back(cfToStr(s));
        }
    };

    readArray(CFSTR("blacklistedApps"), opts.blacklistedApps);
    readArray(CFSTR("frameworkDependencies"), opts.frameworkDependencies);

    CFRelease(dict);
    return opts;
}

bool saveTweakOptions(const std::string &name, const TweakOptions &opts) {
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 2,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    auto writeArray = [&](CFStringRef key, const std::vector<std::string> &items) {
        CFMutableArrayRef arr =
            CFArrayCreateMutable(kCFAllocatorDefault, (CFIndex)items.size(),
                                 &kCFTypeArrayCallBacks);
        for (const auto &item : items) {
            CFStringRef s = strToCF(item);
            CFArrayAppendValue(arr, s);
            CFRelease(s);
        }
        CFDictionarySetValue(dict, key, arr);
        CFRelease(arr);
    };

    writeArray(CFSTR("blacklistedApps"), opts.blacklistedApps);
    writeArray(CFSTR("frameworkDependencies"), opts.frameworkDependencies);

    CFDataRef data = CFPropertyListCreateData(
        kCFAllocatorDefault, dict, kCFPropertyListXMLFormat_v1_0, 0, nullptr);
    CFRelease(dict);

    if (!data)
        return false;
    bool ok = fileWrite(tweakOptionsPath(name).c_str(), data);
    CFRelease(data);
    return ok;
}

bool ensurePermissions() {
    std::string dir = tweaksDir();
    if (access(dir.c_str(), R_OK | W_OK) == 0)
        return true;

    std::string script =
        "display dialog \"Plugin Playground needs permission to write to:\\n"
        + dir + "\\n\\n"
        "Click Fix to authenticate and fix permissions.\" "
        "buttons {\"Exit\", \"Fix\"} default button \"Fix\" with icon caution";

    const char *dialogArgs[] = {"/usr/bin/osascript", "-e", script.c_str(), nullptr};
    int status = -1;
    std::string result = runCapture("/usr/bin/osascript", dialogArgs, &status);
    if (status != 0)
        return false;
    if (result.find("button returned:Exit") != std::string::npos)
        return false;

    std::string fixScript =
        "do shell script \"mkdir -p " + dir + " && chmod 777 " + dir +
        "\" with administrator privileges";
    return runPrivilegedScript(fixScript.c_str()) &&
           access(dir.c_str(), R_OK | W_OK) == 0;
}

SipStatus checkSipStatus() {
    const char *args[] = {"/usr/bin/csrutil", "status", nullptr};
    std::string result = runCapture("/usr/bin/csrutil", args);

    if (result.find("System Integrity Protection status: disabled.") !=
        std::string::npos)
        return SipStatus::Disabled;
    if (result.find("Debugging Restrictions: disabled") != std::string::npos)
        return SipStatus::PartiallyDisabled;
    if (result.find("System Integrity Protection status: enabled.") !=
        std::string::npos)
        return SipStatus::Enabled;
    return SipStatus::Unknown;
}
