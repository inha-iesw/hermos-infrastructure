#include "hermos/node/hermos_context.hpp"
#include "hermos/node/hermos_only_callback_isolated_executor.hpp"
#include "hermos/node/hermos_only_multi_threaded_executor.hpp"
#include "hermos/node/hermos_only_single_threaded_executor.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

namespace
{

using namespace std::chrono_literals;

void reset_context_for_test()
{
  std::lock_guard<std::mutex> lock(hermos::g_context_mtx);
  hermos::g_context = hermos::Context{};
}

void wait_until_or_fail(
  const std::function<bool()> & predicate, const std::chrono::milliseconds timeout,
  const std::string & failure_message)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return;
    }
    std::this_thread::sleep_for(2ms);
  }
  FAIL() << failure_message;
}

template <typename ExecutorT>
void expect_cancel_stops_spin_without_shutdown(const std::string & executor_name)
{
  hermos::init(0, nullptr);
  auto executor = std::make_shared<ExecutorT>();

  std::atomic_bool spin_exited{false};
  std::thread spin_thread([&]() {
    executor->spin();
    spin_exited.store(true);
  });

  std::this_thread::sleep_for(250ms);
  executor->cancel();

  wait_until_or_fail(
    [&]() { return spin_exited.load(); }, 2s,
    executor_name + " spin() did not return after cancel().");

  EXPECT_TRUE(hermos::ok()) << executor_name
                              << " cancel() should not change hermos::ok() without shutdown().";

  if (spin_thread.joinable()) {
    spin_thread.join();
  }

  hermos::shutdown();
}

// Verifies that a cancel() issued *before* spin() ever starts still makes spin() return.
template <typename ExecutorT>
void expect_spin_returns_when_cancelled_before_spin(const std::string & executor_name)
{
  hermos::init(0, nullptr);
  auto executor = std::make_shared<ExecutorT>();

  // cancel() happens before spin() is ever called.
  executor->cancel();

  std::atomic_bool spin_exited{false};
  std::thread spin_thread([&]() {
    executor->spin();
    spin_exited.store(true);
  });

  wait_until_or_fail(
    [&]() { return spin_exited.load(); }, 2s,
    executor_name + " spin() did not return after a cancel() that preceded it.");

  if (spin_thread.joinable()) {
    spin_thread.join();
  }

  hermos::shutdown();
}

}  // namespace

class InitOkShutdownTest : public ::testing::Test
{
protected:
  void SetUp() override { reset_context_for_test(); }

  void TearDown() override
  {
    hermos::shutdown();
    reset_context_for_test();
  }
};

TEST_F(InitOkShutdownTest, OkIsTrueOnlyBetweenInitAndShutdown)
{
  EXPECT_FALSE(hermos::ok());
  hermos::init(0, nullptr);
  EXPECT_TRUE(hermos::ok());
  hermos::shutdown();
  EXPECT_FALSE(hermos::ok());
}

TEST_F(InitOkShutdownTest, InitShutdownCanBeRepeated)
{
  constexpr int kNrIterations = 5;

  for (int i = 0; i < kNrIterations; ++i) {
    EXPECT_FALSE(hermos::ok()) << "hermos::ok() should be false before init()";
    hermos::init(0, nullptr);
    EXPECT_TRUE(hermos::ok()) << "hermos::ok() should be true after init()";
    hermos::shutdown();
    EXPECT_FALSE(hermos::ok()) << "hermos::ok() should be false after shutdown()";
  }
}

TEST_F(InitOkShutdownTest, ShutdownStopsHermosOnlySingleThreadedExecutorSpin)
{
  hermos::init(0, nullptr);
  auto executor = std::make_shared<hermos::HermosOnlySingleThreadedExecutor>();

  std::atomic_bool spin_exited{false};
  std::thread spin_thread([&]() {
    executor->spin();
    spin_exited.store(true);
  });
  std::this_thread::sleep_for(250ms);
  hermos::shutdown();

  wait_until_or_fail(
    [&]() { return spin_exited.load(); }, 2s,
    "executor spin() did not return after hermos::shutdown().");

  if (spin_thread.joinable()) {
    spin_thread.join();
  }
}

TEST_F(InitOkShutdownTest, ShutdownStopsHermosOnlyMultiThreadedExecutorSpin)
{
  hermos::init(0, nullptr);
  auto executor = std::make_shared<hermos::HermosOnlyMultiThreadedExecutor>();

  std::atomic_bool spin_exited{false};
  std::thread spin_thread([&]() {
    executor->spin();
    spin_exited.store(true);
  });
  std::this_thread::sleep_for(250ms);
  hermos::shutdown();

  wait_until_or_fail(
    [&]() { return spin_exited.load(); }, 2s,
    "executor spin() did not return after hermos::shutdown().");

  if (spin_thread.joinable()) {
    spin_thread.join();
  }
}

TEST_F(InitOkShutdownTest, ShutdownStopsHermosOnlyCallbackIsolatedExecutorSpin)
{
  hermos::init(0, nullptr);
  auto executor = std::make_shared<hermos::HermosOnlyCallbackIsolatedExecutor>();

  std::atomic_bool spin_exited{false};
  std::thread spin_thread([&]() {
    executor->spin();
    spin_exited.store(true);
  });
  std::this_thread::sleep_for(250ms);
  hermos::shutdown();

  wait_until_or_fail(
    [&]() { return spin_exited.load(); }, 2s,
    "executor spin() did not return after hermos::shutdown().");

  if (spin_thread.joinable()) {
    spin_thread.join();
  }
}

TEST_F(InitOkShutdownTest, CancelStopsHermosOnlySingleThreadedExecutorSpinWithoutShutdown)
{
  expect_cancel_stops_spin_without_shutdown<hermos::HermosOnlySingleThreadedExecutor>(
    "HermosOnlySingleThreadedExecutor");
}

TEST_F(InitOkShutdownTest, CancelStopsHermosOnlyMultiThreadedExecutorSpinWithoutShutdown)
{
  expect_cancel_stops_spin_without_shutdown<hermos::HermosOnlyMultiThreadedExecutor>(
    "HermosOnlyMultiThreadedExecutor");
}

TEST_F(InitOkShutdownTest, CancelStopsHermosOnlyCallbackIsolatedExecutorSpinWithoutShutdown)
{
  expect_cancel_stops_spin_without_shutdown<hermos::HermosOnlyCallbackIsolatedExecutor>(
    "HermosOnlyCallbackIsolatedExecutor");
}

TEST_F(InitOkShutdownTest, CancelBeforeSpinStopsHermosOnlySingleThreadedExecutorSpin)
{
  expect_spin_returns_when_cancelled_before_spin<hermos::HermosOnlySingleThreadedExecutor>(
    "HermosOnlySingleThreadedExecutor");
}

TEST_F(InitOkShutdownTest, CancelBeforeSpinStopsHermosOnlyMultiThreadedExecutorSpin)
{
  expect_spin_returns_when_cancelled_before_spin<hermos::HermosOnlyMultiThreadedExecutor>(
    "HermosOnlyMultiThreadedExecutor");
}

TEST_F(InitOkShutdownTest, CancelBeforeSpinStopsHermosOnlyCallbackIsolatedExecutorSpin)
{
  expect_spin_returns_when_cancelled_before_spin<hermos::HermosOnlyCallbackIsolatedExecutor>(
    "HermosOnlyCallbackIsolatedExecutor");
}

TEST_F(InitOkShutdownTest, CustomLoopUsingOkCanExitCleanly)
{
  hermos::init(0, nullptr);

  std::atomic_bool entered_loop{false};
  std::atomic_bool loop_exited{false};
  std::thread loop_thread([&]() {
    while (hermos::ok()) {
      entered_loop.store(true);
      std::this_thread::sleep_for(10ms);
    }
    loop_exited.store(true);
  });

  wait_until_or_fail(
    [&]() { return entered_loop.load(); }, 1s,
    "custom loop did not observe hermos::ok()==true after init().");

  hermos::shutdown();

  wait_until_or_fail(
    [&]() { return loop_exited.load(); }, 2s,
    "custom loop did not exit after hermos::shutdown() changed ok() state.");

  if (loop_thread.joinable()) {
    loop_thread.join();
  }
}

TEST_F(InitOkShutdownTest, SigintAndSigtermStopHermosOnlySingleThreadedExecutorSpin)
{
  auto run_signal_case = [](int signum) {
    hermos::init(0, nullptr);

    auto executor = std::make_shared<hermos::HermosOnlySingleThreadedExecutor>();
    std::atomic_bool spin_exited{false};

    std::thread spin_thread([&]() {
      executor->spin();
      spin_exited.store(true);
    });

    ASSERT_EQ(kill(getpid(), signum), 0);

    const char * signal_name = signum == SIGINT ? "SIGINT" : "SIGTERM";
    wait_until_or_fail(
      [&]() { return spin_exited.load(); }, 2s,
      std::string("executor spin() did not stop after ") + signal_name + ".");

    if (spin_thread.joinable()) {
      spin_thread.join();
    }

    hermos::shutdown();
  };

  run_signal_case(SIGINT);
  run_signal_case(SIGTERM);
}

TEST_F(InitOkShutdownTest, SigintAndSigtermStopHermosOnlyMultiThreadedExecutorSpin)
{
  auto run_signal_case = [](int signum) {
    hermos::init(0, nullptr);

    auto executor = std::make_shared<hermos::HermosOnlyMultiThreadedExecutor>();
    std::atomic_bool spin_exited{false};

    std::thread spin_thread([&]() {
      executor->spin();
      spin_exited.store(true);
    });

    ASSERT_EQ(kill(getpid(), signum), 0);

    const char * signal_name = signum == SIGINT ? "SIGINT" : "SIGTERM";
    wait_until_or_fail(
      [&]() { return spin_exited.load(); }, 2s,
      std::string("executor spin() did not stop after ") + signal_name + ".");

    if (spin_thread.joinable()) {
      spin_thread.join();
    }

    hermos::shutdown();
  };

  run_signal_case(SIGINT);
  run_signal_case(SIGTERM);
}

TEST_F(InitOkShutdownTest, SigintAndSigtermStopHermosOnlyCallbackIsolatedExecutorSpin)
{
  auto run_signal_case = [](int signum) {
    hermos::init(0, nullptr);

    auto executor = std::make_shared<hermos::HermosOnlyCallbackIsolatedExecutor>();
    std::atomic_bool spin_exited{false};

    std::thread spin_thread([&]() {
      executor->spin();
      spin_exited.store(true);
    });

    ASSERT_EQ(kill(getpid(), signum), 0);

    const char * signal_name = signum == SIGINT ? "SIGINT" : "SIGTERM";
    wait_until_or_fail(
      [&]() { return spin_exited.load(); }, 2s,
      std::string("executor spin() did not stop after ") + signal_name + ".");

    if (spin_thread.joinable()) {
      spin_thread.join();
    }

    hermos::shutdown();
  };

  run_signal_case(SIGINT);
  run_signal_case(SIGTERM);
}

TEST_F(InitOkShutdownTest, InitAndShutdownAreIdempotent)
{
  // Calling shutdown() before init() should do nothing and not cause any errors.
  hermos::shutdown();
  EXPECT_FALSE(hermos::ok())
    << "hermos::ok() should still be false after shutdown() without init()";

  // Calling init() multiple times should not cause any errors and should keep the context
  // initialized.
  hermos::init(0, nullptr);
  EXPECT_TRUE(hermos::ok()) << "hermos::ok() should be true after first init()";
  hermos::init(0, nullptr);
  EXPECT_TRUE(hermos::ok()) << "hermos::ok() should still be true after second init()";

  // Calling shutdown() multiple times should not cause any errors and should keep the context
  // shutdown.
  hermos::shutdown();
  EXPECT_FALSE(hermos::ok()) << "hermos::ok() should be false after first shutdown()";
  hermos::shutdown();
  EXPECT_FALSE(hermos::ok()) << "hermos::ok() should still be false after second shutdown()";
}
