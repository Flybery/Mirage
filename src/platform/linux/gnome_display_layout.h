/**
 * @file src/platform/linux/gnome_display_layout.h
 * @brief Monitor arrangements for GNOME virtual streaming displays.
 *
 * Mutter publishes a ScreenCast virtual monitor as a new "Meta-N" connector
 * and arranges it with defaults that ignore the existing scale and primary
 * choices. These helpers plan an org.gnome.Mutter.DisplayConfig
 * configuration from the state before the virtual monitor appeared and the
 * state after it did. They perform no I/O so the plans can be unit tested.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::gnome_display_layout {
  struct mode_t {
    std::string id;
    int width = 0;
    int height = 0;
    double preferred_scale = 1.0;
    std::vector<double> supported_scales;
    bool current = false;
  };

  struct monitor_t {
    std::string connector;
    std::vector<mode_t> modes;

    const mode_t *current_mode() const {
      const auto it = std::find_if(modes.begin(), modes.end(), [](const mode_t &mode) {
        return mode.current;
      });
      return it == modes.end() ? nullptr : &*it;
    }
  };

  struct logical_monitor_t {
    int x = 0;
    int y = 0;
    double scale = 1.0;
    std::uint32_t transform = 0;
    bool primary = false;
    std::vector<std::string> connectors;
  };

  struct state_t {
    std::uint32_t serial = 0;
    std::vector<monitor_t> monitors;
    std::vector<logical_monitor_t> logical_monitors;
    // Physical layout mode positions monitors in pixels instead of logical units.
    bool physical_layout = false;

    const monitor_t *monitor(std::string_view connector) const {
      const auto it = std::find_if(monitors.begin(), monitors.end(), [&](const monitor_t &monitor) {
        return monitor.connector == connector;
      });
      return it == monitors.end() ? nullptr : &*it;
    }
  };

  enum class arrangement_e {
    virtual_only,  ///< Only the virtual monitor stays enabled.
    extend,  ///< Restore the previous monitors and place the virtual monitor to their right.
    extend_primary,  ///< As extend, with the virtual monitor primary.
  };

  struct planned_monitor_t {
    std::string connector;
    std::string mode_id;
  };

  struct planned_logical_monitor_t {
    int x = 0;
    int y = 0;
    double scale = 1.0;
    std::uint32_t transform = 0;
    bool primary = false;
    std::vector<planned_monitor_t> monitors;
  };

  /** @brief Mutter names ScreenCast virtual monitors "Meta-<n>". */
  inline bool is_virtual_connector(std::string_view connector) {
    return connector.starts_with("Meta-");
  }

  /**
   * @brief Find the virtual monitor that appeared since @p before.
   * @return Its connector, preferring one already at the requested size.
   */
  inline std::optional<std::string> find_new_virtual(const state_t &before, const state_t &now, int width, int height) {
    std::vector<const monitor_t *> added;
    for (const auto &monitor : now.monitors) {
      if (is_virtual_connector(monitor.connector) && !before.monitor(monitor.connector)) {
        added.push_back(&monitor);
      }
    }
    for (const auto *monitor : added) {
      const auto *mode = monitor->current_mode();
      if (mode && mode->width == width && mode->height == height) {
        return monitor->connector;
      }
    }
    if (added.size() == 1) {
      return added.front()->connector;
    }
    return std::nullopt;
  }

  /** @brief The mode's preferred scale when Mutter supports it, otherwise 1. */
  inline double scale_for(const mode_t &mode) {
    for (const double supported : mode.supported_scales) {
      if (std::abs(supported - mode.preferred_scale) < 0.0001) {
        return supported;
      }
    }
    return 1.0;
  }

  namespace detail {
    inline int layout_width(const mode_t &mode, double scale, std::uint32_t transform, bool physical_layout) {
      // Odd transforms rotate by 90 or 270 degrees.
      const int pixels = transform % 2 == 1 ? mode.height : mode.width;
      return physical_layout ? pixels : static_cast<int>(std::lround(pixels / scale));
    }
  }  // namespace detail

  /**
   * @brief Plan the configuration that places the virtual monitor.
   * @param before State captured before the virtual monitor appeared.
   * @param now Current state that includes the virtual monitor.
   * @return Logical monitors for ApplyMonitorsConfig, or nullopt when the
   *         virtual monitor has no current mode yet.
   */
  inline std::optional<std::vector<planned_logical_monitor_t>> plan(
    const state_t &before,
    const state_t &now,
    const std::string &virtual_connector,
    arrangement_e arrangement
  ) {
    const auto *virtual_monitor = now.monitor(virtual_connector);
    const auto *virtual_mode = virtual_monitor ? virtual_monitor->current_mode() : nullptr;
    if (!virtual_mode) {
      return std::nullopt;
    }
    const double virtual_scale = scale_for(*virtual_mode);
    planned_logical_monitor_t virtual_logical {
      .scale = virtual_scale,
      .primary = true,
      .monitors = {{virtual_connector, virtual_mode->id}},
    };
    if (arrangement == arrangement_e::virtual_only) {
      return std::vector {virtual_logical};
    }

    // Rebuild the previous arrangement, which Mutter's generated default for
    // the new monitor set does not keep (it resets fractional scales).
    std::vector<planned_logical_monitor_t> planned;
    int right_edge = 0;
    int top_edge = 0;
    bool first = true;
    for (const auto &logical : before.logical_monitors) {
      planned_logical_monitor_t entry {
        .x = logical.x,
        .y = logical.y,
        .scale = logical.scale,
        .transform = logical.transform,
        .primary = arrangement == arrangement_e::extend && logical.primary,
      };
      const mode_t *size_mode = nullptr;
      for (const auto &connector : logical.connectors) {
        const auto *monitor = now.monitor(connector);
        const auto *mode = monitor ? monitor->current_mode() : nullptr;
        if (!mode) {
          continue;
        }
        entry.monitors.push_back({connector, mode->id});
        size_mode = size_mode ? size_mode : mode;
      }
      if (entry.monitors.empty()) {
        continue;
      }
      right_edge = std::max(right_edge, logical.x + detail::layout_width(*size_mode, logical.scale, logical.transform, now.physical_layout));
      top_edge = first ? logical.y : std::min(top_edge, logical.y);
      first = false;
      planned.push_back(std::move(entry));
    }
    if (planned.empty()) {
      return std::vector {virtual_logical};
    }
    virtual_logical.x = right_edge;
    virtual_logical.y = top_edge;
    virtual_logical.primary = arrangement == arrangement_e::extend_primary;
    // Mutter rejects a configuration without exactly one primary monitor.
    if (!virtual_logical.primary && std::none_of(planned.begin(), planned.end(), [](const auto &entry) {
          return entry.primary;
        })) {
      planned.front().primary = true;
    }
    planned.push_back(std::move(virtual_logical));
    return planned;
  }
}  // namespace platf::gnome_display_layout
