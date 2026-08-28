#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static inline std::string cfToStr(CFStringRef s) {
    if (!s)
        return {};
    char buf[4096];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8))
        return buf;
    return {};
}

static inline CFStringRef strToCF(const std::string &s) {
    return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(),
                                     kCFStringEncodingUTF8);
}

static inline CFDataRef fileRead(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nullptr;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return nullptr;
    }
    rewind(f);
    if (len == 0) {
        fclose(f);
        return CFDataCreate(kCFAllocatorDefault, nullptr, 0);
    }
    auto *buf = static_cast<UInt8 *>(malloc((size_t)len));
    if (!buf) {
        fclose(f);
        return nullptr;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return nullptr;
    }
    fclose(f);
    return CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, buf, (CFIndex)len,
                                       kCFAllocatorMalloc);
}

static inline bool fileWrite(const char *path, CFDataRef data) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    auto len = (size_t)CFDataGetLength(data);
    bool ok = fwrite(CFDataGetBytePtr(data), 1, len, f) == len;
    fclose(f);
    return ok;
}
