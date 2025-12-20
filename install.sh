#!/usr/bin/env bash
set -euo pipefail

APP_ID="colossus-editor"
APP_NAME="COLOSSUS Editor"
BIN_NAME="editor"
COMMENT="High-contrast monochrome text editor inspired by CRT aesthetics"
CATEGORIES="Utility;TextEditor;Development;"
MIME="text/plain;text/x-csrc;text/x-chdr;text/x-c++src;text/x-c++hdr;text/x-python;text/x-shellscript;application/x-shellscript;application/json;text/markdown;"

PREFIX="$HOME/.local"
BIN_DIR="$PREFIX/bin"
APP_DIR="$PREFIX/share/applications"
ICON_ROOT="$PREFIX/share/icons/hicolor"

DESKTOP_FILE="$APP_DIR/${APP_ID}.desktop"
EXEC_ABS="$BIN_DIR/${BIN_NAME}"

echo "[${APP_ID}] Starting install..."

# deps
if command -v pacman >/dev/null 2>&1; then
  echo "[${APP_ID}] Installing dependencies..."
  sudo pacman -S --needed --noconfirm base-devel gtk3 gtksourceview3
fi

echo "[${APP_ID}] Building..."
make

if [[ ! -f "./${BIN_NAME}" ]]; then
  echo "[${APP_ID}] ERROR: build output './${BIN_NAME}' not found."
  exit 1
fi

# ensure dirs exist (THIS WAS MISSING)
mkdir -p "${BIN_DIR}" "${APP_DIR}"

echo "[${APP_ID}] Installing binary -> ${EXEC_ABS}"
install -Dm755 "./${BIN_NAME}" "${EXEC_ABS}"

# style
STYLE_DIR="$PREFIX/share/gtksourceview-3.0/styles"
mkdir -p "${STYLE_DIR}"
if [[ -f "./styles/colossus-mono.xml" ]]; then
  echo "[${APP_ID}] Installing style scheme..."
  install -Dm644 "./styles/colossus-mono.xml" "${STYLE_DIR}/colossus-mono.xml"
fi

# icon (optional)
ICON_SRC=""
for f in "./editor.png" "./icon.png" "./assets/editor.png"; do
  [[ -f "$f" ]] && ICON_SRC="$f" && break
done

if [[ -n "$ICON_SRC" ]]; then
  echo "[${APP_ID}] Installing icon from ${ICON_SRC}"
  for size in 256x256 128x128 64x64 48x48; do
    install -Dm644 "$ICON_SRC" "$ICON_ROOT/$size/apps/${APP_ID}.png"
  done
fi

echo "[${APP_ID}] Installing desktop entry -> ${DESKTOP_FILE}"
cat > "${DESKTOP_FILE}" <<EOF
[Desktop Entry]
Type=Application
Name=${APP_NAME}
GenericName=Text Editor
Comment=${COMMENT}
Exec=${EXEC_ABS} %F
Icon=${APP_ID}
Terminal=false
Categories=${CATEGORIES}
StartupNotify=true
MimeType=${MIME}
EOF

# Compatibility alias: keep old name working (VERY IMPORTANT for your system)
ln -sf "${APP_ID}.desktop" "${APP_DIR}/editor.desktop"

# refresh caches
update-desktop-database "${APP_DIR}" >/dev/null 2>&1 || true
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f -t "${ICON_ROOT}" >/dev/null 2>&1 || true
fi

# restore “default editor” behavior (like old script)
if command -v xdg-mime >/dev/null 2>&1; then
  echo "[${APP_ID}] Setting as default handler..."
  xdg-mime default "${APP_ID}.desktop" text/plain || true
  xdg-mime default "${APP_ID}.desktop" text/x-c++src || true
  xdg-mime default "${APP_ID}.desktop" text/x-c++hdr || true
  xdg-mime default "${APP_ID}.desktop" text/x-csrc || true
  xdg-mime default "${APP_ID}.desktop" text/x-chdr || true
fi

echo "[${APP_ID}] Install complete!"
echo "Try: gtk-launch ${APP_ID}"
