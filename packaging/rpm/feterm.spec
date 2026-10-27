Name:           feterm
Version:        0.5.1
Release:        1%{?dist}
Summary:        Modern terminal emulator with network-device syntax highlighting
License:        MIT
URL:            https://gitlab.com/faheeme/feterm
Source0:        %{url}/-/archive/v%{version}/%{name}-v%{version}.tar.gz

BuildRequires:  gcc make pkgconf-pkg-config
BuildRequires:  pkgconfig(sdl3) pkgconfig(libpcre2-8) pkgconfig(freetype2)
BuildRequires:  pkgconfig(fontconfig) pkgconfig(glesv2)
BuildRequires:  libasan libubsan desktop-file-utils
Requires:       hicolor-icon-theme
Recommends:     openssh-clients xdg-utils libnotify

%description
FeTerm is a GPU-rendered terminal emulator for Wayland and X11 with tabs,
split panes, themes, inline images, an SSH/Telnet/serial session manager,
an SFTP file explorer, and PCRE2 syntax highlighting for Cisco, Arista and
Juniper output.

%prep
%autosetup -n %{name}-v%{version}

%build
%make_build PREFIX=%{_prefix} WERROR=

%install
%make_install PREFIX=%{_prefix}

%check
make PREFIX=%{_prefix} WERROR= test
desktop-file-validate %{buildroot}%{_datadir}/applications/io.gitlab.faheeme.%{name}.desktop

%files
%license LICENSE
%doc README.md
%{_bindir}/%{name}
%{_mandir}/man1/%{name}.1*
%{_datadir}/%{name}/
%{_datadir}/applications/io.gitlab.faheeme.%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/io.gitlab.faheeme.%{name}*

%changelog
* Tue Oct 27 2026 Faheem Essack <faheem.essack@gmail.com> - 0.5.1-1
- Security release: harden against malicious servers and hostile terminal output

* Tue Oct 27 2026 Faheem Essack <faheem.essack@gmail.com> - 0.5.0-1
- New upstream release: Windows port; Linux behaviour unchanged

* Mon Oct 26 2026 Faheem Essack <faheem.essack@gmail.com> - 0.4.3-1
- New upstream release: zooming no longer resizes the sidebar

* Fri Oct 23 2026 Faheem Essack <faheem.essack@gmail.com> - 0.4.2-1
- New upstream release: builds on aarch64

* Fri Oct 23 2026 Faheem Essack <faheem.essack@gmail.com> - 0.4.1-1
- Initial package
