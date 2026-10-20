#include "../tests_common.h"
#include <src/thread_safe.h>

namespace {
  struct counted_t {
    int value = 0;
  };

  int constructions = 0;
  int destructions = 0;

  auto make_counted() {
    return safe::make_shared<counted_t>(
      [](counted_t &object) {
        ++constructions;
        object.value = 42;
        return 0;
      },
      [](counted_t &) {
        ++destructions;
      }
    );
  }
}  // namespace

TEST(ThreadSafeShared, RefIfActiveNeverConstructs) {
  constructions = destructions = 0;
  auto shared = make_counted();
  EXPECT_FALSE(shared.ref_if_active());
  EXPECT_EQ(constructions, 0);
  EXPECT_EQ(destructions, 0);
}

TEST(ThreadSafeShared, RefIfActiveSharesTheRunningObject) {
  constructions = destructions = 0;
  auto shared = make_counted();
  {
    auto owner = shared.ref();
    ASSERT_TRUE(owner);
    {
      auto observer = shared.ref_if_active();
      ASSERT_TRUE(observer);
      EXPECT_EQ(observer->value, 42);
    }
    EXPECT_EQ(destructions, 0);
  }
  EXPECT_EQ(constructions, 1);
  EXPECT_EQ(destructions, 1);
  EXPECT_FALSE(shared.ref_if_active());
}
