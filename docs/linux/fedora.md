# Vibepollo on Fedora (experimental)

Fedora uses the same machine-wide services as the Arch package, installed from an RPM. Read the
[Linux install guide](install.md) first; this page covers only what differs on Fedora.
Porting status and the hardware test plan are in [fedora-handoff.md](fedora-handoff.md).

## Requirements

| Requirement | Fedora detail |
| --- | --- |
| Release | Fedora 43 or 44, x86_64. Fedora Atomic desktops (Kinoite, Silverblue, Bazzite) are not supported. |
| Desktop | KDE Plasma 6 on **Wayland**. Use Fedora KDE Plasma Desktop, or add Plasma to Fedora Workstation with `sudo dnf group install kde-desktop` and choose **Plasma (Wayland)** on the login screen. GNOME sessions are not streamed. |
| Login manager | SDDM, Plasma Login Manager or GDM. The login screen itself is streamed only with Plasma Login Manager on NVIDIA; otherwise streaming starts after you log in. Automatic login avoids having to sign in locally after a reboot. |
| Kernel | Linux 6.16 or newer with the matching `kernel-devel`. Fedora mirrors carry headers only for the newest kernel, so install updates and reboot before installing Vibepollo. |
| Encoding drivers | Fedora's own Mesa and Intel media drivers cannot encode H.264/HEVC. The installer offers [RPM Fusion](https://rpmfusion.org/Configuration) and installs the right driver for your GPU (see below). |
| Secure Boot | Supported. The DKMS signing key needs a one-time MOK enrollment at the next reboot, separate from any akmods key you enrolled for NVIDIA. |

### GPU encoding drivers

| GPU | Package from RPM Fusion | Notes |
| --- | --- | --- |
| AMD | `mesa-va-drivers-freeworld` | Replaces Fedora's `mesa-va-drivers` (`dnf swap`). |
| Intel | `intel-media-driver` | Replaces Fedora's `libva-intel-media-driver`. Broadwell (5th gen) or newer. |
| NVIDIA | `akmod-nvidia` and `xorg-x11-drv-nvidia-cuda-libs` | The second package provides NVENC (`libnvidia-encode`, `libcuda`). Install the driver yourself and reboot first. |

Check encoding support with `vainfo` (package `libva-utils`): AMD and Intel need lines such as
`VAProfileH264Main : VAEntrypointEncSlice`.

## Build the RPM

Until Fedora RPMs are published with releases, build one from a checkout on the Fedora machine:

```bash
sudo dnf install git rpm-build dnf-plugins-core
git clone https://github.com/Nonary/Vibepollo.git && cd Vibepollo
scripts/build_fedora_rpm.sh --version 2.0.2 --install-deps --without-cuda
```

Leave out `--without-cuda` on NVIDIA machines: the build then downloads the CUDA 12.9 toolkit for
GPU-resident capture. The build also downloads FFmpeg, Boost and the web UI's npm packages. It
needs about 8 GB of RAM at the default parallelism; use `--jobs 4` on smaller machines.

## Install

```bash
sudo bash scripts/linux_install.sh --package build/rpm/vibepollo-*.rpm
```

The installer checks the requirements, offers RPM Fusion and the encoding driver for your GPU
(`--no-codecs` skips this), installs `kernel-devel` for the running kernel, installs the RPM,
builds and signs the virtual-display driver, configures the machine profile, opens firewalld, and
tells you whether to reboot. Reboot after the first installation and approve the MOK enrollment if
the blue MOK Manager screen appears.

Remove Vibepollo with `sudo bash scripts/linux_install.sh --remove` (or `sudo dnf remove vibepollo`).
Removal keeps the machine profile and pairings in `/var/lib/vibepollo`.

## Known differences from Arch

- **Steam and Lutris**: games launch through `/usr/bin/steam` and `/usr/bin/lutris`. Use the RPM
  Fusion `steam` package and the Fedora `lutris` package. Flatpak Steam and Lutris libraries are
  listed but their games do not launch yet.
- **SELinux** stays enforcing. If a stream fails, look for denials:
  `sudo ausearch -m avc -ts boot | grep -iE 'vibe|kwin'` and include them in a bug report.
- **Firewall**: the installer enables the `vibepollo` firewalld service in the default zone.
  Fedora Workstation's default zone already allows high ports; Fedora KDE and Server zones do not.

## Verify

```bash
sudo systemctl --no-pager status vibepollo-session-controller.service vibepollo.service
sudo journalctl -u vibepollo-session-controller.service -u vibepollo.service -b --no-pager | tail -50
getcap /usr/libexec/vibeshine/vibepollo-host   # cap_sys_admin,cap_sys_nice=p
modinfo -F version vibeshine_drm; cat /sys/module/vibeshine_drm/version
mokutil --sb-state; mokutil --list-new
vainfo 2>/dev/null | grep -i enc
```

Then open `https://localhost:47990`, create the Web UI login, and pair Moonlight.
