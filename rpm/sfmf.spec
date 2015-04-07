%define package_summary Sailfish OS Factory Snapshot Update
%define version_info %{version}-%{release}

Name:          sfmf
Version:       0.0.7
Release:       1
Summary:       %{package_summary} (metapackage)
Group:         System/Base
License:       TBD
Source0:       %{name}-%{version}.tar.gz
BuildRequires: zlib-devel
BuildRequires: zlib-static
BuildRequires: pkgconfig(libcurl)
Requires:      %{name}-pack = %{version}
Requires:      %{name}-deploy = %{version}
Requires:      %{name}-dump = %{version}

%prep
%setup -q

%build
make PREFIX=%{_prefix} VERSION=%{version_info}

%install
make install PREFIX=%{_prefix} VERSION=%{version_info} DESTDIR=%{buildroot}

%description
Converts a live filesystem into a factory snapshot given a
manifest file built from a factory image, reducing the download
time and disk usage to a minimum by finding files locally first
and creating cross-subvolume btrfs reflinks.

This metapackage installs the deployment tools, the packing tool
and the manifest dump utility used for debugging and development.

%files

%package deploy
Summary: %{package_summary} (deployment)
Requires: sailfish-snapshot >= 1.0.4
Requires: sailfish-snapshot-sbj-config >= 1.0.4
Requires: ssu-slipstream

%description deploy
Unpack utility and scripts for deploying updated factory images.

%files deploy
%attr(755,root,root) %{_bindir}/%{name}-unpack
%attr(755,root,root) %{_bindir}/%{name}-deploy
%attr(755,root,root) %{_bindir}/%{name}-upgrade-factory-snapshot
%attr(644,root,root) %{_sysconfdir}/dbus-1/system.d/org.sailfishos.sfmf.conf
%attr(644,root,root) %{_datadir}/dbus-1/system-services/org.sailfishos.sfmf.ufs.service


%package pack
Summary: %{package_summary} (pack utility)
Requires: btrfs-progs
Requires: python >= 2.7
Requires: simg2img

%description pack
This package contains the utility to create a SFMF directory
tree from a Sailfish OS release tarball. It is supposed to
run on a host/development machine and on release builders.

%files pack
%attr(755,root,root) %{_bindir}/%{name}-pack
%attr(755,root,root) %{_bindir}/%{name}-convert
%attr(755,root,root) %{_bindir}/%{name}-create-tarball


%package dump
Summary: %{package_summary} (dump utilities)

%description dump
This package contains utilities to inspect SFMF manifests.

%files dump
%attr(755,root,root) %{_bindir}/%{name}-dumpmanifest
%attr(755,root,root) %{_bindir}/%{name}-dumppack


%package tests
Summary: %{package_summary} (tests)

%description tests
Unit and regression tests for %{package_summary}.

%files tests
%attr(755,root,root) %{_bindir}/%{name}-tests
