/**
 * @file src/platform/linux/pipewire_format.h
 * @brief PipeWire video format negotiation shared with focused SPA tests.
 */
#pragma once

#include <cstdint>
#include <limits>
#include <pipewire/version.h>
#include <spa/param/video/format.h>
#include <spa/pod/builder.h>

namespace pipewire {
  // PipeWire before 1.6 (Fedora 43 ships 1.4) fixates an intersected range to
  // the consumer default clamped into the overlap. An unpaced default of 0
  // then becomes the producer minimum, so KWin's 1..119 fps range negotiates
  // a 1 fps maximum. Newer SPA keeps the producer default instead.
#if PW_CHECK_VERSION(1, 6, 0)
  inline constexpr bool clamps_consumer_framerate_default = false;
#else
  inline constexpr bool clamps_consumer_framerate_default = true;
#endif

  inline void add_framerate_parameters(spa_pod_builder *builder, bool negotiate_maxframerate) {
    const auto variable_rate = SPA_FRACTION(0, 1);
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable_rate), 0);
    if (negotiate_maxframerate) {
      // Prefer an unpaced stream, but accept compositors such as KWin whose
      // maximum-rate range starts at 1 fps. A zero-only range has no overlap
      // with those producers and rejects every otherwise compatible format.
      const auto maximum_rate = SPA_FRACTION(std::numeric_limits<int32_t>::max(), 1);
      // Older SPA clamps this default into the overlap: the maximum keeps
      // KWin at its full rate while a 0-only producer still stays unpaced.
      const auto default_rate = clamps_consumer_framerate_default ? maximum_rate : variable_rate;
      spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_maxFramerate,
                          SPA_POD_CHOICE_RANGE_Fraction(&default_rate, &variable_rate, &maximum_rate), 0);
    }
  }
}  // namespace pipewire
