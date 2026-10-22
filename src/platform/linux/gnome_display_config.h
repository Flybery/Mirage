/**
 * @file src/platform/linux/gnome_display_config.h
 * @brief Arrange GNOME virtual streaming displays through Mutter's DisplayConfig.
 */
#pragma once

#include "gnome_display_layout.h"

#include <functional>
#include <optional>

namespace platf::gnome_display_config {
  /** @brief Read the current monitor state from org.gnome.Mutter.DisplayConfig. */
  std::optional<gnome_display_layout::state_t> read_state();

  /**
   * @brief Wait for the virtual monitor of a starting capture and arrange it.
   *
   * Runs on a detached worker. @p before is the state captured before the
   * capture started. The configuration is applied as temporary, so Mutter
   * falls back to the stored layout of the remaining monitors as soon as the
   * capture ends and the virtual monitor disappears.
   * Mutter stops drawing to physical monitors left out of the configuration
   * but leaves their panels lit with the last frame, so the virtual-only
   * arrangement also turns the backlights off until the virtual monitor is
   * gone, whichever way the stream ends.
   * @param still_wanted Polled while waiting; false abandons the arrangement.
   */
  void arrange_new_virtual_monitor_async(
    gnome_display_layout::state_t before,
    int width,
    int height,
    gnome_display_layout::arrangement_e arrangement,
    std::function<bool()> still_wanted
  );

  /** @brief Restore backlights left off by a stream that ended with the process. */
  void restore_pending_backlight();
}  // namespace platf::gnome_display_config
