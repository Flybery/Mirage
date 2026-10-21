# Fedora port: status and handoff notes

Working notes for the Fedora port on branch `fedora-support` of `https://github.com/Flybery/Vibepollo`.
The user-facing guide is [fedora.md](fedora.md).

## Known-good state (tag `fedora-gnome-good-2026-10-21`)

Verified on the owner's laptop: **Fedora 44 Workstation (GNOME 50, Mutter 50.6), Intel Meteor Lake Arc,
Secure Boot on (MOK enrolled), SELinux enforcing**, client: iPad Mini with Moonlight/VoidLink.

Working:

- RPM build (`scripts/build_fedora_rpm.sh`) and install (`scripts/linux_install.sh`) on Fedora 43/44.
- GNOME mode: `vibepollo-gnome.service` (user unit) streams the existing GNOME desktop through the
  ScreenCast portal; permission is remembered (restore tokens in `~/.config/vibepollo/portal_token*`).
- Intel VAAPI encoding (H.264/HEVC/AV1) and PyroWave; audio; app list; connect/disconnect/reconnect.
- Remote Monitor on GNOME: a ScreenCast **virtual monitor** at the client's resolution, extended next
  to the laptop screen; it can be rearranged in GNOME Settings without being recreated.
- Steam / Big Picture launched through the stream run in their own user scope
  (`app-vibepollo-<pid>-<n>.scope`), not inside the host service.

## How the laptop is driven

- SSH over Tailscale from the Windows dev PC: `bo@100.69.189.33`, key `~/.ssh/vibepollo_fedora`.
  The owner runs every `sudo` command themselves.
- Build on the laptop (no sudo): `scripts/build_fedora_rpm.sh --version 2.0.2 --without-cuda --jobs 16`
  (about 10 minutes; logs to `~/vp-rpm-build.log`, ends with `BUILD-EXIT=`).
- Install (owner, detached so SSH drops cannot interrupt it):
  `sudo systemd-run --unit=vibepollo-install --collect /usr/bin/bash /home/bo/vp-finish-install.sh`
  (`~/vp-finish-install.sh` waits for `BUILD-EXIT=0`, then runs `linux_install.sh --yes --package …`,
  logging to `~/vp-install.log`, ending with `INSTALL-EXIT=`).
- GNOME host: `systemctl --user {status,restart} vibepollo-gnome.service`,
  `journalctl --user -u vibepollo-gnome.service -b`.

## Fixes made along the way (see `git log`)

Packaging/installer: dnf backend, RPM Fusion codecs, kernel-devel naming, Sunshine COPR replacement,
GCC 15 on Fedora 44, Ninja generator, pinned FFmpeg tag for tarball builds, submodule integrity check,
systemd 259 template mask probes, stale control-socket removal, versioned executable symlink, no
prompts in `%post`, queued-MOK detection, DualSense module without srcversion, stopping/starting the
GNOME user unit around package transactions. Core: setBitrate/IDR never start a broadcast (deadlock
after disconnect), PipeWire < 1.6 max-framerate negotiation, tolerant pause for virtual monitors,
tray disabled in GNOME mode, JPEG/WebP cover conversion deps.

## Open items

- While a virtual monitor exists, GNOME treats the monitor set as new and may drop the laptop scale
  from 125% to 100% (needs `org.gnome.Mutter.DisplayConfig` handling).
- Remote Monitor position across reconnects depends on whether Mutter recognizes the virtual monitor.
- CUDA/NVENC build on Fedora 44 (GCC 15 + CUDA 12.9 `-allow-unsupported-compiler`) is untested.
- Flatpak Steam/Lutris games are listed but cannot launch (`/usr/bin/steam`, `/usr/bin/lutris`).
- `vibeshine_ds5` should declare `MODULE_VERSION` upstream (libvirtualgamepad).
- Fedora CI workflow (`.github/workflows/ci-fedora.yml`) has never run.

## Working rules

- Restarting `vibepollo-gnome.service` ends an active stream; warn first.
- Keep Arch behavior unchanged; run `tests/unit/platform/linux/test_linux_installer*.sh`,
  `test_session_controller.sh` and the `test_package_*.py` scripts after packaging edits.
- Never push to `Nonary/Vibepollo`.
