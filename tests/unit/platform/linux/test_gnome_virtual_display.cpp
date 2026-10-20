#include "../../../tests_common.h"
#include <src/platform/linux/gnome_virtual_display.h>

namespace gvd = platf::gnome_virtual_display;

TEST(GnomeVirtualDisplay, TargetNameRoundTrips) {
  const auto name = gvd::target_name("7C355BC6-7146-E306-B71E-05435DF665DC", 2266, 1488, 60);
  EXPECT_EQ(name, "gnome-virtual:7C355BC6-7146-E306-B71E-05435DF665DC:2266x1488@60");
  const auto target = gvd::parse_target(name);
  ASSERT_TRUE(target);
  EXPECT_EQ(target->client_uuid, "7C355BC6-7146-E306-B71E-05435DF665DC");
  EXPECT_EQ(target->width, 2266);
  EXPECT_EQ(target->height, 1488);
  EXPECT_EQ(target->refresh_hz, 60);
}

TEST(GnomeVirtualDisplay, RejectsOtherAndMalformedNames) {
  for (const char *name : {
         "eDP-1",
         "gnome-virtual:",
         "gnome-virtual:abc",
         "gnome-virtual::1920x1080@60",
         "gnome-virtual:abc:1920x1080",
         "gnome-virtual:abc:1920@60",
         "gnome-virtual:abc:0x1080@60",
         "gnome-virtual:abc:1920x1080@0",
         "gnome-virtual:abc:20000x1080@60",
         "gnome-virtual:abc:1920x1080@60x",
         "gnome-virtual:a/b:1920x1080@60",
         "gnome-virtual:a b:1920x1080@60",
       }) {
    EXPECT_FALSE(gvd::parse_target(name)) << name;
  }
}

TEST(GnomeVirtualDisplay, RegistryPublishesCurrentModes) {
  gvd::register_client("client-a", 1920, 1080, 60);
  gvd::register_client("client-b", 2266, 1488, 120);
  gvd::register_client("client-a", 2560, 1440, 60);  // mode change replaces
  gvd::register_client("bad/uuid", 1920, 1080, 60);  // ignored
  const auto targets = gvd::targets();
  ASSERT_EQ(targets.size(), 2u);
  EXPECT_EQ(targets[0], "gnome-virtual:client-a:2560x1440@60");
  EXPECT_EQ(targets[1], "gnome-virtual:client-b:2266x1488@120");
  gvd::remove_client("client-a");
  gvd::remove_client("client-b");
  EXPECT_TRUE(gvd::targets().empty());
}
