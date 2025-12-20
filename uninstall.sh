#!/usr/bin/env bash
set -e

APP_NAME="colossus-editor"
BIN_NAME="colossus-editor"

PREFIX="/usr/local"
BIN_DIR="$PREFIX/bin"
APP_DIR="$PREFIX/share/applications"
ICON_DIR="$PREFIX/share/icons/hicolor"

echo "[+] Uninstalling COLOSSUS Editor..."

# ---- remove binary ----
if [[ -f "$BIN_DIR/$BIN_NAME" ]]; then
    echo "[+] Removing binary"
    sudo rm -f "$BIN_DIR/$BIN_NAME"
fi

# ---- remove desktop entry ----
if [[ -f "$APP_DIR/$APP_NAME.desktop" ]]; then
    echo "[+] Removing desktop entry"
    sudo rm -f "$APP_DIR/$APP_NAME.desktop"
fi

# ---- remove icons ----
echo "[+] Removing icons"
for size in 256x256 128x128 64x64; do
    sudo rm -f "$ICON_DIR/$size/apps/$APP_NAME.png"
done

# ---- refresh caches ----
echo "[+] Updating icon cache..."
sudo gtk-update-icon-cache -f "$ICON_DIR" >/dev/null 2>&1 || true

echo "[+] Updating desktop database..."
sudo update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true

echo "[✓] COLOSSUS Editor fully removed"
