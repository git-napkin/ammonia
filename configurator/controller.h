#pragma once
#include "configurator.h"
#include "options.h"
#include "tweaks.h"
#include <memory>

class Controller {
public:
    Controller(MainWindow& window);
    void load();

private:
    void save();
    void refreshTweaks();
    void openEditor(const std::string& name);
    void toggleTweak(int index);
    void packageTweak(const std::string& name);
    void refreshDaemonStatus();
    void installDaemon();
    void uninstallDaemon();

    MainWindow& m_window;
    std::shared_ptr<slint::VectorModel<TweakInfo>> m_tweaks;
};
