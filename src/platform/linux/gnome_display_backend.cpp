/**
 * @file src/platform/linux/gnome_display_backend.cpp
 * @brief Display backend for the per-user GNOME mode.
 *
 * Streams capture the existing desktop through the ScreenCast portal, or a
 * ScreenCast virtual monitor at the client's mode when a virtual display is
 * requested. Mutter creates that monitor for the capture stream and removes
 * it when the capture ends, so there is no connector pool to lease or restore.
 */
#include "display_backend.h"
#include "gnome_display_config.h"
#include "gnome_virtual_display.h"
#include "src/logging.h"
#include "src/rtsp.h"

#include <atomic>
#include <mutex>
#include <set>

namespace platf::linux_display {
  namespace {
    namespace layout = gnome_display_layout;

    // Clients whose normal stream owns a virtual monitor; Remote Monitor
    // clients are registered separately by the Remote Monitor runtime.
    std::mutex streaming_clients_mutex;
    std::set<std::string> streaming_clients;
    // Bumped on revert so a pending arrangement of an ended stream is dropped.
    std::atomic<std::uint64_t> arrangement_generation {0};

    std::string virtual_display_key(const rtsp_stream::launch_session_t &session, bool shared) {
      if (shared) {
        return "shared";
      }
      const auto &uuid = session.normal_vdd_owner_uuid.empty() ? session.client_uuid : session.normal_vdd_owner_uuid;
      return gnome_virtual_display::detail::valid_client_uuid(uuid) ? uuid : "client";
    }

    layout::arrangement_e arrangement_for(config::video_t::virtual_display_layout_e configured) {
      using enum config::video_t::virtual_display_layout_e;
      switch (configured) {
        case exclusive:
          return layout::arrangement_e::virtual_only;
        case extended_primary:
        case extended_primary_isolated:
          return layout::arrangement_e::extend_primary;
        case extended:
        case extended_isolated:
          break;
      }
      return layout::arrangement_e::extend;
    }

    void reset_virtual_display_state(rtsp_stream::launch_session_t &session) {
      session.virtual_display = false;
      session.virtual_display_failed = false;
      session.virtual_display_device_id.clear();
      session.virtual_display_ready_since.reset();
      session.virtual_display_hdr_enabled.reset();
      session.virtual_display_recreated_on_demand = false;
      session.virtual_display_needs_resume_apply = false;
    }

    class gnome_backend_t final : public backend_t {
    public:
      prepared_display_t prepare_session(rtsp_stream::launch_session_t &session, bool, bool) const override {
        const auto mode = session.virtual_display_mode_override.value_or(config::video.virtual_display_mode);
        const bool config_requests_virtual = mode != config::video_t::virtual_display_mode_e::disabled;
        const bool client_requests_virtual = session.client_virtual_display_override.value_or(session.client_requests_virtual_display);
        const bool explicit_physical = session.client_virtual_display_override && !*session.client_virtual_display_override;
        const bool app_requests_virtual = session.virtual_display;
        bool requested = app_requests_virtual || client_requests_virtual || (config_requests_virtual && !explicit_physical);
        if (session.output_name_override && !session.output_name_override->empty() && !app_requests_virtual && !client_requests_virtual) {
          requested = false;
        }

        reset_virtual_display_state(session);
        if (!requested) {
          return {false, {}, {}};
        }

        const bool shared = mode == config::video_t::virtual_display_mode_e::shared;
        const auto key = virtual_display_key(session, shared);
        const auto target = gnome_virtual_display::target_name(key, session.width, session.height, session.fps);
        gnome_virtual_display::register_client(key, session.width, session.height, session.fps);
        {
          std::lock_guard lock {streaming_clients_mutex};
          streaming_clients.insert(key);
        }

        // Capture the arrangement before the capture creates the monitor.
        if (auto before = gnome_display_config::read_state()) {
          const auto generation = ++arrangement_generation;
          gnome_display_config::arrange_new_virtual_monitor_async(
            std::move(*before),
            session.width,
            session.height,
            arrangement_for(config::video.virtual_display_layout),
            [generation] {
              return arrangement_generation.load() == generation;
            }
          );
        }

        BOOST_LOG(info) << "GNOME desktop mode: streaming a virtual monitor at "
                        << session.width << 'x' << session.height << '@' << session.fps << '.';
        session.virtual_display = true;
        session.virtual_display_device_id = target;
        session.virtual_display_hdr_enabled = false;
        return {false, target, {}};
      }

      capabilities_t capabilities() const override {
        // ScreenCast virtual monitors are SDR.
        return {"gnome-portal", true, true, false};
      }

      std::optional<display_device::EnumeratedDeviceList> enumerate_devices(display_device::DeviceEnumerationDetail) const override {
        // Listing portal outputs opens a ScreenCast session; the existing
        // desktop is selected through the portal's own consent instead.
        return display_device::EnumeratedDeviceList {};
      }

      std::string capture_target(const std::string &requested_output) const override {
        return requested_output;
      }

      bool revert() const override {
        // The capture owns the virtual monitor and Mutter restores the
        // remaining monitors' stored layout when it disappears.
        ++arrangement_generation;
        std::lock_guard lock {streaming_clients_mutex};
        for (const auto &key : streaming_clients) {
          gnome_virtual_display::remove_client(key);
        }
        streaming_clients.clear();
        return true;
      }

      void schedule_revert(std::chrono::milliseconds, std::string) const override {
        (void) revert();
      }
    };
  }  // namespace

  const backend_t &gnome_backend() {
    static const gnome_backend_t gnome;
    return gnome;
  }
}  // namespace platf::linux_display
