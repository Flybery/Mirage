#!/usr/bin/env bash
# Build a Vibepollo RPM for the running Fedora release from this checkout.
#
# The RPM is built from packaging/linux/copr/Sunshine.spec with the tracked
# files of this checkout (including uncommitted edits) and all submodules.
# The build downloads FFmpeg, Boost, npm packages and, unless --without-cuda
# is given, the CUDA toolkit, so network access is required.
#
# Usage:
#   scripts/build_fedora_rpm.sh [options]
#
# Options:
#   --version VERSION   Package version (default: BUILD_VERSION, else the exact
#                       Git tag of HEAD). Example: 2.0.2
#   --without-cuda      Skip CUDA/NVENC GPU-resident capture (AMD/Intel hosts).
#   --install-deps      Install the spec's BuildRequires with sudo dnf builddep.
#   --jobs N            Parallel compile jobs (default: CPU count, max 8).
#   --output DIR        Directory for the finished RPMs (default: build/rpm).
#   -h, --help          Show this help.
#
# Then install with:  sudo bash scripts/linux_install.sh --package build/rpm/vibepollo-*.rpm

set -euo pipefail

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=${BUILD_VERSION:-}
with_cuda=1
install_deps=0
jobs=''
output="$repo/build/rpm"

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '==> %s\n' "$*"; }

while (($# > 0)); do
  case "$1" in
    --version) [[ $# -ge 2 ]] || die '--version requires a value'; version=${2#v}; shift 2 ;;
    --without-cuda) with_cuda=0; shift ;;
    --install-deps) install_deps=1; shift ;;
    --jobs) [[ $# -ge 2 && "$2" =~ ^[1-9][0-9]*$ ]] || die '--jobs requires a positive number'; jobs=$2; shift 2 ;;
    --output) [[ $# -ge 2 ]] || die '--output requires a directory'; output=$2; shift 2 ;;
    -h | --help) usage; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
done

# Build as a normal user; containers that only have root skip sudo.
sudo_cmd=(sudo)
[[ $EUID -ne 0 ]] || sudo_cmd=()
command -v rpmbuild >/dev/null 2>&1 || die 'rpmbuild is missing: sudo dnf install rpm-build'
command -v git >/dev/null 2>&1 || die 'git is required'

cd "$repo"
if [[ -z "$version" ]]; then
  version=$(git describe --tags --exact-match HEAD 2>/dev/null || true)
  version=${version#v}
fi
[[ "$version" =~ ^[1-9][0-9]*\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.-]+)?$ ]] ||
  die "a release version is required (for example --version 2.0.2); got '${version:-nothing}'"
commit=$(git rev-parse HEAD)
branch=$(git rev-parse --abbrev-ref HEAD)
[[ -n "$jobs" ]] || jobs=$(($(nproc) < 8 ? $(nproc) : 8))

topdir="$repo/build/rpmbuild"
rm -rf -- "$topdir"
mkdir -p "$topdir"/{SOURCES,SPECS,BUILD,RPMS,SRPMS} "$output"

log 'Updating submodules'
git -c submodule.fetchJobs=8 submodule update --init --recursive --depth 1

# The tarball loses submodule metadata, so resolve the pinned FFmpeg release
# here; otherwise CMake would query this enclosing checkout's tags.
git -C third-party/build-deps fetch -q --tags --depth=1 2>/dev/null || true
ffmpeg_tag=$(git -C third-party/build-deps describe --tags --exact-match 2>/dev/null ||
  git -C third-party/build-deps tag --points-at HEAD 2>/dev/null | head -n1)
[[ "$ffmpeg_tag" =~ ^[A-Za-z0-9._-]+$ ]] ||
  die 'could not determine the build-deps release tag for FFmpeg; check third-party/build-deps'

log 'Creating the source tarball from tracked files and submodules'
# Untracked files are not packaged; `git add` new files first.
git ls-files --recurse-submodules -z |
  tar --null --no-recursion -T - -czf "$topdir/SOURCES/tarball.tar.gz"

spec="$topdir/SPECS/vibepollo.spec"
sed -e "s|^%global build_version 0\$|%global build_version ${version}|" \
    -e "s|^%global branch 0\$|%global branch ${branch}|" \
    -e "s|^%global commit 0\$|%global commit ${commit}|" \
    "$repo/packaging/linux/copr/Sunshine.spec" > "$spec"
grep -qx "%global build_version ${version}" "$spec" || die 'could not set the spec version'

cuda_args=()
((with_cuda)) || cuda_args=(--without cuda)

if ((install_deps)); then
  log 'Installing build dependencies'
  "${sudo_cmd[@]}" dnf builddep -y "$spec"
fi

log "Building vibepollo ${version} ($( ((with_cuda)) && echo 'with' || echo 'without') CUDA, ${jobs} jobs)"
RPM_BUILD_NCPUS=$jobs rpmbuild -ba "${cuda_args[@]}" \
  --define "_topdir $topdir" \
  --define "ffmpeg_release_tag $ffmpeg_tag" \
  "$spec"

find "$topdir/RPMS" -name 'vibepollo-*.rpm' ! -name '*debug*' -exec cp -t "$output" {} +
log "RPMs written to ${output}:"
ls -1 "$output"
