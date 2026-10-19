#!/usr/bin/env bash
# Fedora (dnf/rpm) paths of scripts/linux_install.sh, exercised with mocked
# package managers. Never touches the real package database.
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd -P)
source "$repo/scripts/linux_install.sh"
# The installer's own cleanup and signal traps are not part of this fixture.
trap - EXIT HUP INT TERM

fixture=$(mktemp -d /tmp/vibepollo-fedora-installer-test.XXXXXXXX)
trap 'rm -rf -- "$fixture"' EXIT
calls="$fixture/calls"
: > "$calls"

fail_test() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

cat > "$fixture/os-release-fedora" <<'EOF'
NAME="Fedora Linux"
ID=fedora
VERSION_ID=43
EOF
cat > "$fixture/os-release-debian" <<'EOF'
NAME="Debian GNU/Linux"
ID=debian
VERSION_ID=13
EOF
os_release_path="$fixture/os-release-fedora"
ostree_marker="$fixture/ostree-booted"

# Installed RPMs for the current scenario.
declare -A installed_rpms=()
rpm() {
  printf 'rpm %s\n' "$*" >> "$calls"
  case "$1" in
    -q)
      if [[ "$2" == --quiet ]]; then [[ -n "${installed_rpms[$3]:-}" ]]; return; fi
      if [[ "$2" == --queryformat ]]; then
        [[ -n "${installed_rpms[$4]:-}" ]] || return 1
        printf '%s %s\n' "$4" "${installed_rpms[$4]}"
        return
      fi
      ;;
    -qp) printf '%s\n' "$local_rpm_identity" ;;
    -qf) [[ "${!#}" == */vmlinuz ]] || return 1; printf '%s\n' "$kernel_owner" ;;
  esac
}
dnf() { printf 'dnf %s\n' "$*" >> "$calls"; return "${dnf_result:-0}"; }

# --- Backend selection -------------------------------------------------------
(
  package_backend=''
  select_package_backend
  [[ "$package_backend" == dnf ]] || fail_test "Fedora without pacman selected '$package_backend'"
)
(
  package_backend=''
  pacman() { :; }
  select_package_backend
  [[ "$package_backend" == pacman ]] || fail_test 'an available pacman must keep the Arch backend'
)
(
  package_backend=''
  os_release_path="$fixture/os-release-debian"
  select_package_backend
  [[ "$package_backend" == pacman ]] || fail_test 'dnf on a non-Fedora system must not select the Fedora backend'
  if (require_pacman) 2>/dev/null; then fail_test 'non-Fedora system without pacman was accepted'; fi
)
printf 'PASS: package backend selection keeps Arch first and recognizes Fedora.\n'

package_backend=dnf
require_dnf
[[ "$fedora_release" == 43 ]] || fail_test "Fedora release was '$fedora_release'"
touch "$ostree_marker"
if (require_dnf) 2>/dev/null; then fail_test 'rpm-ostree systems must be rejected'; fi
rm -f -- "$ostree_marker"
printf 'PASS: Fedora release detection rejects rpm-ostree systems.\n'

# --- Kernel headers ----------------------------------------------------------
modules_root="$fixture/modules"
mkdir -p "$modules_root/6.17.4-200.fc43.x86_64" "$modules_root/6.17.4-200.fc43.x86_64+debug"
uname() { printf '%s\n' "$mock_release"; }
mock_release=6.17.4-200.fc43.x86_64
kernel_owner=kernel-core
check_kernel "$modules_root" > /dev/null
[[ "$kernel_package" == kernel-core ]] || fail_test "kernel package was '$kernel_package'"
[[ "$headers_package" == kernel-devel-6.17.4-200.fc43.x86_64 ]] || fail_test "headers were '$headers_package'"
mock_release=6.17.4-200.fc43.x86_64+debug
kernel_owner=kernel-debug-core
check_kernel "$modules_root" > /dev/null
[[ "$headers_package" == kernel-debug-devel-6.17.4-200.fc43.x86_64 ]] || fail_test "debug headers were '$headers_package'"
unset -f uname
printf 'PASS: Fedora kernel-devel names match the running kernel exactly.\n'

# --- GPU encoding drivers ----------------------------------------------------
reset_scenario() {
  : > "$calls"
  installed_rpms=()
  gpu_nvidia=0; gpu_amd=0; gpu_intel=0
  assume_yes=0; dnf_confirm=(); install_codecs=1; dnf_result=0
  warnings=()
}

reset_scenario
gpu_amd=1; assume_yes=1; dnf_confirm=(-y)
installed_rpms[mesa-va-drivers]=25.2.0-1.fc43.x86_64
install_gpu_codecs > /dev/null 2>&1
grep -Fxq "dnf install -y https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-43.noarch.rpm https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-43.noarch.rpm" "$calls" ||
  fail_test 'RPM Fusion was not enabled for the running Fedora release'
grep -Fxq 'dnf swap -y --allowerasing mesa-va-drivers mesa-va-drivers-freeworld' "$calls" ||
  fail_test 'AMD did not swap to the RPM Fusion Mesa VA-API driver'
printf 'PASS: AMD hosts swap to mesa-va-drivers-freeworld after enabling RPM Fusion.\n'

reset_scenario
gpu_intel=1
installed_rpms[rpmfusion-free-release]=43-1.noarch
installed_rpms[rpmfusion-nonfree-release]=43-1.noarch
install_gpu_codecs > /dev/null 2>&1
grep -Fxq 'dnf install intel-media-driver' "$calls" || fail_test 'Intel did not install intel-media-driver'
! grep -q 'rpmfusion-free-release-43' "$calls" || fail_test 'RPM Fusion was re-enabled although present'
printf 'PASS: Intel hosts install intel-media-driver.\n'

reset_scenario
gpu_amd=1
install_gpu_codecs < /dev/null > /dev/null 2>&1
! grep -q '^dnf ' "$calls" || fail_test 'repositories changed without consent'
((${#warnings[@]} == 1)) || fail_test 'missing RPM Fusion consent did not warn'
printf 'PASS: RPM Fusion is never enabled without consent.\n'

reset_scenario
gpu_amd=1; install_codecs=0
install_gpu_codecs > /dev/null 2>&1
[[ ! -s "$calls" ]] || fail_test '--no-codecs still inspected or changed packages'
reset_scenario
gpu_amd=1; installed_rpms[mesa-va-drivers-freeworld]=25.2.0-1.fc43.x86_64
install_gpu_codecs > /dev/null 2>&1
! grep -q '^dnf ' "$calls" || fail_test 'an installed codec driver was reinstalled'
printf 'PASS: codec installation is skipped when disabled or already satisfied.\n'

# --- Package installation ----------------------------------------------------
workdir="$fixture/work"
mkdir -p "$workdir"
local_package="$fixture/vibepollo-2.1.0-1.fc43.x86_64.rpm"
touch "$local_package"
local_rpm_identity='vibepollo 2.1.0-1.fc43.x86_64'

reset_scenario
dnf_confirm=(-y)
install_from_rpm > /dev/null 2>&1
grep -Fxq "dnf install -y $local_package" "$calls" || fail_test 'local RPM was not installed with dnf'
reset_scenario
installed_rpms[vibepollo]=2.1.0-1.fc43.x86_64
install_from_rpm > /dev/null 2>&1
grep -Fxq "dnf reinstall $local_package" "$calls" || fail_test 'same-version RPM was not reinstalled'
reset_scenario
installed_rpms[sunshine]=2025.1-1.fc43.x86_64
install_from_rpm > /dev/null 2>&1
grep -Fxq "dnf install --allowerasing $local_package" "$calls" || fail_test 'Sunshine was not replaced'
reset_scenario
local_rpm_identity='sunshine 2025.1-1.fc43.x86_64'
if (install_from_rpm) > /dev/null 2>&1; then fail_test 'a non-Vibepollo RPM was accepted'; fi
! grep -q '^dnf ' "$calls" || fail_test 'a rejected RPM reached dnf'
printf 'PASS: local RPMs are identity-checked and installed, reinstalled or replace Sunshine.\n'

reset_scenario
installed_rpms[vibepollo]=2.1.0-1.fc43.x86_64
remove_vibepollo > /dev/null 2>&1
grep -Fxq 'dnf remove vibepollo' "$calls" || fail_test 'Fedora removal did not use dnf'
printf 'PASS: Fedora removal uses dnf and the package scripts.\n'

# --- Main step order ---------------------------------------------------------
(
  trace="$fixture/main-trace"
  : > "$trace"
  package_backend=''
  for step in parse_args require_root require_pacman run_checks check_session_restart install_vibepollo \
    check_private_host_security install_virtual_driver install_dualsense_driver repair_machine_setup \
    open_firewall check_driver_state check_services print_summary; do
    eval "$step() { :; }"
  done
  require_dnf() { printf 'require_dnf\n' >> "$trace"; }
  install_gpu_codecs() { printf 'codecs\n' >> "$trace"; }
  install_kernel_headers() { printf 'headers\n' >> "$trace"; }
  main
  [[ $(< "$trace") == $'require_dnf\ncodecs\nheaders' ]] || fail_test "unexpected Fedora main order: $(< "$trace")"
)
printf 'PASS: Fedora installation installs encoding drivers before kernel headers.\n'
