#!/bin/sh

# Remove directories
sudo rm -rf /private/var/ammonia
sudo rm -rf /usr/local/bin/ammonia

# Remove system launch daemon
sudo launchctl bootout system/com.bedtime.ammonia 2>/dev/null || true
sudo rm -f /Library/LaunchDaemons/com.bedtime.ammonia.plist

echo "Ammonia removed. A reboot is recommended."
exit 0