/**
 * @file src/platform/linux/gnome_display_backend.cpp
 * @brief Existing-desktop display backend for the per-user GNOME mode.
 */
#include "display_backend.h"
#include "src/logging.h"
#include "src/rtsp.h"

namespace platf::linux_display {
  namespace {
    class gnome_backend_t final : public backend_t {
    public:
      prepared_display_t prepare_session(rtsp_stream::launch_session_t &session, bool, bool) const override {
        // Mutter owns the monitor layout and the managed KScreen/VKMS pool is
        // not available to a user session. Stream the existing desktop through
        // the ScreenCast portal instead of rejecting a virtual-display request.
        if (session.virtual_display) {
          BOOST_LOG(info) << "GNOME desktop mode: virtual displays are not available yet; streaming the existing desktop.";
        }
        session.virtual_display = false;
        session.virtual_display_failed = false;
        session.virtual_display_device_id.clear();
        session.virtual_display_ready_since.reset();
        session.virtual_display_hdr_enabled.reset();
        session.virtual_display_recreated_on_demand = false;
        session.virtual_display_needs_resume_apply = false;
        session.virtual_display_mode_override = config::video_t::virtual_display_mode_e::disabled;
        return {false, {}, {}};
      }

      capabilities_t capabilities() const override { return {"gnome-portal"}; }

      std::optional<display_device::EnumeratedDeviceList> enumerate_devices(display_device::DeviceEnumerationDetail) const override {
        // Listing portal outputs opens a ScreenCast session; the existing
        // desktop is selected through the portal's own consent instead.
        return display_device::EnumeratedDeviceList {};
      }

      std::string capture_target(const std::string &requested_output) const override {
        return requested_output;
      }
    };
  }  // namespace

  const backend_t &gnome_backend() {
    static const gnome_backend_t gnome;
    return gnome;
  }
}  // namespace platf::linux_display
