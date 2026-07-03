#!/usr/bin/env bash
set -euo pipefail

WORKDIR="${WORKDIR:-${CATCHEYE_CAPTURE_CONTAINER_WORKDIR:-/home/user/catcheye-capture}}"
# host_arch="$(uname -m)"
# case "${DOCKER_ARCH:-$host_arch}" in
#   x86_64|amd64) arch="amd64" ;;
#   aarch64|arm64) arch="arm64" ;;
#   *) echo "unknown arch: ${DOCKER_ARCH:-$host_arch}" >&2; exit 2 ;;
# esac
arch="arm64"

CONTAINER="${CONTAINER:-catcheye-capture-develop-arm64}"
# if [[ -z "$CONTAINER" ]]; then
#   case "$arch" in
#     amd64) CONTAINER="catcheye-capture-develop-amd64" ;;
#     arm64) CONTAINER="catcheye-capture-develop-arm64" ;;
#   esac
# fi

usage() {
  cat <<'EOF'
Usage:
  scripts/cmake.sh <command> [profile]

Commands:
  configure   Configure CMake
  build       Build
  install     Install
  verify      Run installed app --help
  compile-db  Sync compile_commands.json for host tools
  clean       Remove build/install
  all         Configure, build, install, verify

Profiles:
  debug
  release (default)

Examples:
  scripts/cmake.sh build
  scripts/cmake.sh all release
  DOCKER_ARCH=arm64 scripts/cmake.sh build
  DOCKER_ARCH=arm64 CONTAINER=catcheye-capture-develop-arm64 scripts/cmake.sh all
  scripts/cmake.sh clean
EOF
}

command="${1:-}"
profile="${2:-release}"

if [[ -z "$command" || "$command" == "-h" || "$command" == "--help" ]]; then
  usage
  exit 0
fi

build_dir() {
  case "$profile" in
    debug) echo "build/debug-$arch" ;;
    # release) echo "build/release-$arch" ;;
    release) echo "build/release" ;;
    *) echo "unknown profile: $profile" >&2; exit 2 ;;
  esac
}

install_dir() {
  case "$profile" in
    debug) echo "install/debug-$arch" ;;
    # release) echo "install/release-$arch" ;;
    release) echo "install/release" ;;
    *) echo "unknown profile: $profile" >&2; exit 2 ;;
  esac
}

config_name() {
  case "$profile" in
    debug) echo "Debug" ;;
    release) echo "Release" ;;
    *) echo "unknown profile: $profile" >&2; exit 2 ;;
  esac
}

configure_args() {
  case "$arch:$profile" in
    amd64:debug) echo "-DCMAKE_BUILD_TYPE=Debug -DCATCHEYE_CAPTURE_ENABLE_LIBCAMERA=OFF" ;;
    amd64:release) echo "-DCMAKE_BUILD_TYPE=Release -DCATCHEYE_CAPTURE_ENABLE_LIBCAMERA=OFF" ;;
    arm64:debug) echo "-DCMAKE_BUILD_TYPE=Debug -DCATCHEYE_CAPTURE_ENABLE_LIBCAMERA=ON" ;;
    arm64:release) echo "-DCMAKE_BUILD_TYPE=Release -DCATCHEYE_CAPTURE_ENABLE_LIBCAMERA=ON" ;;
    *) echo "unknown profile: $profile" >&2; exit 2 ;;
  esac
}

in_container() {
  docker exec "$CONTAINER" bash -lc "cd '$WORKDIR' && $*"
}

configure() {
  in_container "cmake -S . -B '$(build_dir)' -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $(configure_args)"
}

build() {
  in_container "cmake --build '$(build_dir)' --config '$(config_name)' -- -j \$(nproc)"
  # scripts/sync-compile-commands.sh "$profile" "$arch"
  scripts/sync-compile-commands.sh "$profile"
}

install_app() {
  in_container "rm -rf '$(install_dir)' && cmake --install '$(build_dir)' --prefix '$(install_dir)'"
}

verify() {
  in_container "'$(install_dir)'/bin/catcheye-capture --help"
}

clean() {
  in_container "rm -rf build install"
}

case "$command" in
  configure) configure ;;
  build) build ;;
  # compile-db) scripts/sync-compile-commands.sh "$profile" "$arch" ;;
  compile-db) scripts/sync-compile-commands.sh "$profile" ;;
  install) install_app ;;
  verify) verify ;;
  clean) clean ;;
  all)
    configure
    build
    install_app
    verify
    ;;
  *)
    echo "unknown command: $command" >&2
    usage >&2
    exit 2
    ;;
esac
