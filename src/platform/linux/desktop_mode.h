/**
 * @file src/platform/linux/desktop_mode.h
 * @brief Detection of the per-user GNOME desktop mode.
 */
#pragma once

#include <cstdlib>
#include <string_view>

namespace platf::linux_desktop {
  /**
   * @brief Decide whether this host streams an existing GNOME desktop.
   *
   * The GNOME user unit sets VIBEPOLLO_DESKTOP_MODE=gnome. Without that
   * explicit choice, a launch inside a GNOME session is detected from the
   * colon-separated XDG_CURRENT_DESKTOP list. Any other explicit mode, and
   * the KDE machine host, keep the managed-display behavior.
   */
  inline bool gnome_mode_from_environment(const char *machine_host, const char *desktop_mode, const char *current_desktop) noexcept {
    if (machine_host && std::string_view {machine_host} == "1") {
      return false;
    }
    if (desktop_mode && desktop_mode[0] != '\0') {
      return std::string_view {desktop_mode} == "gnome";
    }
    if (!current_desktop) {
      return false;
    }
    std::string_view desktops {current_desktop};
    while (!desktops.empty()) {
      const auto separator = desktops.find(':');
      if (desktops.substr(0, separator) == "GNOME") {
        return true;
      }
      if (separator == std::string_view::npos) {
        break;
      }
      desktops.remove_prefix(separator + 1);
    }
    return false;
  }

  /** @brief The GNOME mode of this process, fixed at first use. */
  inline bool gnome_mode() noexcept {
    static const bool enabled = gnome_mode_from_environment(
      std::getenv("VIBEPOLLO_MACHINE_HOST"),
      std::getenv("VIBEPOLLO_DESKTOP_MODE"),
      std::getenv("XDG_CURRENT_DESKTOP")
    );
    return enabled;
  }
}  // namespace platf::linux_desktop
