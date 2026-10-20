/**
 * @file src/platform/linux/portalgrab.cpp
 * @brief Definitions for XDG portal grab.
 */
// local includes
#include "gnome_virtual_display.h"
#include "pipewire.cpp"

namespace {
  // Portal configuration constants
  constexpr uint32_t SOURCE_TYPE_MONITOR = 1;
  constexpr uint32_t SOURCE_TYPE_VIRTUAL = 4;
  constexpr uint32_t CURSOR_MODE_EMBEDDED = 2;

  constexpr uint32_t PERSIST_FORGET = 0;
  constexpr uint32_t PERSIST_WHILE_RUNNING = 1;
  constexpr uint32_t PERSIST_UNTIL_REVOKED = 2;

  constexpr uint32_t TYPE_KEYBOARD = 1;
  constexpr uint32_t TYPE_POINTER = 2;
  constexpr uint32_t TYPE_TOUCHSCREEN = 4;

  // Portal D-Bus interface names and paths
  constexpr const char *PORTAL_NAME = "org.freedesktop.portal.Desktop";
  constexpr const char *PORTAL_PATH = "/org/freedesktop/portal/desktop";
  constexpr const char *REMOTE_DESKTOP_IFACE = "org.freedesktop.portal.RemoteDesktop";
  constexpr const char *SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast";
  constexpr const char *REQUEST_IFACE = "org.freedesktop.portal.Request";

  constexpr const char REQUEST_PREFIX[] = "/org/freedesktop/portal/desktop/request/";
  constexpr const char SESSION_PREFIX[] = "/org/freedesktop/portal/desktop/session/";
}  // namespace

using namespace std::literals;

namespace portal {
  // Forward declarations
  class runtime_t;

  /** A persisted portal restore token. Monitor and virtual-monitor sessions
   *  keep separate tokens so neither selection invalidates the other. */
  class restore_token_t {
  public:
    explicit restore_token_t(std::string_view file_name):
        file_name_ {file_name} {
    }

    /** Token for sessions that capture existing monitors. */
    static restore_token_t &monitor() {
      static restore_token_t token {"portal_token"};
      return token;
    }

    /** Token for sessions that create a GNOME virtual monitor. */
    static restore_token_t &virtual_monitor() {
      static restore_token_t token {"portal_token_virtual"};
      return token;
    }

    std::string get() const {
      std::lock_guard lock {mutex_};
      return token_;
    }

    void set(std::string_view value) {
      std::lock_guard lock {mutex_};
      token_ = value;
    }

    bool empty() const {
      std::lock_guard lock {mutex_};
      return token_.empty();
    }

    void load() {
      std::ifstream file(get_file_path());
      if (file.is_open()) {
        std::string value;
        std::getline(file, value);
        set(value);
        if (!value.empty()) {
          BOOST_LOG(info) << "[portalgrab] Loaded portal restore token from disk ("sv << file_name_ << ")"sv;
        }
      }
    }

    void save() {
      const auto value = get();
      if (value.empty()) {
        return;
      }
      std::ofstream file(get_file_path());
      if (file.is_open()) {
        file << value;
        BOOST_LOG(info) << "[portalgrab] Saved portal restore token to disk ("sv << file_name_ << ")"sv;
      } else {
        BOOST_LOG(warning) << "[portalgrab] Failed to save portal restore token"sv;
      }
    }

  private:
    std::string get_file_path() const {
      return platf::appdata().string() + "/" + file_name_;
    }

    std::string file_name_;
    mutable std::mutex mutex_;
    std::string token_;
  };

  struct dbus_response_t {
    GMainLoop *loop;
    GVariant *response;
    guint subscription_id;
  };

  struct pipewire_streaminfo_t {
    int pipewire_node = -1;
    int width = 0;
    int height = 0;
    int pos_x = 0;
    int pos_y = 0;
    std::string monitor_name;

    std::string to_display_name() {
      if (!monitor_name.empty()) {
        return monitor_name;
      }
      return std::format("position-{}x{}-resolution-{}x{}", pos_x, pos_y, width, height);
    }

    bool match_display_name(const std::string_view &display_name) {
      // Check the given non-empty display name matches the display name for this struct
      return !display_name.empty() && display_name == to_display_name();
    }
  };

  class dbus_t {
  public:
    dbus_t &operator=(dbus_t &&) = delete;  // Do not allow to copying

    ~dbus_t() noexcept {
      try {
        if (conn && !session_handle.empty()) {
          g_autoptr(GError) err = nullptr;
          // This is a blocking C call; it won't throw, but we wrap for safety
          g_dbus_connection_call_sync(
            conn,
            "org.freedesktop.portal.Desktop",
            session_handle.c_str(),
            "org.freedesktop.portal.Session",
            "Close",
            nullptr,
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &err
          );

          if (err) {
            BOOST_LOG(warning) << "[portalgrab] Failed to explicitly close portal session: "sv << err->message;
          } else {
            BOOST_LOG(debug) << "[portalgrab] Explicitly closed portal session: "sv << session_handle;
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "[portalgrab] Standard exception caught in ~dbus_t: "sv << e.what();
      } catch (...) {
        BOOST_LOG(error) << "[portalgrab] Unknown exception caught in ~dbus_t"sv;
      }

      if (pipewire_fd >= 0) {
        close(pipewire_fd);
      }
      if (screencast_proxy) {
        g_clear_object(&screencast_proxy);
      }
      if (remote_desktop_proxy) {
        g_clear_object(&remote_desktop_proxy);
      }
      if (conn) {
        g_clear_object(&conn);
      }
    }

    int init() {
      token().load();

      conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
      if (!conn) {
        return -1;
      }
      remote_desktop_proxy = g_dbus_proxy_new_sync(conn, G_DBUS_PROXY_FLAGS_NONE, nullptr, PORTAL_NAME, PORTAL_PATH, REMOTE_DESKTOP_IFACE, nullptr, nullptr);
      if (!remote_desktop_proxy) {
        return -1;
      }
      screencast_proxy = g_dbus_proxy_new_sync(conn, G_DBUS_PROXY_FLAGS_NONE, nullptr, PORTAL_NAME, PORTAL_PATH, SCREENCAST_IFACE, nullptr, nullptr);
      if (!screencast_proxy) {
        return -1;
      }

      return 0;
    }

    int connect_to_portal() {
      g_autoptr(GMainLoop) loop = g_main_loop_new(nullptr, FALSE);
      g_autofree gchar *session_path = nullptr;
      g_autofree gchar *session_token = nullptr;
      create_session_path(conn, nullptr, &session_token);

      // Try combined RemoteDesktop + ScreenCast session first. A virtual
      // monitor needs no portal input devices (input goes through uinput).
      bool use_screencast_only = virtual_source || !try_remote_desktop_session(loop, &session_path, session_token);

      // Fall back to ScreenCast-only if RemoteDesktop failed
      if (use_screencast_only && try_screencast_only_session(loop, &session_path) < 0) {
        return -1;
      }

      if (start_portal_session(loop, session_path, pipewire_streams, use_screencast_only) < 0) {
        return -1;
      }

      if (open_pipewire_remote(session_path, pipewire_fd) < 0) {
        return -1;
      }

      return 0;
    }

    // Try to create a combined RemoteDesktop + ScreenCast session
    // Returns true on success, false if should fall back to ScreenCast-only
    bool try_remote_desktop_session(GMainLoop *loop, gchar **session_path, const gchar *session_token) {
      if (create_portal_session(loop, session_path, session_token, false) < 0) {
        return false;
      }

      if (select_remote_desktop_devices(loop, *session_path) < 0) {
        BOOST_LOG(warning) << "[portalgrab] RemoteDesktop.SelectDevices failed, falling back to ScreenCast-only mode"sv;
        g_free(*session_path);
        *session_path = nullptr;
        return false;
      }

      if (select_screencast_sources(loop, *session_path, false) < 0) {
        BOOST_LOG(warning) << "[portalgrab] ScreenCast.SelectSources failed with RemoteDesktop session, trying ScreenCast-only mode"sv;
        g_free(*session_path);
        *session_path = nullptr;
        return false;
      }

      return true;
    }

    // Create a ScreenCast-only session
    int try_screencast_only_session(GMainLoop *loop, gchar **session_path) {
      g_autofree gchar *new_session_token = nullptr;
      create_session_path(conn, nullptr, &new_session_token);
      if (create_portal_session(loop, session_path, new_session_token, true) < 0) {
        return -1;
      }
      if (select_screencast_sources(loop, *session_path, true) < 0) {
        g_free(*session_path);
        *session_path = nullptr;
        return -1;
      }
      return 0;
    }

    bool is_session_closed() const {
      if (conn && !session_handle.empty()) {
        // Try to retrieve property org.freedesktop.portal.Session::version
        g_autoptr(GError) err = nullptr;
        g_dbus_connection_call_sync(
          conn,
          "org.freedesktop.portal.Desktop",
          session_handle.c_str(),
          "org.freedesktop.DBus.Properties",
          "Get",
          g_variant_new("(ss)", "org.freedesktop.portal.Session", "version"),
          G_VARIANT_TYPE("(v)"),
          G_DBUS_CALL_FLAGS_NONE,
          -1,
          nullptr,
          &err
        );
        // If we cannot get the property then the session portal was closed.
        if (err) {
          BOOST_LOG(debug) << "[portalgrab] Session closed as check failed: "sv << err->message;
          return true;
        }
      }
      // The session is not closed (or might not have been opened yet).
      return false;
    }

    std::vector<pipewire_streaminfo_t> pipewire_streams;
    int pipewire_fd = -1;
    /** Request a new GNOME virtual monitor instead of existing monitors. */
    bool virtual_source = false;

  private:
    GDBusConnection *conn = nullptr;
    GDBusProxy *screencast_proxy = nullptr;
    GDBusProxy *remote_desktop_proxy = nullptr;
    std::string session_handle;

    restore_token_t &token() const {
      return virtual_source ? restore_token_t::virtual_monitor() : restore_token_t::monitor();
    }

    int create_portal_session(GMainLoop *loop, gchar **session_path_out, const gchar *session_token, bool use_screencast) {
      GDBusProxy *proxy = use_screencast ? screencast_proxy : remote_desktop_proxy;
      const char *session_type = use_screencast ? "ScreenCast" : "RemoteDesktop";

      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(a{sv})"));
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(session_token));
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(proxy, "CreateSession", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);

      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not create "sv << session_type << " session: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) create_response = dbus_response_wait(&response);

      if (!create_response) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " CreateSession: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_autoptr(GVariant) results = nullptr;
      g_variant_get(create_response, "(u@a{sv})", &response_code, &results);

      BOOST_LOG(debug) << "[portalgrab] " << session_type << " CreateSession response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " CreateSession failed with response code: "sv << response_code;
        return -1;
      }

      g_autoptr(GVariant) session_handle_v = g_variant_lookup_value(results, "session_handle", nullptr);
      if (!session_handle_v) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " CreateSession: session_handle not found in response"sv;
        return -1;
      }

      if (g_variant_is_of_type(session_handle_v, G_VARIANT_TYPE_VARIANT)) {
        g_autoptr(GVariant) inner = g_variant_get_variant(session_handle_v);
        *session_path_out = g_strdup(g_variant_get_string(inner, nullptr));
      } else {
        *session_path_out = g_strdup(g_variant_get_string(session_handle_v, nullptr));
      }

      BOOST_LOG(debug) << "[portalgrab] " << session_type << " CreateSession: got session handle: "sv << *session_path_out;
      // Save it for the destructor to use during cleanup
      this->session_handle = *session_path_out;
      return 0;
    }

    int select_remote_desktop_devices(GMainLoop *loop, const gchar *session_path) {
      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(oa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(TYPE_KEYBOARD | TYPE_POINTER | TYPE_TOUCHSCREEN));
      g_variant_builder_add(&builder, "{sv}", "persist_mode", g_variant_new_uint32(PERSIST_UNTIL_REVOKED));
      if (!token().empty()) {
        g_variant_builder_add(&builder, "{sv}", "restore_token", g_variant_new_string(token().get().c_str()));
      }
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(remote_desktop_proxy, "SelectDevices", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);

      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not select devices: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) devices_response = dbus_response_wait(&response);

      if (!devices_response) {
        BOOST_LOG(error) << "[portalgrab] SelectDevices: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_variant_get(devices_response, "(u@a{sv})", &response_code, nullptr);
      BOOST_LOG(debug) << "[portalgrab] SelectDevices response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] SelectDevices failed with response code: "sv << response_code;
        return -1;
      }

      return 0;
    }

    int select_screencast_sources(GMainLoop *loop, const gchar *session_path, bool persist) {
      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(oa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(virtual_source ? SOURCE_TYPE_VIRTUAL : SOURCE_TYPE_MONITOR));
      g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(CURSOR_MODE_EMBEDDED));
      // One virtual monitor per session; Mutter sizes it from the stream.
      g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(virtual_source ? FALSE : TRUE));
      if (persist) {
        g_variant_builder_add(&builder, "{sv}", "persist_mode", g_variant_new_uint32(PERSIST_UNTIL_REVOKED));
        if (!token().empty()) {
          g_variant_builder_add(&builder, "{sv}", "restore_token", g_variant_new_string(token().get().c_str()));
        }
      }
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(screencast_proxy, "SelectSources", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not select sources: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) sources_response = dbus_response_wait(&response);

      if (!sources_response) {
        BOOST_LOG(error) << "[portalgrab] SelectSources: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_variant_get(sources_response, "(u@a{sv})", &response_code, nullptr);
      BOOST_LOG(debug) << "[portalgrab] SelectSources response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] SelectSources failed with response code: "sv << response_code;
        return -1;
      }

      return 0;
    }

    int start_portal_session(GMainLoop *loop, const gchar *session_path, std::vector<pipewire_streaminfo_t> &out_pipewire_streams, bool use_screencast) {
      GDBusProxy *proxy = use_screencast ? screencast_proxy : remote_desktop_proxy;
      const char *session_type = use_screencast ? "ScreenCast" : "RemoteDesktop";

      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(osa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_add(&builder, "s", "");  // parent_window
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(proxy, "Start", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not start "sv << session_type << " session: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) start_response = dbus_response_wait(&response);

      if (!start_response) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " Start: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_autoptr(GVariant) dict = nullptr;
      g_autoptr(GVariant) streams = nullptr;
      g_variant_get(start_response, "(u@a{sv})", &response_code, &dict);

      BOOST_LOG(debug) << "[portalgrab] " << session_type << " Start response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " Start failed with response code: "sv << response_code;
        return -1;
      }

      streams = g_variant_lookup_value(dict, "streams", G_VARIANT_TYPE("a(ua{sv})"));
      if (!streams) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " Start: no streams in response"sv;
        return -1;
      }

      if (const gchar *new_token = nullptr; g_variant_lookup(dict, "restore_token", "s", &new_token) && new_token && new_token[0] != '\0' && token().get() != new_token) {
        token().set(new_token);
        token().save();
      }

      GVariantIter iter;
      const auto wl_monitors = virtual_source ? decltype(wl::monitors()) {} : wl::monitors();
      int out_pipewire_node;
      g_autoptr(GVariant) value = nullptr;
      g_variant_iter_init(&iter, streams);
      while (g_variant_iter_next(&iter, "(u@a{sv})", &out_pipewire_node, &value)) {
        if (virtual_source) {
          // A virtual monitor has no size until the consumer negotiates one.
          out_pipewire_streams.emplace_back(pipewire_streaminfo_t {out_pipewire_node, 0, 0, 0, 0});
          g_clear_pointer(&value, g_variant_unref);
          continue;
        }
        int out_width;
        int out_height;
        bool result = g_variant_lookup(value, "size", "(ii)", &out_width, &out_height, nullptr);
        if (!result) {
          BOOST_LOG(warning) << "[portalgrab] Ignoring stream without proper resolution on pipewire node "sv << out_pipewire_node;
          continue;
        }

        int out_pos_x;
        int out_pos_y;
        result = g_variant_lookup(value, "position", "(ii)", &out_pos_x, &out_pos_y, nullptr);
        if (!result) {
          BOOST_LOG(warning) << "[portalgrab] Falling back to position 0x0 for stream with resolution "sv << out_width << "x"sv << out_height << "on pipewire node "sv << out_pipewire_node;
          out_pos_x = 0;
          out_pos_y = 0;
        }
        auto stream = pipewire_streaminfo_t {out_pipewire_node, out_width, out_height, out_pos_x, out_pos_y};

        // Try to match the stream to a monitor_name by position/resolution and update stream info
        for (const auto &monitor : wl_monitors) {
          if (monitor->viewport.offset_x == out_pos_x && monitor->viewport.offset_y == out_pos_y && monitor->viewport.logical_width == out_width && monitor->viewport.logical_height == out_height) {
            stream.monitor_name = monitor->name;
            break;
          }
        }

        out_pipewire_streams.emplace_back(stream);
      }

      // The portal call returns the streams sorted by out_pipewire_node which can shuffle displays around, so
      // we have to sort pipewire streams by position here to be consistent
      std::ranges::sort(out_pipewire_streams, [](const auto &a, const auto &b) {
        return a.pos_x < b.pos_x || a.pos_y < b.pos_y;
      });

      return 0;
    }

    int open_pipewire_remote(const gchar *session_path, int &fd) {
      g_autoptr(GUnixFDList) fd_list = nullptr;
      g_autoptr(GVariant) msg = g_variant_ref_sink(g_variant_new("(oa{sv})", session_path, nullptr));

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_with_unix_fd_list_sync(screencast_proxy, "OpenPipeWireRemote", msg, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fd_list, nullptr, &err);
      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not open pipewire remote: "sv << err->message;
        return -1;
      }

      int fd_handle;
      g_variant_get(reply, "(h)", &fd_handle);
      fd = g_unix_fd_list_get(fd_list, fd_handle, nullptr);
      return 0;
    }

    static void on_response_received_cb([[maybe_unused]] GDBusConnection *connection, [[maybe_unused]] const gchar *sender_name, [[maybe_unused]] const gchar *object_path, [[maybe_unused]] const gchar *interface_name, [[maybe_unused]] const gchar *signal_name, GVariant *parameters, gpointer user_data) {
      auto *response = static_cast<dbus_response_t *>(user_data);
      response->response = g_variant_ref_sink(parameters);
      g_main_loop_quit(response->loop);
    }

    static gchar *get_sender_string(GDBusConnection *conn) {
      gchar *sender = g_strdup(g_dbus_connection_get_unique_name(conn) + 1);
      gchar *dot;
      while ((dot = strstr(sender, ".")) != nullptr) {
        *dot = '_';
      }
      return sender;
    }

    static void create_request_path(GDBusConnection *conn, gchar **out_path, gchar **out_token) {
      static uint32_t request_count = 0;

      request_count++;

      if (out_token) {
        *out_token = g_strdup_printf("Sunshine%u", request_count);
      }
      if (out_path) {
        g_autofree gchar *sender = get_sender_string(conn);
        *out_path = g_strdup(std::format("{}{}{}{}", REQUEST_PREFIX, sender, "/Sunshine", request_count).c_str());
      }
    }

    static void create_session_path(GDBusConnection *conn, gchar **out_path, gchar **out_token) {
      static uint32_t session_count = 0;

      session_count++;

      if (out_token) {
        *out_token = g_strdup_printf("Sunshine%u", session_count);
      }

      if (out_path) {
        g_autofree gchar *sender = get_sender_string(conn);
        *out_path = g_strdup(std::format("{}{}{}{}", SESSION_PREFIX, sender, "/Sunshine", session_count).c_str());
      }
    }

    static void dbus_response_init(struct dbus_response_t *response, GMainLoop *loop, GDBusConnection *conn, const char *request_path) {
      response->loop = loop;
      response->subscription_id = g_dbus_connection_signal_subscribe(conn, PORTAL_NAME, REQUEST_IFACE, "Response", request_path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_response_received_cb, response, nullptr);
    }

    static GVariant *dbus_response_wait(struct dbus_response_t *response) {
      g_main_loop_run(response->loop);
      return response->response;
    }
  };

  class portal_t: public pipewire::pipewire_display_t {
  public:
    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_object_serial [[maybe_unused]]) override {
      // A GNOME Remote Monitor target asks Mutter for a new virtual monitor
      // sized from the stream we negotiate; it exists while this stream runs.
      const auto virtual_target = platf::gnome_virtual_display::parse_target(display_name);
      dbus.virtual_source = virtual_target.has_value();

      // Connect DBus portal session
      if (dbus.init() < 0) {
        BOOST_LOG(error) << "[portalgrab] Failed to connect to dbus. portal_t setup failed.";
        return -1;
      }
      if (dbus.connect_to_portal() < 0) {
        BOOST_LOG(error) << "[portalgrab] Failed to connect to portal. portal_t setup failed.";
        return -1;
      }

      // Match display_name to a stream from the pipewire_streams vector
      bool use_fallback = true;
      pipewire_streaminfo_t stream;
      auto streams = dbus.pipewire_streams;
      if (streams.empty()) {
        BOOST_LOG(error) << "[portalgrab] No streams found on portal. portal_t setup failed.";
        return -1;
      }
      if (virtual_target) {
        pipewire.set_negotiate_maxframerate(negotiate_maxframerate.load());
        pipewire.set_exact_size(true);
        out_pipewire_fd = dbus.pipewire_fd;
        out_pipewire_node = streams.front().pipewire_node;
        this->offset_x = 0;
        this->offset_y = 0;
        this->width = virtual_target->width;
        this->height = virtual_target->height;
        this->logical_width = virtual_target->width;
        this->logical_height = virtual_target->height;
        BOOST_LOG(info) << "[portalgrab] Requesting a GNOME virtual monitor of "sv << width << "x"sv << height
                        << " for client "sv << virtual_target->client_uuid;
        return 0;
      }
      for (auto &stream_ : streams) {
        if (stream_.match_display_name(display_name)) {
          stream = stream_;
          use_fallback = false;
          break;
        }
      }
      // Fall back to first stream if we cannot match the given display_name to a stream in currently available streams.
      if (use_fallback) {
        BOOST_LOG(info) << "[portalgrab] Using first available stream as no matching stream was found for: '"sv << display_name << "'";
        stream = dbus.pipewire_streams.at(0);
      }

      // Restore global maxframerate negotiation state
      pipewire.set_negotiate_maxframerate(negotiate_maxframerate.load());

      // Return values for pipewire init
      out_pipewire_fd = dbus.pipewire_fd;
      out_pipewire_node = stream.pipewire_node;
      // Set/update basic stream parameters on display_t
      this->offset_x = stream.pos_x;
      this->offset_y = stream.pos_y;
      this->width = stream.width;
      this->height = stream.height;
      this->logical_width = 0;  // Explicitly mark for pipewire_display_t to try to figure this out.
      this->logical_height = 0;  // Explicitly Mark for pipewire_display_t to try to figure this out.
      // Flag successful setup
      return 0;
    }

    bool check_stream_dead(platf::capture_e &out_status) override {
      // If the pipewire stream stopped due to closed portal session stop the capture with an error
      if (dbus.is_session_closed()) {
        BOOST_LOG(warning) << "[portalgrab] PipeWire stream stopped by closed portal session."sv;
        pipewire.frame_cv().notify_all();
        out_status = platf::capture_e::error;
        return true;  // Stop capture with error (due to out_status)
      }
      // Disable maxframerate negotiation if the stream died without having ever started (e.g. GNOME mutter does not support it)
      if (shared_state->previous_state != PW_STREAM_STATE_STREAMING && negotiate_maxframerate.load()) {
        BOOST_LOG(warning) << "[portalgrab] Negotiation failed, will retry without maxFramerate"sv;
        negotiate_maxframerate.store(false);
        pipewire.set_negotiate_maxframerate(false);
        out_status = platf::capture_e::reinit;
        return true;  // Stop capture with reinit (due to out_status)
      }
      return false;  // Return to default stream dead handling
    }

    // DBus portal connection
    dbus_t dbus;

    // Class variable to store runtime state of maxFramerate negotiation
    static inline std::atomic<bool> negotiate_maxframerate {true};
  };
}  // namespace portal

namespace platf {
  std::shared_ptr<display_t> portal_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    using enum platf::mem_type_e;
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[portalgrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    // Drop CAP_SYS_ADMIN, CAP_SYS_NICE and set DUMPABLE flag to allow XDG /root access
    if (has_elevated_privileges(true)) {
      if (!drop_elevated_privileges(true)) {
        BOOST_LOG(error) << "[portalgrab] Refusing portal capture after the permanent capability drop failed."sv;
        return nullptr;
      }
    }

    auto portal = std::make_shared<portal::portal_t>();
    if (portal->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return portal;
  }

  std::vector<std::string> portal_display_names() {
    std::vector<std::string> display_names;
    auto dbus = std::make_shared<portal::dbus_t>();

    if (dbus->init() < 0) {
      BOOST_LOG(warning) << "[portalgrab] Failed to connect to dbus. Cannot enumerate displays, returning empty list.";
      return {};
    }

    if (has_elevated_privileges(true)) {
      // We're still in the probing phase of Sunshine startup. Dropping portal security early will break KMS.
      // Just return a dummy screen for now. Display re-enumeration after encoder probing will yield full result.
      display_names.emplace_back("init");
      return display_names;
    }

    if (dbus->connect_to_portal() < 0) {
      BOOST_LOG(warning) << "[portalgrab] Failed to connect to portal. Cannot enumerate displays, returning empty list.";
      return {};
    }

    for (auto stream_ : dbus->pipewire_streams) {
      BOOST_LOG(info) << "[portalgrab] Found stream for display id/name: '"sv << stream_.monitor_name << "' position: "sv << stream_.pos_x << "x"sv << stream_.pos_y << " resolution: "sv << stream_.width << "x"sv << stream_.height;
      display_names.emplace_back(stream_.to_display_name());
    }
    // Release the portal session as soon as possible to properly release related resources early.
    dbus.reset();

    // Return currently active display names
    return display_names;
  }
}  // namespace platf
