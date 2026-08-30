#!/bin/sh
set -eu

echo "[-] Uninstalling Ammonia and leftover Plugin Playground files..."

if [ "$(id -u)" -ne 0 ]; then
    echo "Elevating privileges to uninstall system-wide files..."
    exec sudo "$0" "$@"
fi

echo "Pausing injection..."
if [ -f "/private/var/ammonia/core/current.options" ]; then
    defaults write /private/var/ammonia/core/current.options pauseInjection -bool true
fi

echo "Unloading daemons..."
launchctl bootout system/com.ammonia.inject 2>/dev/null || true
launchctl bootout system/com.pluginplayground.grant 2>/dev/null || true
sleep 1

echo "Removing GUI..."
rm -rf "/Applications/Ammonia.app"
rm -rf "/Applications/Plugin Playground.app"

echo "Removing core..."
rm -rf "/private/var/ammonia"
rm -rf "/opt/pluginplayground"

echo "Removing launch daemons..."
rm -f "/Library/LaunchDaemons/com.ammonia.inject.plist"
rm -f "/Library/LaunchDaemons/com.pluginplayground.grant.plist"

echo "Removing logs..."
rm -rf "/var/log/ammonia"
rm -rf "/var/log/pluginplayground"

echo "Forgetting package receipts..."
pkgutil --forget "com.ammonia.core" > /dev/null 2>&1 || true
pkgutil --forget "com.pluginplayground.core" > /dev/null 2>&1 || true

echo "[-] Uninstallation complete."
echo "    Reboot before installing Ammonia again so launchd is clean."
