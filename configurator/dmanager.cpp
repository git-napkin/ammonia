#include "dmanager.h"
#include "process_utils.h"
#include <cstdio>
#include <string>
#include <unistd.h>

static const char kPlistPath[] = "/Library/LaunchDaemons/com.pluginplayground.grant.plist";
static const char kPlistSource[] = "/opt/pluginplayground/share/com.pluginplayground.grant.plist";

DaemonStatus DaemonManager::status() {
    if (access(kPlistPath, F_OK) != 0)
        return DaemonStatus::NotInstalled;

    const char *args[] = {
        "/bin/launchctl", "print", "system/com.pluginplayground.grant", nullptr};
    if (runArgv("/bin/launchctl", args))
        return DaemonStatus::InstalledRunning;
    return DaemonStatus::InstalledStopped;
}

static bool writeFallbackPlist(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fputs(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>Label</key>\n"
        "    <string>com.pluginplayground.grant</string>\n"
        "    <key>ProgramArguments</key>\n"
        "    <array>\n"
        "        <string>/opt/pluginplayground/bin/grant</string>\n"
        "    </array>\n"
        "    <key>RunAtLoad</key>\n"
        "    <true/>\n"
        "    <key>KeepAlive</key>\n"
        "    <dict>\n"
        "        <key>SuccessfulExit</key>\n"
        "        <false/>\n"
        "    </dict>\n"
        "    <key>ThrottleInterval</key>\n"
        "    <integer>10</integer>\n"
        "    <key>StandardOutPath</key>\n"
        "    <string>/var/log/pluginplayground/grant.log</string>\n"
        "    <key>StandardErrorPath</key>\n"
        "    <string>/var/log/pluginplayground/grant.err</string>\n"
        "</dict>\n"
        "</plist>\n",
        f);
    fclose(f);
    return true;
}

bool DaemonManager::install() {
    const char *src = kPlistSource;
    if (access(kPlistSource, R_OK) != 0) {
        if (!writeFallbackPlist("/tmp/com.pluginplayground.grant.plist"))
            return false;
        src = "/tmp/com.pluginplayground.grant.plist";
    }

    std::string script =
        std::string("do shell script \"")
        + "mkdir -p /var/log/pluginplayground && "
          "cp '" + src + "' '" + kPlistPath + "' && "
          "chown root:wheel '" + kPlistPath + "' && "
          "chmod 644 '" + kPlistPath + "' && "
          "(launchctl bootout system/com.pluginplayground.grant 2>/dev/null || true) && "
          "(launchctl bootstrap system '" + kPlistPath + "' || "
          "launchctl load '" + kPlistPath + "')"
        + "\" with administrator privileges";

    bool ok = runPrivilegedScript(script.c_str());
    if (src != kPlistSource)
        remove("/tmp/com.pluginplayground.grant.plist");
    return ok;
}

bool DaemonManager::uninstall() {
    return runPrivilegedScript(
        "do shell script \""
        "launchctl bootout system/com.pluginplayground.grant 2>/dev/null; "
        "launchctl unload /Library/LaunchDaemons/com.pluginplayground.grant.plist 2>/dev/null; "
        "rm -f /Library/LaunchDaemons/com.pluginplayground.grant.plist"
        "\" with administrator privileges");
}
