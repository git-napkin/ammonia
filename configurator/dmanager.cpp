#include "dmanager.h"
#include "process_utils.h"
#include <cstdio>
#include <unistd.h>

static const char kPlistPath[] = "/Library/LaunchDaemons/com.pluginplayground.grant.plist";

DaemonStatus DaemonManager::status() {
    if (access(kPlistPath, F_OK) != 0)
        return DaemonStatus::NotInstalled;

    const char *args[] = {
        "/bin/launchctl", "print", "system/com.pluginplayground.grant", nullptr};
    if (runArgv("/bin/launchctl", args))
        return DaemonStatus::InstalledRunning;
    return DaemonStatus::InstalledStopped;
}

bool DaemonManager::install() {
    const char *plist =
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
        "    <true/>\n"
        "    <key>StandardOutPath</key>\n"
        "    <string>/var/log/pluginplayground/grant.log</string>\n"
        "    <key>StandardErrorPath</key>\n"
        "    <string>/var/log/pluginplayground/grant.err</string>\n"
        "</dict>\n"
        "</plist>\n";

    FILE *f = fopen("/tmp/com.pluginplayground.grant.plist", "w");
    if (!f)
        return false;
    fputs(plist, f);
    fclose(f);

    bool ok = runPrivilegedScript(
        "do shell script \""
        "mkdir -p /var/log/pluginplayground && "
        "cp /tmp/com.pluginplayground.grant.plist "
        "/Library/LaunchDaemons/com.pluginplayground.grant.plist && "
        "chown root:wheel /Library/LaunchDaemons/com.pluginplayground.grant.plist && "
        "chmod 644 /Library/LaunchDaemons/com.pluginplayground.grant.plist && "
        "launchctl load /Library/LaunchDaemons/com.pluginplayground.grant.plist"
        "\" with administrator privileges");
    remove("/tmp/com.pluginplayground.grant.plist");
    return ok;
}

bool DaemonManager::uninstall() {
    return runPrivilegedScript(
        "do shell script \""
        "launchctl unload /Library/LaunchDaemons/com.pluginplayground.grant.plist 2>/dev/null; "
        "rm -f /Library/LaunchDaemons/com.pluginplayground.grant.plist"
        "\" with administrator privileges");
}
