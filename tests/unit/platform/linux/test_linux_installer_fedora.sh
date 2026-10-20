#!/usr/bin/env bash
# Fedora (dnf/rpm) paths of scripts/linux_install.sh, exercised with mocked
# package managers. Never touches the real package database.
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd -P)
source "$repo/scripts/linux_install.sh"
# The installer's own cleanup and signal traps are not part of this fixture.
trap - EXIT HUP INT TERM
# Never touch the real user managers; the dedicated test below uses mocks.
eval "$(declare -f stop_gnome_user_services | sed '1s/stop_gnome_user_services/stop_gnome_user_services_real/')"
stop_gnome_user_services() { :; }

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
installed_rpms[Sunshine]=2026.1017.185028-1.fc44.x86_64  # LizardByte COPR spelling
install_from_rpm > /dev/null 2>&1
grep -Fxq "dnf install --allowerasing $local_package" "$calls" || fail_test 'COPR Sunshine was not replaced'
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

# --- Queued Secure Boot enrollment -------------------------------------------
(
  certificate="$fixture/mok.pub"
  touch "$certificate"
  openssl() { printf 'SHA1 Fingerprint=AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01\n'; }
  mokutil() { printf '%s\n' "$pending_list"; }
  pending_list='[key 1]
SHA1 Fingerprint: ab:cd:ef:01:23:45:67:89:ab:cd:ef:01:23:45:67:89:ab:cd:ef:01'
  signing_key_enrollment_pending "$certificate" || fail_test 'a queued DKMS key was not recognized'
  pending_list='SHA1 Fingerprint: 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00'
  if signing_key_enrollment_pending "$certificate"; then fail_test 'another queued key was accepted'; fi
  pending_list=''
  if signing_key_enrollment_pending "$certificate"; then fail_test 'an empty MOK queue was accepted'; fi
  if signing_key_enrollment_pending "$fixture/absent.pub"; then fail_test 'a missing certificate was accepted'; fi
)
printf 'PASS: an already queued DKMS signing key is recognized without prompting again.\n'

# --- DualSense module without srcversion -------------------------------------
(
  module_root="$fixture/sys-module"
  mkdir -p "$module_root/vibeshine_ds5"
  kernel_release=7.2.11-200.fc44.x86_64
  modinfo() {
    case "$*" in
      *srcversion*) printf '%s\n' "$module_srcversion" ;;
      *filename*) printf '/lib/modules/%s/extra/vibeshine_ds5.ko.xz\n' "$kernel_release" ;;
    esac
  }
  module_srcversion=''
  dualsense_loaded_without_srcversion "$module_root" || fail_test 'a loaded module on a srcversion-less kernel was rejected'
  module_srcversion=ABCDEF0123456789
  if dualsense_loaded_without_srcversion "$module_root"; then fail_test 'a module with srcversion bypassed the freshness check'; fi
  module_srcversion=''
  touch "$module_root/vibeshine_ds5/srcversion"
  if dualsense_loaded_without_srcversion "$module_root"; then fail_test 'a loaded srcversion bypassed the freshness check'; fi
  rm -rf "$module_root/vibeshine_ds5"
  if dualsense_loaded_without_srcversion "$module_root"; then fail_test 'an unloaded module was accepted'; fi
)
printf 'PASS: a loaded DualSense module is accepted on kernels that record no srcversion.\n'

# --- GNOME user service ------------------------------------------------------
(
  unit_root="$fixture/user-units"
  mkdir -p "$unit_root"
  touch "$unit_root/vibepollo-gnome.service"
  systemctl() { printf 'systemctl %s\n' "$*" >> "$calls"; }
  loginctl() {
    case "$*" in
      *ActiveSession*) printf '5\n' ;;
      *Name*) printf 'bo\n' ;;
      *Class*) printf '%s\n' "$session_class" ;;
    esac
  }
  reset_scenario
  installed_rpms[gnome-shell]=50.5-1.fc44.x86_64
  session_class=user
  enable_gnome_user_service "$unit_root" > /dev/null 2>&1
  grep -Fxq 'systemctl --global enable vibepollo-gnome.service' "$calls" || fail_test 'GNOME unit was not enabled'
  grep -Fxq 'systemctl --user --machine=bo@.host start vibepollo-gnome.service' "$calls" ||
    fail_test 'GNOME unit was not started for the logged-in user'
  reset_scenario
  installed_rpms[gnome-shell]=50.5-1.fc44.x86_64
  session_class=greeter
  enable_gnome_user_service "$unit_root" > /dev/null 2>&1
  ! grep -q -- '--machine=' "$calls" || fail_test 'the GNOME unit was started for a greeter session'
  reset_scenario
  enable_gnome_user_service "$unit_root" > /dev/null 2>&1
  ! grep -q '^systemctl' "$calls" || fail_test 'the GNOME unit was enabled without GNOME installed'
)
printf 'PASS: GNOME hosts enable the user service and start it in the active desktop session.\n'

(
  unit_root="$fixture/user-units"
  mkdir -p "$unit_root"
  touch "$unit_root/vibepollo-gnome.service"
  loginctl() { printf '1000 bo no active\n1001 alice no lingering\n42 bad:name no active\n'; }
  systemctl() {
    printf 'systemctl %s\n' "$*" >> "$calls"
    case "$*" in
      *bo@.host\ is-active*) return 0 ;;
      *is-active*) return 3 ;;
    esac
  }
  : > "$calls"
  stop_gnome_user_services_real "$unit_root" > /dev/null 2>&1
  grep -Fxq 'systemctl --user --machine=bo@.host stop vibepollo-gnome.service' "$calls" ||
    fail_test 'a running GNOME host was not stopped before the package transaction'
  ! grep -q 'alice@.host stop' "$calls" || fail_test 'an inactive GNOME unit was stopped'
  ! grep -q 'bad:name' "$calls" || fail_test 'an invalid user name reached systemctl'
  : > "$calls"
  stop_gnome_user_services_real "$fixture/absent-units" > /dev/null 2>&1
  [[ ! -s "$calls" ]] || fail_test 'user managers were queried without the GNOME unit installed'
)
printf 'PASS: running GNOME hosts are stopped before the package is replaced.\n'

# --- Main step order ---------------------------------------------------------
(
  trace="$fixture/main-trace"
  : > "$trace"
  package_backend=''
  for step in parse_args require_root require_pacman run_checks check_session_restart install_vibepollo \
    check_private_host_security install_virtual_driver install_dualsense_driver repair_machine_setup \
    enable_gnome_user_service open_firewall check_driver_state check_services print_summary; do
    eval "$step() { :; }"
  done
  require_dnf() { printf 'require_dnf\n' >> "$trace"; }
  install_gpu_codecs() { printf 'codecs\n' >> "$trace"; }
  install_kernel_headers() { printf 'headers\n' >> "$trace"; }
  main
  [[ $(< "$trace") == $'require_dnf\ncodecs\nheaders' ]] || fail_test "unexpected Fedora main order: $(< "$trace")"
)
printf 'PASS: Fedora installation installs encoding drivers before kernel headers.\n'
