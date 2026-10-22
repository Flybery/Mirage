/**
 * @file src/web_routes.h
 * @brief Redirects from the retired classic web interface to the current one.
 */
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace web_routes {
  /**
   * @brief Where a classic interface address continues in the current interface.
   *
   * Bookmarks, tray links and older clients still open classic page paths
   * (also as ".html" pages). Unknown paths land on the overview.
   */
  inline std::string classic_redirect(std::string_view path) {
    if (path.ends_with(".html")) {
      path.remove_suffix(5);
    }
    while (path.size() > 1 && path.ends_with('/')) {
      path.remove_suffix(1);
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 12> routes {{
      {"/applications", "/v2/library"},
      {"/apps", "/v2/library"},
      {"/settings", "/v2/settings"},
      {"/config", "/v2/settings"},
      {"/logs", "/v2/logs"},
      {"/troubleshooting", "/v2/maintenance"},
      {"/clients", "/v2/devices"},
      {"/pin", "/v2/pair"},
      {"/stats", "/v2/stats"},
      {"/api-tokens", "/v2/api-tokens"},
      {"/webrtc", "/v2/webrtc"},
      {"/sessions", "/v2/sessions"},
    }};
    for (const auto &[classic, current] : routes) {
      if (path == classic) {
        return std::string {current};
      }
    }
    return "/v2/";
  }
}  // namespace web_routes
