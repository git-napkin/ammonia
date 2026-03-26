#!/bin/sh

# Remove directories
sudo rm -rf /var/ammonia
sudo rm -rf /usr/local/bin/ammonia

# Remove system launch daemon
sudo launchctl bootout system/com.bedtime.ammonia
sudo rm -rf /Library/LaunchDaemons/com.bedtime.ammonia.plist

# Reboot
sudo shutdown -r now