/**
 * @file src/platform/linux/gnome_display_config.cpp
 * @brief Arrange GNOME virtual streaming displays through Mutter's DisplayConfig.
 */
#include "gnome_display_config.h"

#include "src/logging.h"
#include "src/platform/common.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <thread>
#include <utility>

namespace platf::gnome_display_config {
  namespace layout = gnome_display_layout;

  namespace {
    constexpr const char *bus_name = "org.gnome.Mutter.DisplayConfig";
    constexpr const char *object_path = "/org/gnome/Mutter/DisplayConfig";
    // Applied without storing it in monitors.xml.
    constexpr guint32 method_temporary = 1;
    constexpr guint32 layout_mode_physical = 2;
    constexpr auto poll_interval = std::chrono::milliseconds(250);
    // The first virtual capture may wait for the portal permission dialog.
    constexpr auto appear_timeout = std::chrono::seconds(30);
    constexpr auto vanish_poll_interval = std::chrono::seconds(1);
    constexpr const char *sysfs_backlight_dir = "/sys/class/backlight";

    GDBusConnection *session_bus() {
      g_autoptr(GError) error = nullptr;
      auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
      if (!bus) {
        BOOST_LOG(warning) << "GNOME display config: session bus unavailable: " << (error ? error->message : "unknown error");
      }
      return bus;
    }

    layout::mode_t parse_mode(GVariant *value) {
      const gchar *id = nullptr;
      gint32 width = 0;
      gint32 height = 0;
      gdouble refresh = 0;
      gdouble preferred_scale = 1.0;
      g_autoptr(GVariant) scales = nullptr;
      g_autoptr(GVariant) properties = nullptr;
      g_variant_get(value, "(&siidd@ad@a{sv})", &id, &width, &height, &refresh, &preferred_scale, &scales, &properties);
      layout::mode_t mode {.id = id, .width = width, .height = height, .preferred_scale = preferred_scale};
      gsize count = 0;
      const auto *supported = static_cast<const gdouble *>(g_variant_get_fixed_array(scales, &count, sizeof(gdouble)));
      mode.supported_scales.assign(supported, supported + count);
      gboolean current = FALSE;
      if (g_variant_lookup(properties, "is-current", "b", &current)) {
        mode.current = current;
      }
      return mode;
    }

    bool apply(GDBusConnection *bus, const layout::state_t &state, const std::vector<layout::planned_logical_monitor_t> &planned) {
      GVariantBuilder logical_builder;
      g_variant_builder_init(&logical_builder, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
      for (const auto &logical : planned) {
        GVariantBuilder monitors_builder;
        g_variant_builder_init(&monitors_builder, G_VARIANT_TYPE("a(ssa{sv})"));
        for (const auto &monitor : logical.monitors) {
          g_variant_builder_add(&monitors_builder, "(ss@a{sv})", monitor.connector.c_str(), monitor.mode_id.c_str(), g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0));
        }
        g_variant_builder_add(&logical_builder, "(iidub@a(ssa{sv}))", logical.x, logical.y, logical.scale, logical.transform, logical.primary, g_variant_builder_end(&monitors_builder));
      }
      g_autoptr(GError) error = nullptr;
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        bus,
        bus_name,
        object_path,
        bus_name,
        "ApplyMonitorsConfig",
        g_variant_new("(uu@a(iiduba(ssa{sv}))@a{sv})", state.serial, method_temporary, g_variant_builder_end(&logical_builder), g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error
      );
      if (!reply) {
        BOOST_LOG(warning) << "GNOME display config: ApplyMonitorsConfig failed: " << (error ? error->message : "unknown error");
        return false;
      }
      return true;
    }

    std::optional<layout::state_t> read_state(GDBusConnection *bus) {
      g_autoptr(GError) error = nullptr;
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        bus,
        bus_name,
        object_path,
        bus_name,
        "GetCurrentState",
        nullptr,
        G_VARIANT_TYPE("(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error
      );
      if (!reply) {
        BOOST_LOG(warning) << "GNOME display config: GetCurrentState failed: " << (error ? error->message : "unknown error");
        return std::nullopt;
      }

      layout::state_t state;
      g_autoptr(GVariantIter) monitors = nullptr;
      g_autoptr(GVariantIter) logical_monitors = nullptr;
      g_autoptr(GVariant) properties = nullptr;
      g_variant_get(reply, "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})@a{sv})", &state.serial, &monitors, &logical_monitors, &properties);

      const gchar *connector = nullptr;
      GVariantIter *modes = nullptr;
      while (g_variant_iter_next(monitors, "((&s&s&s&s)a(siiddada{sv})@a{sv})", &connector, nullptr, nullptr, nullptr, &modes, nullptr)) {
        layout::monitor_t monitor {.connector = connector};
        GVariant *mode = nullptr;
        while ((mode = g_variant_iter_next_value(modes))) {
          monitor.modes.push_back(parse_mode(mode));
          g_variant_unref(mode);
        }
        g_variant_iter_free(modes);
        state.monitors.push_back(std::move(monitor));
      }

      gint32 x = 0;
      gint32 y = 0;
      gdouble scale = 1.0;
      guint32 transform = 0;
      gboolean primary = FALSE;
      GVariantIter *assigned = nullptr;
      while (g_variant_iter_next(logical_monitors, "(iiduba(ssss)@a{sv})", &x, &y, &scale, &transform, &primary, &assigned, nullptr)) {
        layout::logical_monitor_t logical {.x = x, .y = y, .scale = scale, .transform = transform, .primary = static_cast<bool>(primary)};
        const gchar *assigned_connector = nullptr;
        while (g_variant_iter_next(assigned, "(&s&s&s&s)", &assigned_connector, nullptr, nullptr, nullptr)) {
          logical.connectors.emplace_back(assigned_connector);
        }
        g_variant_iter_free(assigned);
        state.logical_monitors.push_back(std::move(logical));
      }

      guint32 layout_mode = 0;
      if (g_variant_lookup(properties, "layout-mode", "u", &layout_mode)) {
        state.physical_layout = layout_mode == layout_mode_physical;
      }
      return state;
    }

    /* Backlights ---------------------------------------------------------- */

    // Brightness to restore once the virtual-only stream ends: the sysfs
    // devices written through logind and the values Mutter reports per
    // connector, so its own brightness state matches the panel again.
    struct saved_backlight_t {
      std::vector<std::pair<std::string, int>> devices;
      std::vector<std::pair<std::string, int>> connectors;
    };

    std::filesystem::path backlight_restore_file() {
      return platf::appdata() / "gnome_backlight_restore";
    }

    std::vector<std::pair<std::string, int>> read_sysfs_backlights() {
      std::vector<std::pair<std::string, int>> devices;
      std::error_code error;
      for (const auto &entry : std::filesystem::directory_iterator(sysfs_backlight_dir, error)) {
        std::ifstream input {entry.path() / "brightness"};
        int value = 0;
        if (input >> value) {
          devices.emplace_back(entry.path().filename().string(), value);
        }
      }
      return devices;
    }

    std::optional<guint32> read_mutter_backlights(GDBusConnection *bus, std::vector<std::pair<std::string, int>> *connectors) {
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        bus,
        bus_name,
        object_path,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", bus_name, "Backlight"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        nullptr
      );
      if (!reply) {
        return std::nullopt;
      }
      g_autoptr(GVariant) value = nullptr;
      g_variant_get(reply, "(v)", &value);
      if (!g_variant_is_of_type(value, G_VARIANT_TYPE("(uaa{sv})"))) {
        return std::nullopt;
      }
      guint32 serial = 0;
      g_autoptr(GVariantIter) entries = nullptr;
      g_variant_get(value, "(uaa{sv})", &serial, &entries);
      GVariant *entry = nullptr;
      while (connectors && (entry = g_variant_iter_next_value(entries))) {
        const gchar *connector = nullptr;
        gint32 brightness = 0;
        if (g_variant_lookup(entry, "connector", "&s", &connector) && g_variant_lookup(entry, "value", "i", &brightness)) {
          connectors->emplace_back(connector, brightness);
        }
        g_variant_unref(entry);
      }
      return serial;
    }

    // Unprivileged brightness changes go through the active graphical
    // session; the host service itself runs outside any logind session.
    std::optional<std::string> active_graphical_session(GDBusConnection *system_bus) {
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        system_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", "org.freedesktop.login1.Seat", "ActiveSession"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        nullptr
      );
      if (!reply) {
        return std::nullopt;
      }
      g_autoptr(GVariant) value = nullptr;
      g_variant_get(reply, "(v)", &value);
      if (!g_variant_is_of_type(value, G_VARIANT_TYPE("(so)"))) {
        return std::nullopt;
      }
      const gchar *path = nullptr;
      g_variant_get(value, "(&s&o)", nullptr, &path);
      return std::string {path};
    }

    bool set_device_brightness(GDBusConnection *system_bus, const std::string &session, const std::string &device, int value) {
      g_autoptr(GError) error = nullptr;
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        system_bus,
        "org.freedesktop.login1",
        session.c_str(),
        "org.freedesktop.login1.Session",
        "SetBrightness",
        g_variant_new("(ssu)", "backlight", device.c_str(), static_cast<guint32>(std::max(value, 0))),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error
      );
      if (!reply) {
        BOOST_LOG(warning) << "GNOME display config: setting " << device << " brightness failed: " << (error ? error->message : "unknown error");
      }
      return reply != nullptr;
    }

    void write_restore_file(const saved_backlight_t &saved) {
      std::ofstream output {backlight_restore_file(), std::ios::trunc};
      for (const auto &[device, value] : saved.devices) {
        output << "device " << device << ' ' << value << '\n';
      }
      for (const auto &[connector, value] : saved.connectors) {
        output << "connector " << connector << ' ' << value << '\n';
      }
    }

    std::optional<saved_backlight_t> read_restore_file() {
      std::ifstream input {backlight_restore_file()};
      if (!input) {
        return std::nullopt;
      }
      saved_backlight_t saved;
      std::string kind;
      std::string name;
      int value = 0;
      while (input >> kind >> name >> value) {
        (kind == "device" ? saved.devices : saved.connectors).emplace_back(name, value);
      }
      return saved;
    }

    std::optional<saved_backlight_t> turn_backlights_off(GDBusConnection *bus) {
      g_autoptr(GDBusConnection) system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
      const auto session = system_bus ? active_graphical_session(system_bus) : std::nullopt;
      if (!session) {
        return std::nullopt;
      }
      saved_backlight_t saved {.devices = read_sysfs_backlights()};
      (void) read_mutter_backlights(bus, &saved.connectors);
      if (saved.devices.empty()) {
        return std::nullopt;
      }
      // Persist first so a crash mid-stream still restores at the next start.
      write_restore_file(saved);
      for (const auto &[device, value] : saved.devices) {
        (void) set_device_brightness(system_bus, *session, device, 0);
      }
      BOOST_LOG(info) << "GNOME display config: turned the physical panel backlight off for the virtual-only stream.";
      return saved;
    }

    void restore_backlights(const saved_backlight_t &saved) {
      g_autoptr(GDBusConnection) system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
      const auto session = system_bus ? active_graphical_session(system_bus) : std::nullopt;
      bool restored = session.has_value();
      for (const auto &[device, value] : saved.devices) {
        restored = session && set_device_brightness(system_bus, *session, device, value) && restored;
      }
      // Mutter clamps the off state to its minimum; set its value back too.
      if (g_autoptr(GDBusConnection) bus = session_bus()) {
        if (const auto serial = read_mutter_backlights(bus, nullptr)) {
          for (const auto &[connector, value] : saved.connectors) {
            g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
              bus,
              bus_name,
              object_path,
              bus_name,
              "SetBacklight",
              g_variant_new("(usi)", *serial, connector.c_str(), value),
              nullptr,
              G_DBUS_CALL_FLAGS_NONE,
              -1,
              nullptr,
              nullptr
            );
          }
        }
      }
      if (restored) {
        std::error_code error;
        std::filesystem::remove(backlight_restore_file(), error);
        BOOST_LOG(info) << "GNOME display config: restored the physical panel backlight.";
      }
    }

    // The capture owns the virtual monitor, so its disappearance is the one
    // signal that covers every way a stream can end.
    void restore_when_gone(GDBusConnection *bus, const std::string &connector, const saved_backlight_t &saved) {
      int failed_reads = 0;
      while (failed_reads < 10) {
        std::this_thread::sleep_for(vanish_poll_interval);
        const auto state = read_state(bus);
        if (!state) {
          ++failed_reads;
          continue;
        }
        failed_reads = 0;
        if (!state->monitor(connector)) {
          break;
        }
      }
      restore_backlights(saved);
    }
  }  // namespace

  void restore_pending_backlight() {
    if (const auto saved = read_restore_file()) {
      BOOST_LOG(info) << "GNOME display config: restoring a backlight left off by an earlier stream.";
      restore_backlights(*saved);
    }
  }

  std::optional<layout::state_t> read_state() {
    g_autoptr(GDBusConnection) bus = session_bus();
    return bus ? read_state(bus) : std::nullopt;
  }

  void arrange_new_virtual_monitor_async(
    layout::state_t before,
    int width,
    int height,
    layout::arrangement_e arrangement,
    std::function<bool()> still_wanted
  ) {
    std::thread([before = std::move(before), width, height, arrangement, still_wanted = std::move(still_wanted)] {
      g_autoptr(GDBusConnection) bus = session_bus();
      if (!bus) {
        return;
      }
      const auto deadline = std::chrono::steady_clock::now() + appear_timeout;
      while (still_wanted() && std::chrono::steady_clock::now() < deadline) {
        const auto now = read_state(bus);
        const auto connector = now ? layout::find_new_virtual(before, *now, width, height) : std::nullopt;
        const auto planned = connector ? layout::plan(before, *now, *connector, arrangement) : std::nullopt;
        if (planned) {
          if (apply(bus, *now, *planned)) {
            BOOST_LOG(info) << "GNOME display config: arranged virtual monitor " << *connector << '.';
            if (arrangement == layout::arrangement_e::virtual_only) {
              if (const auto saved = turn_backlights_off(bus)) {
                restore_when_gone(bus, *connector, *saved);
              }
            }
            return;
          }
          // The serial goes stale when Mutter reconfigures concurrently; retry.
        }
        std::this_thread::sleep_for(poll_interval);
      }
      if (still_wanted()) {
        BOOST_LOG(warning) << "GNOME display config: the virtual monitor did not appear; keeping Mutter's arrangement.";
      }
    }).detach();
  }
}  // namespace platf::gnome_display_config
