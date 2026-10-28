Mirage streams your Linux GNOME desktop to Moonlight on an iPad, phone, TV or PC.
Each device gets its own Mirage virtual display at its exact resolution, and the
laptop panel switches off while you stream. The same virtual display also works as a
second monitor (remote monitor): set the layout to *Extended* and the device becomes an
extra screen next to the laptop's own. Mirage is a fork of
[Vibepollo](https://github.com/Nonary/Vibepollo) (itself based on Apollo and
Sunshine), focused on Fedora Workstation with GNOME.

## Install

### Fedora 43 and 44 (GNOME, x86_64)

```
curl -fsSLO https://github.com/Flybery/Mirage/releases/download/@VERSION@/linux_install.sh
sudo bash linux_install.sh --version @VERSION@
```

The script checks the machine, offers RPM Fusion and the hardware-encoding drivers
for your GPU, installs the RPM that matches your Fedora release and opens the
firewall. Reboot if it says so, sign in to GNOME, then open
**https://localhost:47990** to create your login and pair Moonlight.

To install a downloaded package instead:
`sudo bash linux_install.sh --package ./vibepollo-@VERSION@-1.fc44.x86_64.rpm`

### Arch Linux and CachyOS (x86_64)

Same two commands as above. The Arch package is built from the same source but is
not tested by the Mirage project yet.

### Windows and macOS

Mirage only ships Linux packages. On Windows, use
[Vibepollo](https://github.com/Nonary/Vibepollo/releases), which Mirage is based on.

### Notes

- The package and its services are still named `vibepollo` internally; the app
  shows up as Mirage.
- `SHA256SUMS` lists the checksums of every file in this release. The install
  script verifies the package against it.
