%{!?pkg_version:%global pkg_version 0.3.1}

Name:           windowtolayer
Version:        %{pkg_version}
Release:        1%{?dist}
Summary:        Display Wayland applications as layer-shell wallpaper surfaces

License:        GPL-3.0-or-later
URL:            https://gitlab.freedesktop.org/mstoeckl/windowtolayer
Source0:        %{url}/-/archive/v%{version}/windowtolayer-v%{version}.tar.gz

BuildRequires:  cargo-rpm-macros
BuildRequires:  gcc
BuildRequires:  python3
BuildRequires:  rustfmt
BuildRequires:  pkgconfig(wayland-client) >= 1.20.0
BuildRequires:  pkgconfig(wayland-protocols) >= 1.25
BuildRequires:  pkgconfig(wayland-scanner)

%generate_buildrequires
%cargo_generate_buildrequires

%description
windowtolayer transforms Wayland xdg-shell clients into clients that use
wlr-layer-shell so they can render as wallpaper surfaces on supported
compositors.

%prep
%autosetup -n windowtolayer-v%{version}
%cargo_prep

%build
%cargo_build

%install
install -Dpm0755 target/rpm/%{name} %{buildroot}%{_bindir}/%{name}

%files
%license COPYING
%{_bindir}/%{name}

%changelog
* Thu Jul 02 2026 Syndr <syndr@styx.ultroncore.net> - 0.3.1-1
- Initial COPR package
