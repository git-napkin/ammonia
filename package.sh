#!/bin/sh

# Check if the variable $ammoniabuildfolder is set and not empty
if [ -z "$ammoniabuildfolder" ]; then
    # Check if Build/ammonia exists in the current directory
    if [ -f "./Build/ammonia" ]; then
        # Set $ammoniabuildfolder to the current directory
        ammoniabuildfolder=$(pwd)
    else
        # Ask the user to enter the build directory's path
        echo "Please enter the build directory's path:"
        read ammoniabuildfolder
    fi
fi

# Create a directory for the scripts
mkdir "$ammoniabuildfolder/scripts"

# Create a postinstall script to set up the LaunchDaemon
cat <<'EOL' > "$ammoniabuildfolder/scripts/postinstall"
#!/bin/sh

# Write the LaunchDaemon plist
cat > /Library/LaunchDaemons/com.bedtime.ammonia.plist <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.bedtime.ammonia</string>
    <key>ProgramArguments</key>
    <array>
        <string>/private/var/ammonia/core/ammonia</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <false/>
</dict>
</plist>
PLIST
chmod 644 /Library/LaunchDaemons/com.bedtime.ammonia.plist

# Lock down ownership and permissions on the entire ammonia directory tree
chown -R root:wheel /private/var/ammonia
chmod 755 /private/var/ammonia
chmod 755 /private/var/ammonia/core
chmod 755 /private/var/ammonia/core/tweaks
chmod 755 /private/var/ammonia/core/gui

# Unload any existing registration, then load the updated plist for next boot
launchctl unload /Library/LaunchDaemons/com.bedtime.ammonia.plist 2>/dev/null || true
launchctl load /Library/LaunchDaemons/com.bedtime.ammonia.plist 2>/dev/null || launchctl bootstrap system /Library/LaunchDaemons/com.bedtime.ammonia.plist 2>/dev/null || true

# One-time system configuration (idempotent)
CURRENT_BOOT_ARGS="$(nvram boot-args 2>/dev/null | cut -f 2-)"
if [ -n "$CURRENT_BOOT_ARGS" ]; then
    case " $CURRENT_BOOT_ARGS " in
        *" -arm64e_preview_abi "*) ;;
        *) nvram boot-args="-arm64e_preview_abi $CURRENT_BOOT_ARGS" ;;
    esac
else
    nvram boot-args="-arm64e_preview_abi" 2>/dev/null || true
fi
defaults write /Library/Preferences/com.apple.security.libraryvalidation DisableLibraryValidation -bool true

echo ""
echo "== Ammonia installed =="
echo "A reboot is required for the update to take full effect."
echo ""

exit 0
EOL

# Make the postinstall script executable
chmod +x "$ammoniabuildfolder/scripts/postinstall"

# Create a temporary directory and setup the installation files in it.
mkdir "$ammoniabuildfolder/temp"
mkdir "$ammoniabuildfolder/temp/ammonia"
mkdir "$ammoniabuildfolder/temp/ammonia/core"
mkdir "$ammoniabuildfolder/temp/ammonia/core/tweaks"
cp "$ammoniabuildfolder/./fridagum.dylib" "$ammoniabuildfolder/temp/ammonia/core/"
cp "$ammoniabuildfolder/./Build/ammonia" "$ammoniabuildfolder/temp/ammonia/core/"
cp "$ammoniabuildfolder/./Build/liblibinfect.dylib" "$ammoniabuildfolder/temp/ammonia/core/"
cp "$ammoniabuildfolder/./Build/libopener.dylib" "$ammoniabuildfolder/temp/ammonia/core/"

chmod 755 "$ammoniabuildfolder/temp/ammonia/core/tweaks"

mkdir "$ammoniabuildfolder/temp/ammonia/core/gui"
chmod 755 "$ammoniabuildfolder/temp/ammonia/core/gui"

# Build the package
sudo pkgbuild --install-location /private/var/ --root "$ammoniabuildfolder/temp" --scripts "$ammoniabuildfolder/scripts" --identifier com.bedtime.ammonia "$ammoniabuildfolder/ammonia.pkg"
rm -r "$ammoniabuildfolder/scripts/"

# Remove the temporary directory
rm -r "$ammoniabuildfolder/temp"