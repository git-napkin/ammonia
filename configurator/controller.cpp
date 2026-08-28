#include "controller.h"
#include "dmanager.h"
#include <thread>
#include <unistd.h>

static void eraseMatching(slint::VectorModel<slint::SharedString> &model,
                          const slint::SharedString &value) {
    for (int i = 0; i < model.row_count(); i++) {
        if (*model.row_data(i) == value) {
            model.erase(i);
            break;
        }
    }
}

Controller::Controller(MainWindow &window)
    : m_window(window)
{
    m_window.on_save([this] { save(); });
    m_window.on_edit_tweak([this](slint::SharedString name) {
        openEditor(std::string(name));
    });
    m_window.on_toggle_tweak([this](int index) { toggleTweak(index); });
    m_window.on_package_tweak([this](slint::SharedString name) {
        packageTweak(std::string(name));
    });
    m_window.on_install_tweak([this] { installTweak(); });
    m_window.on_install_daemon([this] { installDaemon(); });
    m_window.on_uninstall_daemon([this] { uninstallDaemon(); });
}

Controller::~Controller() {
    m_alive = false;
}

void Controller::load() {
    Options opts = loadOptions();
    m_window.set_use_legacy_ammonia(opts.useLegacyAmmonia);
    m_window.set_disable_pac(opts.disablePAC);
    m_window.set_pause_injection(opts.pauseInjection);

    refreshTweaks();
    refreshProbes();
}

void Controller::refreshProbes() {
    std::thread([this] {
        bool tools = hasDeveloperTools();
        auto sip = checkSipStatus();
        auto daemon = DaemonManager::status();
        slint::invoke_from_event_loop([this, tools, sip, daemon] {
            if (!m_alive)
                return;
            m_window.set_dev_tools_available(tools);
            m_window.set_sip_kind(static_cast<int>(sip));
            m_window.set_daemon_kind(static_cast<int>(daemon));
        });
    }).detach();
}

void Controller::save() {
    Options opts = loadOptions();
    bool dirChanged = opts.useLegacyAmmonia != m_window.get_use_legacy_ammonia();
    opts.useLegacyAmmonia = m_window.get_use_legacy_ammonia();
    opts.disablePAC = m_window.get_disable_pac();
    opts.pauseInjection = m_window.get_pause_injection();

    if (saveOptions(opts)) {
        m_window.set_status_message("Settings saved.");
        if (dirChanged)
            refreshTweaks();
    } else {
        m_window.set_status_message(
            "Error: Cannot write to /opt/pluginplayground/current.options.");
    }
}

void Controller::refreshTweaks() {
    auto scanned = scanTweaks();
    m_tweaks = std::make_shared<slint::VectorModel<TweakInfo>>();
    auto defaultIcon = m_window.get_default_icon();
    std::string dir = tweaksDir();
    for (const auto &t : scanned) {
        TweakInfo ti;
        ti.name = slint::SharedString(t.name);
        ti.disabled = t.disabled;
        std::string iconPath = dir + "/" + t.name + ".png";
        if (access(iconPath.c_str(), R_OK) == 0)
            ti.icon = slint::Image::load_from_path(
                slint::SharedString(iconPath.c_str()));
        else
            ti.icon = defaultIcon;
        m_tweaks->push_back(std::move(ti));
    }
    m_window.set_tweaks(m_tweaks);
}

void Controller::openEditor(const std::string &name) {
    if (!m_tweaks)
        return;
    bool found = false;
    for (int i = 0; i < m_tweaks->row_count(); i++) {
        if (std::string(m_tweaks->row_data(i)->name) == name) {
            found = true;
            break;
        }
    }
    if (!found)
        return;

    auto editor = TweakEditor::create();
    editor->set_tweak_name(name.c_str());

    TweakOptions opts = loadTweakOptions(name);
    auto apps = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const auto &a : opts.blacklistedApps)
        apps->push_back(slint::SharedString(a));
    editor->set_blacklisted_apps(apps);

    auto deps = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const auto &d : opts.frameworkDependencies)
        deps->push_back(slint::SharedString(d));
    editor->set_framework_deps(deps);

    auto whitelist = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const auto &w : opts.processWhitelist)
        whitelist->push_back(slint::SharedString(w));
    editor->set_process_whitelist(whitelist);

    slint::ComponentWeakHandle<TweakEditor> weak(editor);

    editor->on_save([weak, name]() {
        auto opt = weak.lock();
        if (!opt)
            return;
        auto e = *opt;
        TweakOptions newOpts;
        auto appModel = e->get_blacklisted_apps();
        for (int i = 0; i < appModel->row_count(); i++)
            newOpts.blacklistedApps.push_back(std::string(*appModel->row_data(i)));
        auto depModel = e->get_framework_deps();
        for (int i = 0; i < depModel->row_count(); i++)
            newOpts.frameworkDependencies.push_back(
                std::string(*depModel->row_data(i)));
        auto wlModel = e->get_process_whitelist();
        for (int i = 0; i < wlModel->row_count(); i++)
            newOpts.processWhitelist.push_back(std::string(*wlModel->row_data(i)));
        if (!saveTweakOptions(name, newOpts))
            return;
        e->hide();
    });

    editor->on_cancel([weak]() {
        auto opt = weak.lock();
        if (opt)
            (*opt)->hide();
    });

    editor->on_add_blacklisted([weak, apps](slint::SharedString value) {
        if (!weak.lock() || std::string(value).empty())
            return;
        apps->push_back(value);
    });

    editor->on_add_framework_dep([weak, deps](slint::SharedString value) {
        if (!weak.lock() || std::string(value).empty())
            return;
        deps->push_back(value);
    });

    editor->on_add_whitelisted([weak, whitelist](slint::SharedString value) {
        if (!weak.lock() || std::string(value).empty())
            return;
        whitelist->push_back(value);
    });

    editor->on_remove_blacklisted([weak, apps](slint::SharedString value) {
        if (!weak.lock())
            return;
        eraseMatching(*apps, value);
    });

    editor->on_remove_framework_dep([weak, deps](slint::SharedString value) {
        if (!weak.lock())
            return;
        eraseMatching(*deps, value);
    });

    editor->on_remove_whitelisted([weak, whitelist](slint::SharedString value) {
        if (!weak.lock())
            return;
        eraseMatching(*whitelist, value);
    });

    editor->show();
}

void Controller::toggleTweak(int index) {
    if (!m_tweaks || index < 0 || index >= m_tweaks->row_count())
        return;

    auto row = m_tweaks->row_data(index);
    if (!row)
        return;

    std::string name(row->name);
    if (::toggleTweak(name)) {
        TweakInfo updated = *row;
        updated.disabled = !updated.disabled;
        m_tweaks->set_row_data(index, updated);
    } else {
        m_window.set_status_message(
            slint::SharedString(("Error: Cannot toggle " + name).c_str()));
    }
}

void Controller::packageTweak(const std::string &name) {
    if (::packageTweak(name)) {
        m_window.set_status_message(
            slint::SharedString(("Packaged: /tmp/" + name + ".pkg").c_str()));
    } else {
        m_window.set_status_message(
            slint::SharedString(("Package failed for " + name).c_str()));
    }
}

void Controller::installTweak() {
    if (::installTweakFromDialog()) {
        refreshTweaks();
        m_window.set_status_message("Tweak installed.");
    } else {
        m_window.set_status_message("Install canceled or failed.");
    }
}

void Controller::refreshDaemonStatus() {
    m_window.set_daemon_kind(static_cast<int>(DaemonManager::status()));
}

void Controller::installDaemon() {
    if (DaemonManager::install()) {
        m_window.set_status_message("Launch daemon installed.");
    } else {
        m_window.set_status_message("Error: Failed to install launch daemon.");
    }
    refreshDaemonStatus();
}

void Controller::uninstallDaemon() {
    if (DaemonManager::uninstall()) {
        m_window.set_status_message("Launch daemon uninstalled.");
    } else {
        m_window.set_status_message("Error: Failed to uninstall launch daemon.");
    }
    refreshDaemonStatus();
}
