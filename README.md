# FeTerm

**Website:** https://faheeme.gitlab.io/feterm-site/

A GPU-accelerated terminal emulator in C23 that runs natively on Wayland and X11 through SDL3,
and on Windows 10 (1809) and 11.
The name lives in two Makefile variables: `NAME` (binary, config and data directory, app-id)
and `DISPLAY_NAME` (window title, `--version`, `TERM_PROGRAM`).

**Status:** the core terminal is complete. It has PCRE2 network highlighting (Cisco, Arista,
Juniper and a shared network profile), click-to-position, selection and copy, mouse
reporting, links, scrollback search, reflow, configurable keys, live reload, tabs and split
panes, the kitty keyboard protocol, desktop notifications, drawn box characters, cursor
animation, 12 themes, true transparency, and inline images (sixel and kitty graphics).
The sidebar has a session manager (SSH, Telnet, serial and local sessions in folders, with
keepalives, anti-idle and reconnect) and a remote file explorer (SFTP, with an SCP fallback).

## Install

### Fedora, Rocky, AlmaLinux and RHEL

FeTerm is packaged in [COPR](https://copr.fedorainfracloud.org/coprs/faheeme/feterm/) for
Fedora 43, 44, 45 and Rawhide, and for Rocky, AlmaLinux and RHEL 10 (x86_64 and aarch64).

Fedora:

```sh
sudo dnf copr enable faheeme/feterm
sudo dnf install feterm
```

Rocky / AlmaLinux / RHEL 10 (EPEL and CRB are not needed):

```sh
sudo dnf install dnf-plugins-core
sudo dnf copr enable faheeme/feterm
sudo dnf install feterm
```

Updates arrive with `sudo dnf upgrade`. dnf checks the repo for new versions every day or
two; `sudo dnf upgrade --refresh` checks straight away. To remove FeTerm and the repo:

```sh
sudo dnf remove feterm
sudo dnf copr remove faheeme/feterm
```

### Ubuntu

FeTerm is packaged in a [PPA](https://launchpad.net/~faheeme/+archive/ubuntu/feterm) for
Ubuntu 26.04 LTS and 26.10 (amd64 and arm64). Ubuntu 24.04 has no SDL3, so it isn't supported.

```sh
sudo add-apt-repository ppa:faheeme/feterm
sudo apt install feterm
```

Updates arrive with `sudo apt update && sudo apt upgrade`. To remove FeTerm and the PPA:

```sh
sudo apt remove feterm
sudo add-apt-repository --remove ppa:faheeme/feterm
```

### Other distributions

Build it from source as described below.

### Windows

Windows 10 (version 1809 or newer) and 11, x64. Download from the
[releases page](https://gitlab.com/faheeme/feterm/-/releases):

- `feterm-X.Y.Z-windows-x64-setup.exe`: an installer (per user without admin rights, or
  for all users) with a Start-menu entry and, optionally, "Open FeTerm here" on folders.
- `feterm-X.Y.Z-windows-x64.zip`: portable. Unzip anywhere and run `feterm.exe`.

The files are not code-signed yet, so SmartScreen may warn the first time: choose
**More info → Run anyway**. Settings and sessions live in `%APPDATA%\feterm`.

If Microsoft Defender blocks or quarantines FeTerm instead:

1. **Allow it from Protection history.** Open **Windows Security → Virus & threat
   protection → Protection history**, select the entry for FeTerm and choose **Actions →
   Allow on device**.
2. **If it's still blocked, add an exclusion.** In **Virus & threat protection settings →
   Manage settings → Exclusions**, choose **Add or remove exclusions** and add FeTerm's
   folder (or just `feterm.exe`).

An exclusion stops Defender scanning that folder at all, so keep it to FeTerm's own folder.

SSH sessions and the Files tab use the OpenSSH client that comes with Windows. If `ssh -V`
says "not recognized", add it under **Settings → System → Optional features → OpenSSH
Client**. Telnet and serial sessions need nothing extra.

Differences from Linux:
- **The Files tab asks for the password a second time.** Windows' OpenSSH can't share one
  login between connections (no ControlMaster), so the Files tab logs in separately. With
  SSH keys and the agent (the "OpenSSH Authentication Agent" service, then `ssh-add`) neither
  login asks.
- `ssh` typed by hand in a PowerShell or cmd tab doesn't open the Files tab (the same as on
  Linux). Use a session or Quick Connect.

### macOS

macOS 14 Sonoma or newer on Apple Silicon (every M1, M2, M3, M4 and M5 Mac), or macOS 15
Sequoia or newer on Intel.

**With Homebrew** (no Gatekeeper prompt, updates with `brew upgrade`; prebuilt for Apple
Silicon on macOS 15 and 26, while on macOS 14 and Intel Macs Homebrew builds from source, so
the disk image below is quicker there):

```
brew install faheeme/tap/feterm
ln -sf "$(brew --prefix)/opt/feterm/FeTerm.app" /Applications/FeTerm.app   # optional: Launchpad and Dock
```

`feterm` also runs from any terminal. The formula lives in
[faheeme/homebrew-tap](https://github.com/faheeme/homebrew-tap).

**Or download a disk image** from the [releases page](https://gitlab.com/faheeme/feterm/-/releases):
`feterm-X.Y.Z-macos-arm64.dmg` (Apple Silicon) or `feterm-X.Y.Z-macos-x86_64.dmg` (Intel).
Open the image and drag **FeTerm** to Applications.

The disk image is not signed with an Apple Developer ID yet, so macOS blocks the first launch.
On macOS 15 and newer: open FeTerm once, dismiss the warning, then go to **System
Settings → Privacy & Security** and choose **Open Anyway** (on macOS 14, right-click the
app and choose **Open**). Or, in Terminal: `xattr -dr com.apple.quarantine /Applications/FeTerm.app`.

Differences from Linux:
- **Shortcuts use Cmd**, like Terminal.app and iTerm2 (see the table below).
- **Option types characters** (Option+3 is # on a UK keyboard, Option+e e is é). To use
  Option as Alt/Meta for word jumps in the shell, set `[keyboard] macos_option_as_alt =
  left` (or `right` / `both`).
- **Cmd+click opens links** (Ctrl+click is a right-click on a Mac).
- Settings live in `~/.config/feterm`, as on Linux. Serial adapters appear as
  `/dev/cu.usbserial-*` or `/dev/cu.usbmodem*`.

## Build

Fedora:

```sh
sudo dnf install gcc make pkgconf-pkg-config SDL3-devel pcre2-devel freetype-devel \
                 fontconfig-devel libglvnd-devel
```

EL10 / Rocky 10: run `sudo dnf config-manager --set-enabled crb` first, then the same command.

Arch / Omarchy:

```sh
sudo pacman -S --needed base-devel sdl3 pcre2 freetype2 fontconfig libglvnd
```

Ubuntu 26.04 / Debian (SDL3 is not packaged in Ubuntu 24.04):

```sh
sudo apt install build-essential pkgconf libsdl3-dev libpcre2-dev libfreetype-dev \
                 libfontconfig-dev libgles-dev
```

```sh
make            # ./feterm
make test       # headless core tests under ASan + UBSan
make debug      # ./feterm built with ASan + UBSan
make analyze    # GCC -fanalyzer over all sources
sudo make install PREFIX=/usr/local
```

`make install` also installs the man page (`man feterm`), `config.ini.example` in the data
directory, the launcher entry (`io.gitlab.faheeme.feterm.desktop`) and the icons
into the hicolor theme: PNGs from 16 to 512 px, a scalable SVG and a symbolic panel icon.
The window's app-id matches the launcher's name, so docks and task switchers show the icon
on Wayland and X11.

Run from the source tree with `./feterm`: themes are found in `./data` next to the binary.

The Windows build is cross-compiled on Linux in Docker: `packaging/windows/build.sh` (see
`packaging/README.md`).

Warnings are errors by default. `make WERROR=` turns that off, for example when a newer
compiler adds a warning. The distribution packages in `packaging/` (AUR, COPR, PPA) build
this way.

## Usage

```
feterm [-d DIR] [-c CONFIG] [-T TITLE] [--class ID] [--hold] [-e command args...]
```

| Shortcut (default) | Action |
|---|---|
| Ctrl+Shift+C / Ctrl+Shift+V | copy selection / paste clipboard |
| Shift+Insert, middle click | paste the primary selection |
| drag, double-click, triple-click | select text, a word (IPs, paths, URLs), a whole line |
| Alt+drag | rectangular selection |
| Shift+drag / Shift+click | select even when a program uses the mouse |
| Ctrl+click (hover shows the link) | open a URL or OSC 8 hyperlink |
| Ctrl+Shift+F | search the scrollback (regex; Enter older, Shift+Enter newer, Esc close) |
| Ctrl+Shift+Z / X | jump to the previous / next shell prompt (OSC 133) |
| Ctrl+Shift+= / - / 0 | zoom in / out / reset |
| Shift+PgUp / PgDn, Shift+Home / End | scroll through history |
| Ctrl+Shift+Up / Down, mouse wheel | scroll by line |
| Ctrl+Shift+H | highlighting: auto → off → each profile → auto |
| Ctrl+Shift+R | reload configuration (also automatic when files change) |
| Ctrl+Shift+T / Ctrl+Shift+W | new tab / close pane (closing the last pane closes the tab) |
| Ctrl+Shift+E / Ctrl+Shift+O | split right / split down (new panes start in the current directory) |
| Ctrl+Tab, Ctrl+PgDn / Ctrl+Shift+Tab, Ctrl+PgUp, Alt+1..9 | next / previous / numbered tab |
| Ctrl+Shift+] / [ , click | focus next / previous pane |
| Ctrl+Shift+Alt+arrows, drag a divider | resize panes |
| Ctrl+Shift+Enter | zoom the focused pane to the whole tab (and back) |
| Ctrl+Shift+. | switch to the next installed theme |
| Click in the line you are typing | move the cursor there |
| Ctrl+Shift+B | show / hide the sidebar |
| Ctrl+Shift+1 / Ctrl+Shift+2 | Sessions / Files tab (keyboard goes to the sidebar; Esc returns) |
| Ctrl+Shift+N | quick connect (SSH, Telnet or serial without saving) |
| Ctrl+Shift+Pause, Ctrl+Pause | send a Break (serial and Telnet sessions) |

On macOS the defaults use Cmd instead:

| macOS shortcut | Action |
|---|---|
| Cmd+C / Cmd+V | copy / paste |
| Cmd+click | open a link |
| Cmd+F | search the scrollback |
| Cmd+Up / Cmd+Down | previous / next shell prompt |
| Cmd+= / Cmd+- / Cmd+0 | zoom in / out / reset |
| Cmd+Option+Up / Down, Cmd+Home / End | scroll by line / to the top or bottom |
| Cmd+T / Cmd+W | new tab / close pane |
| Cmd+D / Cmd+Shift+D | split right / split down |
| Cmd+Shift+] / [ , Ctrl+Tab, Cmd+1..9 | next / previous / numbered tab |
| Cmd+] / Cmd+[ | focus next / previous pane |
| Ctrl+Cmd+arrows | resize panes |
| Cmd+Shift+Enter | zoom the focused pane |
| Cmd+Shift+H / Cmd+Shift+R / Cmd+Shift+. | highlighting / reload configuration / next theme |
| Cmd+B, Cmd+Shift+1 / 2 | sidebar, Sessions / Files tab |
| Cmd+Shift+N | quick connect |
| Cmd+Option+B | send a Break (serial and Telnet sessions) |
| Cmd+Q, Cmd+H, Cmd+M, Ctrl+Cmd+F | quit, hide, minimise, full screen (the standard Mac menu) |

Every shortcut can be changed in the `[keys]` section of `config.ini`.
Every shortcut can be changed in the `[keys]` section of `config.ini` (`cmd` and `option`
are accepted as names for super and alt).

New local shells start with a small banner: the FeTerm logo, the version and the author.
Turn it off with `[terminal] banner = false`. The name, author and email come from the
Makefile (`DISPLAY_NAME`, `AUTHOR`, `EMAIL`).

Programs can send desktop notifications (OSC 9, OSC 777, OSC 99); by default they are
shown only when you are not looking at that pane, at most one every two seconds, via
`notify-send`. Images use sixel or the kitty graphics protocol with raw RGB/RGBA data
(`f=24` / `f=32`); PNG data (`f=100`) is answered with an error.

Pasting several lines into a program that does not use bracketed paste asks for
confirmation first, because each line would run as a command as soon as it arrived.
Links open through `xdg-open` without a shell, and only for http(s), ftp, sftp, ssh,
file and mailto links.

## Sessions

The sidebar (left by default; `[sidebar] position = right` moves it) holds your saved
sessions in folders, like SecureCRT or MobaXterm. Drag its edge to resize it, and click «
or press Ctrl+Shift+B to collapse it to a thin strip.

- **Types:** SSH, Telnet, serial console and local shell.
  - SSH runs your system's OpenSSH, so keys, the ssh agent, `~/.ssh/config`, jump hosts and
    known_hosts all work as usual.
  - Telnet and serial are built in, so no extra programs are needed.
- **Serial:** any speed from 300 to 4000000 baud, plus custom rates (e.g. 250000), with data
  bits, parity, stop bits and flow control. Ctrl+Shift+Pause sends a Break for ROMMON and
  password recovery. If the port says permission denied, join the `dialout` group:
  `sudo usermod -aG dialout $USER`, then log in again.
- **Managing sessions:** right-click opens a menu (it never acts by itself). You can connect,
  connect in a split, edit, duplicate, rename, copy the host, and delete (it always asks
  first). Folders have New session here, New folder here, Folder settings, Rename and
  Delete. Drag a session onto a folder to move it.
- **Keyboard:** type in the filter box to find sessions by name, host or folder. Up/Down
  select, Enter connects (Shift+Enter in a split), Left/Right close and open folders, F2
  renames, Delete deletes (after asking) and Shift+F10 opens the menu.
- **Folder settings:** a folder's colour, default user and anti-idle setting apply to
  everything inside it unless a session sets its own. A session's colour shows as a stripe
  on its tab (e.g. red for production).
- **Staying connected:**
  - SSH and TCP keepalives stop firewalls from dropping idle sessions.
  - Anti-idle stops the device's own idle logout (Cisco `exec-timeout`, Linux `TMOUT`):
    after 4 minutes without typing, FeTerm sends an invisible NUL byte. Nothing is sent
    while a full-screen program such as vim or less is open.
  - If a connection drops, the tab stays open with its history. Press Enter to reconnect, or
    set `reconnect = auto`.
- **Passwords are never stored.** Use SSH keys or the agent, or type the password when asked.
- **Shared logins:** SSH sessions share one login per host (OpenSSH connection sharing).
  Opening a second tab to the same host is instant, and the file explorer will reuse it. If
  an old device refuses shared connections, untick "Share login" for that session.

Sessions are plain files: `~/.config/feterm/sessions/<folder>/<name>.ini` (mode 0600). You
can back them up, sync them, or edit them by hand.

## Files

The Files tab (Ctrl+Shift+2) browses the focused SSH session's server over SFTP.
- **Logging in:** it reuses the terminal's login, so there's no second password. If it can't,
  it asks in a dialog; the answer goes straight to ssh and is wiped from memory. On Windows
  it always logs in separately, so password logins are asked twice (see *Windows* above).
- **Right-click:** opens a menu and never acts by itself.
  - On a file: Download, Download to..., Edit locally, Rename, Copy path, Permissions,
    Properties and Delete.
  - On a folder: Open, Download folder (recursive), Rename, Delete (recursive, after asking).
  - On empty space: Upload files, Upload folder, New folder, Refresh, Show hidden files, Go to
    folder and Home.
  - Ctrl+click and Shift+click select several items.
- **Keyboard:** Enter opens a folder or downloads a file, Backspace goes up, F2 renames, F5
  refreshes, Delete deletes (after asking) and Shift+F10 opens the menu.
- **Uploading:** drag files from your desktop onto the window, or use Upload files.... If a
  name already exists, you're asked whether to keep both (adding a number), replace or skip.
- **Downloads:** they go to `[files] download_dir` (default `~/Downloads`). Each one is written
  to a hidden temporary file and renamed when complete, and an existing file is never replaced
  unless you chose that.
- **Transfers:** shown at the bottom of the tab with progress. Right-click there to cancel,
  retry or clear them.
- **Edit locally:** downloads the file to a private folder, opens it in `[files] editor` (or
  your desktop's default), and uploads it again every time you save.
- **Follow terminal folder** (the ⇵ button): the tab follows the folder of the remote shell.
  The server's shell has to report it, which takes one line in `~/.bashrc` (see
  `config.ini.example`).
- **No SFTP** (e.g. Cisco IOS): the tab offers Upload a file / Download a file using
  `scp -O`, the classic protocol those devices speak.

File dialogs use xdg-desktop-portal, or zenity if there's no portal (tiling window managers),
so install one of them. Drag and drop works either way.

## Transparency

The terminal area is see-through by default (`opacity = 0.90` in the `[window]` section of
`config.ini`). The title bar, tab bar, search bar, pop-ups and text always stay solid, and
so do selection and search highlights. 0.90 reads cleanly over busy wallpapers. With a
compositor that blurs, 0.85 gives a frosted-glass look, and 1.0 turns transparency off.
Changes apply when the file is saved, without a restart.

| Desktop | Transparency |
|---|---|
| GNOME, KDE Plasma, Cinnamon, Xfce (compositing on) | yes |
| Sway, Hyprland, niri, river, Wayfire and other Wayland compositors | yes |
| i3, bspwm, awesome, dwm, Openbox, herbstluftwm (X11) | with picom running |
| X11 without a compositor | drawn opaque (detected automatically) |

On X11, FeTerm checks for a compositor at startup and on every config reload, so you can
start picom and press Ctrl+Shift+R. Window-manager opacity rules (Hyprland `windowrule`,
Sway `opacity`, picom `opacity-rule`) multiply with FeTerm's own setting. FeTerm doesn't ask
for blur itself. Hyprland's `decoration:blur`, picom's blur (`--backend glx`) and GNOME's
Blur my Shell extension blur it automatically.

Programs that paint their own background colour, such as vim colour schemes and tmux status
lines, stay solid. Set `transparent_cell_backgrounds = true` to make them see-through too.

## Network highlighting

Profiles are `.ini` files in `data/highlight/` (bundled) and `~/.config/feterm/highlight/`
(yours; a file with the same profile name replaces the bundled one). In `auto` mode the
profile is chosen from what appears on screen: banners such as `Cisco IOS XE Software`,
`Arista DCS-…` or `JUNOS`, Junos `{master:0}` / `[edit]` lines, and prompt shapes. When the
local shell prompt comes back (shells that send OSC 133 marks), it returns to the fallback.

```ini
[profile]
name = my-vendor
inherits = network-common
detect = My Vendor OS
[rule.interface]
regex = \bport\d+/\d+\b
fg = blue
priority = 50
```

A rule with the same name as an inherited one replaces it; `enabled = false` turns one
off. Colours can be palette names (`blue`, `bright-red`, `color208`) so they follow the
theme, or `#rrggbb`. Values are literal: no quoting, and comments go on their own line.
Rules that backtrack catastrophically are stopped by PCRE2 match limits and disabled.

## Click-to-position

Click inside the command you are typing and the cursor moves there. The terminal sends
the Left/Right keys you would have pressed (or Ctrl-B/Ctrl-F with `click_keys = emacs`),
so it works in bash, zsh and fish, and over ssh in Cisco, Arista and Juniper CLIs. It only
acts on the line being edited, including soft-wrapped continuation rows, and is inactive
in full-screen apps, while scrolled back, or when a program has enabled mouse reporting.
With OSC 133 shell integration it knows exactly where input starts and ignores clicks
while a command is running.

## Icons

The icons in `data/icons/` and the window icon compiled into the binary
(`src/platform/icon_data.h`) are pre-rendered and committed, so building and installing
need no extra tools. X11 and Wayland compositors that support xdg-toplevel-icon (KDE, for
example) show the window icon; GNOME takes the icon from the launcher entry instead. The
"FeTerm>_" lettering is JetBrains Mono Bold (SIL Open Font License) converted to outlines.

Configuration: copy `data/config.ini.example` to `~/.config/feterm/config.ini`.
Themes: `data/themes/*.ini`, or your own in `~/.config/feterm/themes/`.

## License

MIT. Copyright © 2026 Faheem Essack. See [LICENSE](LICENSE).
