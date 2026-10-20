/**
 * @file src/platform/linux/gnome_virtual_display.h
 * @brief Remote Monitor targets backed by GNOME ScreenCast virtual monitors.
 *
 * In the GNOME desktop mode, a Remote Monitor client is served by a virtual
 * monitor that Mutter creates for a ScreenCast portal stream with the VIRTUAL
 * source type. Mutter sizes that monitor from the negotiated stream size, and
 * removes it when the stream ends. The capture target name carries the client
 * identity and the requested mode, so capture can negotiate exactly that size.
 */
#pragma once

#include <charconv>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::gnome_virtual_display {
  inline constexpr std::string_view target_prefix = "gnome-virtual:";

  struct target_t {
    std::string client_uuid;
    int width = 0;
    int height = 0;
    int refresh_hz = 0;
  };

  namespace detail {
    inline bool valid_client_uuid(std::string_view uuid) {
      if (uuid.empty() || uuid.size() > 64) {
        return false;
      }
      for (const char c : uuid) {
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!alnum && c != '-') {
          return false;
        }
      }
      return true;
    }

    inline std::optional<int> parse_bounded(std::string_view text, int minimum, int maximum) {
      int value = 0;
      const auto *end = text.data() + text.size();
      const auto [ptr, ec] = std::from_chars(text.data(), end, value);
      if (text.empty() || ec != std::errc {} || ptr != end || value < minimum || value > maximum) {
        return std::nullopt;
      }
      return value;
    }
  }  // namespace detail

  /** @brief Name of the capture target for a client's virtual monitor. */
  inline std::string target_name(std::string_view client_uuid, int width, int height, int refresh_hz) {
    return std::format("{}{}:{}x{}@{}", target_prefix, client_uuid, width, height, refresh_hz);
  }

  /** @brief Parse a capture target name; rejects anything not produced by target_name(). */
  inline std::optional<target_t> parse_target(std::string_view name) {
    if (!name.starts_with(target_prefix)) {
      return std::nullopt;
    }
    name.remove_prefix(target_prefix.size());
    const auto mode_separator = name.rfind(':');
    if (mode_separator == std::string_view::npos) {
      return std::nullopt;
    }
    const auto uuid = name.substr(0, mode_separator);
    const auto mode = name.substr(mode_separator + 1);
    const auto x = mode.find('x');
    const auto at = mode.find('@');
    if (!detail::valid_client_uuid(uuid) || x == std::string_view::npos || at == std::string_view::npos || at < x) {
      return std::nullopt;
    }
    const auto width = detail::parse_bounded(mode.substr(0, x), 1, 16384);
    const auto height = detail::parse_bounded(mode.substr(x + 1, at - x - 1), 1, 16384);
    const auto refresh = detail::parse_bounded(mode.substr(at + 1), 1, 1000);
    if (!width || !height || !refresh) {
      return std::nullopt;
    }
    return target_t {std::string {uuid}, *width, *height, *refresh};
  }

  /** @brief Remember the requested mode of a client's virtual monitor. */
  void register_client(const std::string &client_uuid, int width, int height, int refresh_hz);
  /** @brief Forget a client's virtual monitor; its capture stream owns removal. */
  void remove_client(const std::string &client_uuid);
  /** @brief Capture targets of all registered virtual monitors. */
  std::vector<std::string> targets();
}  // namespace platf::gnome_virtual_display
