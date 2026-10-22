# FeTerm

A GPU-accelerated terminal emulator in C23 that runs natively on Wayland and X11 through SDL3.
The name lives in two Makefile variables: `NAME` (binary, config and data directory, app-id)
and `DISPLAY_NAME` (window title, `--version`, `TERM_PROGRAM`).

**Status: all four planned phases are done.** Core terminal, PCRE2 network highlighting
(Cisco, Arista, Juniper, shared network profile), click-to-position, selection and copy,
mouse reporting, links, scrollback search, reflow, configurable keys, live reload, tabs
and split panes, the kitty keyboard protocol, desktop notifications, drawn box
characters, cursor animation, 12 themes, and inline images (sixel and kitty graphics).

## Build

Fedora:

```sh
sudo dnf install gcc make pkgconf-pkg-config SDL3-devel pcre2-devel freetype-devel \
                 fontconfig-devel libglvnd-devel
```

EL10 / Rocky 10: run `sudo dnf config-manager --set-enabled crb` first, then the same command.

```sh
make            # ./feterm
make test       # headless core tests under ASan + UBSan
make debug      # ./feterm built with ASan + UBSan
make analyze    # GCC -fanalyzer over all sources
sudo make install PREFIX=/usr/local
```

`make install` also installs the launcher entry (`io.github.feterm.desktop`) and the icons
into the hicolor theme: PNGs from 16 to 512 px, a scalable SVG and a symbolic panel icon.
The window's app-id matches the launcher's name, so docks and task switchers show the icon
on Wayland and X11.

Run from the source tree with `./feterm`: themes are found in `./data` next to the binary.

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

Every shortcut can be changed in the `[keys]` section of `config.ini`.

Programs can send desktop notifications (OSC 9, OSC 777, OSC 99); by default they are
shown only when you are not looking at that pane, at most one every two seconds, via
`notify-send`. Images use sixel or the kitty graphics protocol with raw RGB/RGBA data
(`f=24` / `f=32`); PNG data (`f=100`) is answered with an error.

Pasting several lines into a program that does not use bracketed paste asks for
confirmation first, because each line would run as a command as soon as it arrived.
Links open through `xdg-open` without a shell, and only for http(s), ftp, sftp, ssh,
file and mailto links.

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
