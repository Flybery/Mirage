#include "../../../tests_common.h"
#include <src/platform/linux/desktop_mode.h>

using platf::linux_desktop::gnome_mode_from_environment;

TEST(LinuxDesktopMode, ExplicitGnomeModeFromUserUnit) {
  EXPECT_TRUE(gnome_mode_from_environment(nullptr, "gnome", nullptr));
  EXPECT_TRUE(gnome_mode_from_environment(nullptr, "gnome", "KDE"));
}

TEST(LinuxDesktopMode, DetectsGnomeSessions) {
  EXPECT_TRUE(gnome_mode_from_environment(nullptr, nullptr, "GNOME"));
  EXPECT_TRUE(gnome_mode_from_environment(nullptr, "", "ubuntu:GNOME"));
  EXPECT_TRUE(gnome_mode_from_environment(nullptr, nullptr, "GNOME-Classic:GNOME"));
}

TEST(LinuxDesktopMode, KeepsManagedBehaviorElsewhere) {
  EXPECT_FALSE(gnome_mode_from_environment(nullptr, nullptr, nullptr));
  EXPECT_FALSE(gnome_mode_from_environment(nullptr, nullptr, "KDE"));
  EXPECT_FALSE(gnome_mode_from_environment(nullptr, nullptr, "GNOME-Classic"));
  EXPECT_FALSE(gnome_mode_from_environment(nullptr, nullptr, "XGNOME:GNOMEX"));
  EXPECT_FALSE(gnome_mode_from_environment(nullptr, "kde", "GNOME"));
}

TEST(LinuxDesktopMode, MachineHostNeverUsesGnomeMode) {
  EXPECT_FALSE(gnome_mode_from_environment("1", "gnome", "GNOME"));
  EXPECT_TRUE(gnome_mode_from_environment("0", nullptr, "GNOME"));
}
