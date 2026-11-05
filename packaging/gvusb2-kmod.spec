%global buildforkernels akmod
%global debug_package %{nil}

# Out-of-tree modules must not be built with LTO: the Fedora default
# -flto=auto produces a .gnu.linkonce.this_module section whose size does not
# match the running kernel's struct module, and modprobe then fails with
# "section size must match the kernel's built struct module size at run time".
%global _lto_cflags %{nil}

%global prjname gvusb2

Name:           %{prjname}-kmod
Summary:        Kernel modules for the I-O DATA GV-USB2 USB capture device
Version:        0.1.0
Release:        1%{?dist}
License:        GPL-2.0-only AND BSD-3-Clause
URL:            https://github.com/kurobeats/GV-USB2-Driver
Source0:        %{prjname}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  elfutils-libelf-devel
BuildRequires:  kmodtool

# kmodtool does its magic here
%{expand:%(kmodtool --target %{_target_cpu} --kmodname %{prjname} %{?buildforkernels:--%{buildforkernels}} %{?kernels:--for-kernels "%{?kernels}"} 2>/dev/null)}

%description
Video4Linux2 and ALSA kernel modules for the I-O DATA GV-USB2 USB 2.0 video
capture device (USB 04bb:0532).

The device is a TW9910 analog decoder behind a custom bridge. These modules
provide raw UYVY capture over V4L2 and 48 kHz S16_LE stereo capture over ALSA.

This package contains the kernel modules built against the running kernel by
akmods, so they are rebuilt automatically after a kernel update.


%package -n %{prjname}-kmod-common
Summary:        Common files for the %{prjname} kernel modules
BuildArch:      noarch

%description -n %{prjname}-kmod-common
Configuration shared by the %{prjname} kernel modules: the modules-load.d file
that pulls the capture modules in at boot.


%prep
# error out if there was something wrong with kmodtool
%{?kmodtool_check}

kmodtool --target %{_target_cpu} --kmodname %{prjname} %{?buildforkernels:--%{buildforkernels}} %{?kernels:--for-kernels "%{?kernels}"} 2>/dev/null

%setup -q -c

for kernel_version in %{?kernel_versions} ; do
  cp -a %{prjname}-%{version} _kmod_build_${kernel_version%%___*}
done


%build
for kernel_version in %{?kernel_versions} ; do
  make V=1 %{?_smp_mflags} -C ${kernel_version##*___} M=${PWD}/_kmod_build_${kernel_version%%___*} modules
done


%install
for kernel_version in %{?kernel_versions}; do
  mkdir -p %{buildroot}%{kmodinstdir_prefix}/${kernel_version%%___*}/%{kmodinstdir_postfix}/
  install -D -m 755 _kmod_build_${kernel_version%%___*}/gvusb2-video.ko %{buildroot}%{kmodinstdir_prefix}/${kernel_version%%___*}/%{kmodinstdir_postfix}/gvusb2-video.ko
  install -D -m 755 _kmod_build_${kernel_version%%___*}/gvusb2-sound.ko %{buildroot}%{kmodinstdir_prefix}/${kernel_version%%___*}/%{kmodinstdir_postfix}/gvusb2-sound.ko
done
%{?akmod_install}

mkdir -p %{buildroot}%{_sysconfdir}/modules-load.d
cat > %{buildroot}%{_sysconfdir}/modules-load.d/%{prjname}.conf <<'EOF'
# I-O DATA GV-USB2: load the capture modules at boot.
gvusb2-video
gvusb2-sound
EOF


%files -n %{prjname}-kmod-common
%{_sysconfdir}/modules-load.d/%{prjname}.conf


%changelog
* Wed Nov 04 2026 ants <ants@localhost> - 0.1.0-1
- Initial akmod packaging for gvusb2
