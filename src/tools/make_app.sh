#!/bin/sh
# Builds the GUI into build/Preflop.app and installs it in /Applications, replacing any earlier copy.
# The app holds its own copy of the solves and charts (main.cpp finds them at ../data and ../ref), so
# rerun this after a new solve, and note that painter edits made in the app are lost when it is replaced.
# Personal use only: the bundle contains ref/ and the Anchored solve built from it.
set -e
cd "$(dirname "$0")/../.."

cmake -B build
cmake --build build -j --target preflop

app=build/Preflop.app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/data"
cp build/preflop "$app/Contents/MacOS/"
cp data/strategy_6max.bin data/strategy_anchored.bin data/hu_equity.bin "$app/Contents/data/"
cp -R ref "$app/Contents/ref"
cat > "$app/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>preflop</string>
    <key>CFBundleIdentifier</key><string>local.preflop</string>
    <key>CFBundleName</key><string>Preflop</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF
codesign --force --sign - "$app"

rm -rf /Applications/Preflop.app
cp -R "$app" /Applications/
echo "Installed /Applications/Preflop.app"
