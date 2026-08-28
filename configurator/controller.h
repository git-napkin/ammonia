#pragma once
#include "configurator.h"
#include "options.h"
#include "tweaks.h"
#include <atomic>
#include <memory>

class Controller {
public:
    Controller(MainWindow& window);
    ~Controller();
    void load();

private:
    void save();
    void refreshTweaks();
    void refreshProbes();
    void openEditor(const std::string& name);
    void toggleTweak(int index);
    void packageTweak(const std::string& name);
    void installTweak();
    void refreshDaemonStatus();
    void installDaemon();
    void uninstallDaemon();

    MainWindow& m_window;
    std::shared_ptr<slint::VectorModel<TweakInfo>> m_tweaks;
    std::atomic<bool> m_alive{true};
};
