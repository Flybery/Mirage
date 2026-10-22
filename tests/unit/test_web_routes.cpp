#include "../tests_common.h"
#include <src/web_routes.h>

TEST(WebRoutes, ClassicPagesContinueInTheCurrentInterface) {
  EXPECT_EQ(web_routes::classic_redirect("/"), "/v2/");
  EXPECT_EQ(web_routes::classic_redirect("/applications"), "/v2/library");
  EXPECT_EQ(web_routes::classic_redirect("/apps.html"), "/v2/library");
  EXPECT_EQ(web_routes::classic_redirect("/settings/"), "/v2/settings");
  EXPECT_EQ(web_routes::classic_redirect("/config.html"), "/v2/settings");
  EXPECT_EQ(web_routes::classic_redirect("/troubleshooting"), "/v2/maintenance");
  EXPECT_EQ(web_routes::classic_redirect("/pin"), "/v2/pair");
  EXPECT_EQ(web_routes::classic_redirect("/clients"), "/v2/devices");
  EXPECT_EQ(web_routes::classic_redirect("/api-tokens"), "/v2/api-tokens");
}

TEST(WebRoutes, UnknownClassicPagesLandOnTheOverview) {
  EXPECT_EQ(web_routes::classic_redirect("/welcome"), "/v2/");
  EXPECT_EQ(web_routes::classic_redirect("/login.html"), "/v2/");
  EXPECT_EQ(web_routes::classic_redirect("/changelog"), "/v2/");
  EXPECT_EQ(web_routes::classic_redirect("/applicationsx"), "/v2/");
}
