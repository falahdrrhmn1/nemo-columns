#!/bin/sh
# Salin path file/folder (satu per baris) ke clipboard. Hanya membaca nama, tidak mengubah file apa pun.
[ $# -eq 0 ] && exit 0
printf '%s\n' "$@" | head -c -1 | xclip -selection clipboard -i
