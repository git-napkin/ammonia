#pragma once

enum class DaemonStatus {
    NotInstalled,
    InstalledRunning,
    InstalledStopped,
};

class DaemonManager {
public:
    static DaemonStatus status();
    static bool install();
    static bool uninstall();
};
