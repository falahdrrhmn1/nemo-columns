# Nemo fork: column view, translucent background, Copy as Path, preview

Fork of [linuxmint/nemo](https://github.com/linuxmint/nemo) 6.4.5 (GPL-2.0+).
Branch `column-view`.

## Features
- **Column View (Miller columns, macOS Finder style)**: 4th view button or View → Column View (Ctrl+4).
- **Translucent background**: like a transparent terminal; text stays opaque.
  Needs a compositor (e.g. xfwm4 compositing). Opacity set in `~/.config/gtk-3.0/gtk.css`.
- **Copy as Path / Copy Folder Path**: right-click menu (Nemo action + xclip).
- **Preview**: select a file, press Space (uses GNOME Sushi when nemo-preview is missing).

## Install (Debian / Ubuntu / Mint, X11)
```sh
git clone -b column-view https://github.com/<user>/nemo.git ~/nemo-src
cd ~/nemo-src && ./install-local.sh
nemo -q     # close the stock Nemo, then open any folder
```
The installer is per-user: it does not replace the apt `nemo` package, it builds into
`~/nemo-src/build` and points the menu, D-Bus service, default folder handler and XFCE
"File Manager" helper at `~/.local/bin/nemo-kolom`.

Uninstall: `./install-local.sh --uninstall`.

## Notes for agents / maintainers
- Code: `src/nemo-column-view.[ch]` (subclass of NemoListView), hooks in
  `nemo-window-menus.c`, `nemo-toolbar.c`, `nemo-window-manage-views.c`,
  `nemo-main-application.c`; RGBA visual + `nemo-translucent` class in `nemo-window.c`;
  Sushi fallback in `nemo-previewer.c`. Extras (actions, CSS) in `extras/`.
- No GSettings schema changes, so the binary works with the distro's installed schemas.
  Build against the SAME Nemo version as the installed apt package.
- Update to a new Nemo release: `git fetch upstream --tags && git rebase <new-tag>`,
  fix conflicts, then `./install-local.sh`.
