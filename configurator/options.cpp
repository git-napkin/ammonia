#include "options.h"
#include "file_utils.h"
#include "process_utils.h"
#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *optionsPath() {
    return "/opt/pluginplayground/current.options";
}

Options loadOptions() {
    CFDataRef data = fileRead(optionsPath());
    if (!data)
        return {};

    CFPropertyListRef plist = CFPropertyListCreateWithData(
        kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    if (!plist)
        return {};
    if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        CFRelease(plist);
        return {};
    }

    CFDictionaryRef dict = (CFDictionaryRef)plist;
    Options opts;

    auto getBool = [&](CFStringRef key, bool fallback) {
        CFBooleanRef val = (CFBooleanRef)CFDictionaryGetValue(dict, key);
        if (!val || CFGetTypeID(val) != CFBooleanGetTypeID())
            return fallback;
        return (bool)CFBooleanGetValue(val);
    };

    opts.useLegacyAmmonia = getBool(CFSTR("useLegacyAmmonia"), false);
    opts.disablePAC = getBool(CFSTR("disablePAC"), false);
    opts.pauseInjection = getBool(CFSTR("pauseInjection"), false);

    CFArrayRef enabledArr =
        (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("enabledTweaks"));
    if (enabledArr && CFGetTypeID(enabledArr) == CFArrayGetTypeID()) {
        CFIndex count = CFArrayGetCount(enabledArr);
        for (CFIndex i = 0; i < count; i++) {
            CFStringRef s = (CFStringRef)CFArrayGetValueAtIndex(enabledArr, i);
            if (s && CFGetTypeID(s) == CFStringGetTypeID())
                opts.enabledTweaks.push_back(cfToStr(s));
        }
    }

    CFRelease(dict);
    return opts;
}

static bool fixPermissions() {
    return runPrivilegedScript(
        "do shell script \""
        "mkdir -p /opt/pluginplayground && "
        "touch /opt/pluginplayground/current.options && "
        "chmod 666 /opt/pluginplayground/current.options"
        "\" with administrator privileges");
}

bool saveOptions(const Options &opts) {
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(dict, CFSTR("useLegacyAmmonia"),
        opts.useLegacyAmmonia ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(dict, CFSTR("disablePAC"),
        opts.disablePAC ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(dict, CFSTR("pauseInjection"),
        opts.pauseInjection ? kCFBooleanTrue : kCFBooleanFalse);

    CFMutableArrayRef enabledArr = CFArrayCreateMutable(
        kCFAllocatorDefault, (CFIndex)opts.enabledTweaks.size(),
        &kCFTypeArrayCallBacks);
    for (const auto &t : opts.enabledTweaks) {
        CFStringRef s = strToCF(t);
        if (s) {
            CFArrayAppendValue(enabledArr, s);
            CFRelease(s);
        }
    }
    CFDictionarySetValue(dict, CFSTR("enabledTweaks"), enabledArr);
    CFRelease(enabledArr);

    CFDataRef data = CFPropertyListCreateData(
        kCFAllocatorDefault, dict, kCFPropertyListXMLFormat_v1_0, 0, nullptr);
    CFRelease(dict);

    if (!data)
        return false;
    bool ok = fileWrite(optionsPath(), data);
    if (!ok && fixPermissions())
        ok = fileWrite(optionsPath(), data);
    CFRelease(data);
    if (ok)
        chmod(optionsPath(), 0666);
    return ok;
}
