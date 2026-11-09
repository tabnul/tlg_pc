#!/usr/bin/env bash
# Installs the launcher shortcut in the applications menu (Linux, freedesktop standard: GNOME, KDE...).
# The icon is the port's own (port/packaging/icon/tlg-icon-1024.png, original project art).
# Usage: scripts/install_desktop.sh [--remove]
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
icons=${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor
desktop=$apps/tlg-pc.desktop
if [[ ${1:-} == --remove ]]; then
    rm -f "$desktop"; find "$icons" -name tlg-pc.png -delete 2>/dev/null || true
    echo "Atalho removido."; exit 0
fi
icon_src="$root/port/packaging/icon/tlg-icon-1024.png"
icon=tlg-pc
for size in 512 256 128 64 48; do
    mkdir -p "$icons/${size}x${size}/apps"
    python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).resize((int(sys.argv[3]),) * 2, Image.LANCZOS).save(sys.argv[2])" \
        "$icon_src" "$icons/${size}x${size}/apps/tlg-pc.png" "$size" 2>/dev/null ||
        cp "$icon_src" "$icons/${size}x${size}/apps/tlg-pc.png"
done
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$icons" 2>/dev/null || true
mkdir -p "$apps"
cat > "$desktop" <<DESK
[Desktop Entry]
Type=Application
Name=The Last Guardian
Comment=Native PC port launcher (your own PS4 dump)
Exec=$root/tlg_launcher.sh
Path=$root
Icon=$icon
Terminal=false
Categories=Game;
StartupWMClass=tlg-pc
DESK
command -v update-desktop-database >/dev/null && update-desktop-database -q "$apps" 2>/dev/null || true
echo "Atalho instalado: $desktop"
