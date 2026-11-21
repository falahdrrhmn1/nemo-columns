#!/usr/bin/env bash
# Pasang Nemo modifikasi (column view, latar transparan, Copy as Path, preview Sushi)
# untuk user saat ini. Tidak menimpa Nemo dari apt; semua file ada di $HOME.
# Pemakaian:   ./install-local.sh            (pasang/perbarui)
#              ./install-local.sh --uninstall (kembalikan ke Nemo asli)
set -euo pipefail
REPO="$(cd "$(dirname "$0")" && pwd)"
BIN="$HOME/.local/bin/nemo-kolom"
APPS="$HOME/.local/share/applications"
DBUS="$HOME/.local/share/dbus-1/services"
ACT="$HOME/.local/share/nemo/actions"
HELP="$HOME/.local/share/xfce4/helpers"
CSS="$HOME/.config/gtk-3.0/gtk.css"

if [ "${1:-}" = "--uninstall" ]; then
  rm -f "$BIN" "$APPS/nemo.desktop" "$DBUS/nemo.service" "$DBUS/nemo.FileManager1.service" \
        "$ACT/copy-path.sh" "$ACT/copy-path.nemo_action" "$ACT/copy-path-folder.nemo_action" \
        "$HELP/nemo-kolom.desktop"
  [ -f "$HOME/.config/xfce4/helpers.rc" ] && sed -i '/^FileManager=nemo-kolom$/d' "$HOME/.config/xfce4/helpers.rc"
  [ -f "$CSS" ] && sed -i '/=== Nemo transparan/,/=== akhir Nemo transparan ===/d' "$CSS"
  echo "Selesai. Nemo asli (apt) dipakai lagi. Jalankan: nemo -q"
  exit 0
fi

echo "[1/5] Dependensi (butuh sudo, Debian/Ubuntu/Mint)"
if command -v apt-get >/dev/null; then
  sudo apt-get install -y nemo gnome-sushi xclip meson ninja-build
  sudo apt-get build-dep -y nemo || echo "  ! build-dep gagal: aktifkan baris deb-src di sources.list lalu ulangi"
else
  echo "  ! Bukan distro apt: pasang manual nemo, gnome-sushi, xclip, meson, ninja dan dependensi build nemo"
fi

echo "[2/5] Build (release)"
if [ ! -d "$REPO/build" ]; then
  meson setup "$REPO/build" "$REPO" -Dbuildtype=release -Dgtk_doc=false
else
  meson configure "$REPO/build" -Dbuildtype=release
fi
nice ninja -C "$REPO/build" -j"$(( $(nproc) > 2 ? $(nproc) / 2 : 1 ))"

echo "[3/5] Peluncur dan jadikan default"
mkdir -p "$(dirname "$BIN")" "$APPS" "$DBUS" "$ACT" "$HELP" "$(dirname "$CSS")"
printf '#!/bin/sh\nexec "%s/build/src/nemo" "$@"\n' "$REPO" > "$BIN"; chmod +x "$BIN"
sed "s#^Exec=nemo #Exec=$BIN #" /usr/share/applications/nemo.desktop > "$APPS/nemo.desktop"
for f in nemo.service nemo.FileManager1.service; do
  [ -f "/usr/share/dbus-1/services/$f" ] && sed "s#^Exec=/usr/bin/nemo #Exec=$BIN #" "/usr/share/dbus-1/services/$f" > "$DBUS/$f"
done
xdg-mime default nemo.desktop inode/directory application/x-gnome-saved-search
if command -v xfce4-session >/dev/null; then
  sed "s#@BIN@#$BIN#g" > "$HELP/nemo-kolom.desktop" <<'H'
[Desktop Entry]
NoDisplay=true
Version=1.0
Type=X-XFCE-Helper
X-XFCE-Category=FileManager
X-XFCE-CommandsWithParameter=@BIN@ "%s"
Icon=system-file-manager
Name=Nemo (column view)
X-XFCE-Commands=@BIN@
H
  touch "$HOME/.config/xfce4/helpers.rc"
  sed -i '/^FileManager=/d' "$HOME/.config/xfce4/helpers.rc"
  echo "FileManager=nemo-kolom" >> "$HOME/.config/xfce4/helpers.rc"
fi

echo "[4/5] Menu klik kanan Copy as Path"
cp "$REPO/extras/copy-path.sh" "$ACT/"; chmod +x "$ACT/copy-path.sh"
for f in copy-path.nemo_action copy-path-folder.nemo_action; do
  sed "s#@HOME@#$HOME#g" "$REPO/extras/$f" > "$ACT/$f"
done

echo "[5/5] Latar transparan (gtk.css, tidak menimpa isi lain)"
touch "$CSS"
sed -i '/=== Nemo transparan/,/=== akhir Nemo transparan ===/d' "$CSS"
cat "$REPO/extras/nemo-translucent.css" >> "$CSS"

echo "Selesai. Tutup Nemo lama dengan: nemo -q   lalu buka folder seperti biasa."
