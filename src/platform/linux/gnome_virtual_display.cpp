/**
 * @file src/platform/linux/gnome_virtual_display.cpp
 * @brief Registry of Remote Monitor clients served by GNOME virtual monitors.
 */
#include "gnome_virtual_display.h"

#include <map>
#include <mutex>

namespace platf::gnome_virtual_display {
  namespace {
    std::mutex registry_mutex;
    std::map<std::string, target_t> registry;
  }  // namespace

  void register_client(const std::string &client_uuid, int width, int height, int refresh_hz) {
    if (!detail::valid_client_uuid(client_uuid)) {
      return;
    }
    std::lock_guard lock {registry_mutex};
    registry[client_uuid] = target_t {client_uuid, width, height, refresh_hz};
  }

  void remove_client(const std::string &client_uuid) {
    std::lock_guard lock {registry_mutex};
    registry.erase(client_uuid);
  }

  std::vector<std::string> targets() {
    std::lock_guard lock {registry_mutex};
    std::vector<std::string> names;
    names.reserve(registry.size());
    for (const auto &[uuid, target] : registry) {
      names.emplace_back(target_name(uuid, target.width, target.height, target.refresh_hz));
    }
    return names;
  }
}  // namespace platf::gnome_virtual_display
