#!/usr/bin/env bash

set -euo pipefail

detect_package_manager() {
  pkg_mgr="unknown"
  case "$(uname -s)" in
    Linux)
      if command -v apt-get &>/dev/null; then
        pkg_mgr="apt"
      elif command -v dnf &>/dev/null; then
        pkg_mgr="dnf"
      elif command -v yum &>/dev/null; then
        pkg_mgr="yum"
      elif command -v pacman &>/dev/null; then
        pkg_mgr="pacman"
      elif command -v apk &>/dev/null; then
        pkg_mgr="apk"
      fi
      ;;
    Darwin)
      if command -v brew &>/dev/null; then
        pkg_mgr="brew"
      elif command -v port &>/dev/null; then
        pkg_mgr="ports"
      fi
      ;;
    FreeBSD)
      if command -v pkg &>/dev/null; then
        pkg_mgr="pkg"
      fi
      ;;
    *)
      ;;
  esac
  echo "${pkg_mgr}"
}

install_packages() {
  local pmgr="$1"
  local inst_opts=""

  installer="$(command -v "${pmgr}")"
  "${installer}" update

  case "$pmgr" in
    apt)
      pkg_list="avahi-utils libairspy-dev libairspyhf-dev libavahi-client-dev libbsd-dev libfftw3-dev libhackrf-dev libiniparser-dev libncurses5-dev libopus-dev librtlsdr-dev libusb-1.0-0-dev libusb-dev portaudio19-dev libasound2-dev uuid-dev libogg-dev libsamplerate-dev libliquid-dev libncursesw5-dev libhackrf-dev libbladerf-dev"
      installer="${installer} install -y"
      ;;
    brew)
      pkg_list="fftw opus iniparser hidapi airspy airspyhf hackrf ncurses librtlsdr libogg libsamplerate libbladerf portaudio"
      installer="${installer} install"
      ;;
    pkg)
      pkg_list="avahi avahi-libdns airspy airspyhf fftw3 hackrf iniparser ncurses opus portaudio libuuid libogg libsamplerate liquid-dsp hackrf" 
      installer="${installer} install --yes"
      ;;
    *)
      echo "Unsupported or unknown package manager: ${pm}"
      exit 1
      ;;
  esac

  printf "Installing packages '%s' using %s..." "${pkg_list}" "${installer}"
  for pkg_name in ${pkg_list}
  do
    ${installer} ${pkg_name}
  done
}

main() {
  pm=$(detect_package_manager)
  echo "Detected package manager: $pm"
  install_packages "$pm"
}

main "$@"

