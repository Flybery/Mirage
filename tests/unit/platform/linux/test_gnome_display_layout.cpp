#include "../../../tests_common.h"
#include <src/platform/linux/gnome_display_layout.h>

namespace gdl = platf::gnome_display_layout;

namespace {
  gdl::monitor_t monitor(std::string connector, std::string mode_id, int width, int height, double preferred_scale, std::vector<double> scales) {
    return {std::move(connector), {{std::move(mode_id), width, height, preferred_scale, std::move(scales), true}}};
  }

  // A 2880x1800 laptop panel at 125%, before any virtual monitor exists.
  gdl::state_t laptop_only() {
    gdl::state_t state;
    state.serial = 7;
    state.monitors.push_back(monitor("eDP-1", "2880x1800@120", 2880, 1800, 2.0, {1.0, 1.25, 1.5, 2.0}));
    state.logical_monitors.push_back({0, 0, 1.25, 0, true, {"eDP-1"}});
    return state;
  }

  // Mutter after creating a virtual monitor, with its own default layout.
  gdl::state_t with_virtual(const char *connector = "Meta-0") {
    auto state = laptop_only();
    state.serial = 8;
    state.monitors.push_back(monitor(connector, "2266x1488@60", 2266, 1488, 1.0, {1.0, 2.0}));
    state.logical_monitors = {{0, 0, 1.0, 0, true, {"eDP-1"}}, {2880, 0, 1.0, 0, false, {connector}}};
    return state;
  }
}  // namespace

TEST(GnomeDisplayLayout, RecognizesMutterVirtualConnectors) {
  EXPECT_TRUE(gdl::is_virtual_connector("Meta-0"));
  EXPECT_TRUE(gdl::is_virtual_connector("Meta-12"));
  EXPECT_FALSE(gdl::is_virtual_connector("eDP-1"));
  EXPECT_FALSE(gdl::is_virtual_connector("HDMI-A-1"));
}

TEST(GnomeDisplayLayout, FindsOnlyTheNewlyAddedVirtualMonitor) {
  const auto before = laptop_only();
  EXPECT_EQ(gdl::find_new_virtual(before, before, 2266, 1488), std::nullopt);
  EXPECT_EQ(gdl::find_new_virtual(before, with_virtual(), 2266, 1488), "Meta-0");

  // A Remote Monitor that already existed is not the stream's monitor.
  auto remote_monitor_before = with_virtual("Meta-0");
  auto both = remote_monitor_before;
  both.monitors.push_back(monitor("Meta-1", "1920x1080@60", 1920, 1080, 1.0, {1.0}));
  EXPECT_EQ(gdl::find_new_virtual(remote_monitor_before, both, 1920, 1080), "Meta-1");
}

TEST(GnomeDisplayLayout, PrefersTheRequestedSizeAmongSeveralNewMonitors) {
  auto now = with_virtual("Meta-0");
  now.monitors.push_back(monitor("Meta-1", "1920x1080@60", 1920, 1080, 1.0, {1.0}));
  EXPECT_EQ(gdl::find_new_virtual(laptop_only(), now, 1920, 1080), "Meta-1");
  EXPECT_EQ(gdl::find_new_virtual(laptop_only(), now, 1280, 720), std::nullopt);
}

TEST(GnomeDisplayLayout, VirtualOnlyKeepsJustTheVirtualMonitorAsPrimary) {
  const auto planned = gdl::plan(laptop_only(), with_virtual(), "Meta-0", gdl::arrangement_e::virtual_only);
  ASSERT_TRUE(planned);
  ASSERT_EQ(planned->size(), 1u);
  EXPECT_TRUE(planned->front().primary);
  EXPECT_EQ(planned->front().x, 0);
  EXPECT_DOUBLE_EQ(planned->front().scale, 1.0);
  ASSERT_EQ(planned->front().monitors.size(), 1u);
  EXPECT_EQ(planned->front().monitors.front().connector, "Meta-0");
  EXPECT_EQ(planned->front().monitors.front().mode_id, "2266x1488@60");
}

TEST(GnomeDisplayLayout, ExtendRestoresThePreviousScaleAndPlacesTheVirtualMonitorRight) {
  const auto planned = gdl::plan(laptop_only(), with_virtual(), "Meta-0", gdl::arrangement_e::extend);
  ASSERT_TRUE(planned);
  ASSERT_EQ(planned->size(), 2u);
  const auto &laptop = planned->at(0);
  EXPECT_DOUBLE_EQ(laptop.scale, 1.25);
  EXPECT_TRUE(laptop.primary);
  EXPECT_EQ(laptop.monitors.front().mode_id, "2880x1800@120");
  const auto &stream = planned->at(1);
  // 2880 px at 125% is 2304 logical pixels wide.
  EXPECT_EQ(stream.x, 2304);
  EXPECT_EQ(stream.y, 0);
  EXPECT_FALSE(stream.primary);
}

TEST(GnomeDisplayLayout, ExtendPrimaryMovesPrimaryToTheVirtualMonitor) {
  const auto planned = gdl::plan(laptop_only(), with_virtual(), "Meta-0", gdl::arrangement_e::extend_primary);
  ASSERT_TRUE(planned);
  ASSERT_EQ(planned->size(), 2u);
  EXPECT_FALSE(planned->at(0).primary);
  EXPECT_TRUE(planned->at(1).primary);
}

TEST(GnomeDisplayLayout, PhysicalLayoutModePositionsInPixels) {
  auto now = with_virtual();
  now.physical_layout = true;
  const auto planned = gdl::plan(laptop_only(), now, "Meta-0", gdl::arrangement_e::extend);
  ASSERT_TRUE(planned);
  EXPECT_EQ(planned->at(1).x, 2880);
}

TEST(GnomeDisplayLayout, RotatedMonitorsUseTheirRotatedWidth) {
  auto before = laptop_only();
  before.logical_monitors.front().transform = 1;
  before.logical_monitors.front().scale = 1.0;
  const auto planned = gdl::plan(before, with_virtual(), "Meta-0", gdl::arrangement_e::extend);
  ASSERT_TRUE(planned);
  EXPECT_EQ(planned->at(1).x, 1800);
}

TEST(GnomeDisplayLayout, UnsupportedPreferredScaleFallsBackToOne) {
  gdl::mode_t mode {"m", 1920, 1080, 1.5, {1.0, 2.0}, true};
  EXPECT_DOUBLE_EQ(gdl::scale_for(mode), 1.0);
  mode.preferred_scale = 2.0;
  EXPECT_DOUBLE_EQ(gdl::scale_for(mode), 2.0);
}

TEST(GnomeDisplayLayout, ExtendKeepsExactlyOnePrimaryWhenNoneWasPrimary) {
  auto before = laptop_only();
  before.logical_monitors.front().primary = false;
  const auto planned = gdl::plan(before, with_virtual(), "Meta-0", gdl::arrangement_e::extend);
  ASSERT_TRUE(planned);
  EXPECT_TRUE(planned->at(0).primary);
  EXPECT_FALSE(planned->at(1).primary);
}

TEST(GnomeDisplayLayout, WaitsWhileTheVirtualMonitorHasNoCurrentMode) {
  auto now = with_virtual();
  now.monitors.back().modes.front().current = false;
  EXPECT_EQ(gdl::plan(laptop_only(), now, "Meta-0", gdl::arrangement_e::virtual_only), std::nullopt);
}

TEST(GnomeDisplayLayout, FallsBackToVirtualOnlyWithoutAPreviousLayout) {
  const auto planned = gdl::plan(gdl::state_t {}, with_virtual(), "Meta-0", gdl::arrangement_e::extend);
  ASSERT_TRUE(planned);
  ASSERT_EQ(planned->size(), 1u);
  EXPECT_TRUE(planned->front().primary);
}
