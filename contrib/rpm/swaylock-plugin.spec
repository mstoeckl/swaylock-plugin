%{!?pkg_version:%global pkg_version 1.8.6}

Name:           swaylock-plugin
Version:        %{pkg_version}
Release:        %{?pkg_release}%{!?pkg_release:1}.syndr%{?dist}
Summary:        Screen locker with plugin support for Wayland compositors

License:        MIT
URL:            https://github.com/syndr/swaylock-plugin
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  pam-devel
BuildRequires:  scdoc
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(gdk-pixbuf-2.0)
BuildRequires:  pkgconfig(libsystemd)
BuildRequires:  pkgconfig(wayland-client) >= 1.20.0
BuildRequires:  pkgconfig(wayland-protocols) >= 1.47
BuildRequires:  pkgconfig(wayland-scanner) >= 1.15.0
BuildRequires:  pkgconfig(wayland-server) >= 1.20.0
BuildRequires:  pkgconfig(xkbcommon)

Requires:       python3

%description
swaylock-plugin is a Wayland screen locker derived from swaylock that can run
plugin commands as lockscreen backgrounds.

%prep
%autosetup -n %{name}-%{version}

%build
%meson \
    -Dpam=enabled \
    -Dlogind=enabled \
    -Dgdk-pixbuf=enabled \
    -Dman-pages=enabled
%meson_build

%install
%meson_install
install -Dpm0755 example_xwayland_wrapper.py \
    %{buildroot}%{_libexecdir}/%{name}/example_xwayland_wrapper.py

%files
%license LICENSE
%{_bindir}/swaylock-plugin
%{_bindir}/swaylock-sleep-watcher
%{_libexecdir}/%{name}/example_xwayland_wrapper.py
%{_mandir}/man1/swaylock-plugin.1*
%{_datadir}/bash-completion/completions/swaylock-plugin
%{_datadir}/fish/vendor_completions.d/swaylock-plugin.fish
%{_datadir}/zsh/site-functions/_swaylock-plugin
%config(noreplace) %{_sysconfdir}/pam.d/swaylock-plugin

%changelog
* Thu Jul 02 2026 Syndr <syndr@styx.ultroncore.net> - 1.8.6-1
- Initial COPR package for the syndr swaylock-plugin fork
