# Fedora port: handoff notes

Working notes for continuing the Fedora port on a real Fedora machine (branch `fedora-support`).
The user-facing guide is [fedora.md](fedora.md).

## Goal and target machines

Make the Linux machine-service build of Vibepollo install and stream well on Fedora, for any GPU
vendor, and keep the Arch/CachyOS path unchanged.

- Owner's machine: **Fedora 44 Workstation (GNOME), Intel GPU**, Secure Boot state unknown. Plan:
  add KDE Plasma next to GNOME (`sudo dnf group install kde-desktop`), keep GDM, log in to
  **Plasma (Wayland)**.
- A friend's machine runs **Fedora 43**; GPU unknown. Both releases must work.
- Chosen approach: keep the upstream KDE machine-service design (not a GNOME port). GNOME sessions
  are intentionally not streamed.

## What changed on this branch

| Area | Change |
| --- | --- |
| Session controller | Accepts Plasma desktop sessions started by GDM (`gdm-password`, `gdm-autologin`, `gdm-fingerprint`, `gdm-smartcard`). Readiness still requires the Plasma user units, so GNOME sessions are rejected. The GDM greeter is never streamed. |
| `scripts/linux_install.sh` | dnf backend selected only when pacman is absent and `/etc/os-release` is Fedora. Offers RPM Fusion with consent, installs the GPU encoding driver (AMD `mesa-va-drivers-freeworld`, Intel `intel-media-driver`, NVIDIA `xorg-x11-drv-nvidia-cuda-libs`), installs `kernel-devel-$(uname -r)`, installs/reinstalls a local RPM, removes with dnf, rejects rpm-ostree. |
| `packaging/linux/copr/Sunshine.spec` | Missing BuildRequires added; Ninja generator; `--without cuda` switch; GCC 15 on Fedora 44+ (no gcc14 there); FFmpeg release tag passed in; versioned executable packaged; Requires `dkms`, `mokutil`, `dbus-update-activation-environment`; `%post` uses `vibeshine-drm-install install-package` (MOK enrollment, status 5) and loads `uinput`. |
| `scripts/build_fedora_rpm.sh` | One-command RPM build from the checkout. |
| `cmake/dependencies/ffmpeg.cmake` | Accepts `-DFFMPEG_RELEASE_TAG`; only queries git when build-deps is a real checkout. |
| `src/platform/linux/pipewire_format.h` | On PipeWire < 1.6 (Fedora 43 has 1.4) offer the maximum as the default so KWin does not negotiate a 1 fps maximum. |
| `scripts/linux_build.sh` | Fedora dependency list fixed; GCC 15 for Fedora 44/45. |
| Tests | New `tests/unit/platform/linux/test_linux_installer_fedora.sh`; updated controller tests; PipeWire test skip for < 1.6; tray-metadata test uses the right Python variable. |
| CI | `.github/workflows/ci-fedora.yml` (Fedora 43/44 RPM build and contract check, manual trigger). Never run yet. |

## Verified so far (in Docker containers, not on hardware)

- Fedora 43: full CMake build with `BUILD_WERROR=ON`; 217/218 ctest pass. The remaining failure,
  `test_component_linux_capability_sanitizer`, needs file capabilities, which containers lack.
- Fedora 43 and 44: `scripts/build_fedora_rpm.sh --version 2.0.2 --install-deps --without-cuda`
  succeeds; the RPM contains the expected files and file capabilities
  (`vibepollo-host cap_sys_admin,cap_sys_nice=p`, `vibepollo-session-broker cap_kill,cap_setgid,cap_setuid=p`);
  `dnf install` with scriptlets disabled resolves every dependency; no unresolved libraries.
- Arch installer test (`test_linux_installer.sh`) output is identical before and after the change.

## Not verified yet: do this on the Fedora machine

1. `sudo dnf upgrade --refresh` and reboot (Fedora mirrors only carry the newest `kernel-devel`).
2. Install Plasma and log in to **Plasma (Wayland)** from GDM at least once.
3. Build: `scripts/build_fedora_rpm.sh --version 2.0.2 --install-deps --without-cuda`
   (about 8 GB RAM at default jobs; use `--jobs 4` if short on memory).
4. Install: `sudo bash scripts/linux_install.sh --package build/rpm/vibepollo-2.0.2-1.fc44.x86_64.rpm`.
   Watch the RPM scriptlets (`%pre`/`%post` are large and have never run on Fedora), the DKMS
   build of `vibeshine_drm`, MOK enrollment, `configure-auto`, and firewalld.
5. Reboot, approve **Enroll MOK** if shown, log in to Plasma (Wayland).
6. Check:
   ```bash
   sudo systemctl --no-pager status vibepollo-session-controller.service vibepollo.service
   sudo journalctl -u vibepollo-session-controller.service -u vibepollo.service -b --no-pager | tail -80
   getcap /usr/libexec/vibeshine/vibepollo-host /usr/libexec/vibeshine/vibepollo-session-broker
   modinfo -F version vibeshine_drm; cat /sys/module/vibeshine_drm/version
   mokutil --sb-state; vainfo 2>/dev/null | grep -i enc
   sudo ausearch -m avc -ts boot | grep -iE 'vibe|kwin'
   ```
7. Open `https://localhost:47990`, create the login, pair Moonlight, stream the desktop. Test a
   locked screen, suspend/resume, and an upgrade (rebuild with a higher version and reinstall).

Expect the controller to report the GDM session as `desktop`. If it logs `unsupported-session`,
compare `loginctl show-session $(loginctl show-seat seat0 -p ActiveSession --value)` with
`desktop_service_supported` and the unit checks in `packaging/linux/vibepollo-session-controller`.

## Known open items

- CUDA builds on Fedora 44 use CUDA 12.9 with `-allow-unsupported-compiler` (GCC 15): untested.
- Flatpak Steam/Lutris games are listed but cannot launch (`/usr/bin/steam`, `/usr/bin/lutris` are hard-coded).
- No SELinux policy exists; collect AVC denials if anything fails.
- Pre-login (greeter) streaming needs Plasma Login Manager and NVIDIA; never with GDM.
- Cosmetic rpmbuild warnings: desktop file `Icon=apollo.svg` has an extension; empty `%changelog`.

## Working rules

- Restarting `vibepollo-session-controller` or `vibepollo` ends an active stream; warn first.
- Keep Arch behavior unchanged; run `tests/unit/platform/linux/test_linux_installer.sh`,
  `test_linux_installer_fedora.sh` and `test_session_controller.sh` after installer/controller edits.
- Work on branch `fedora-support` in the fork `https://github.com/Flybery/Vibepollo`; do not push to
  `Nonary/Vibepollo`.
