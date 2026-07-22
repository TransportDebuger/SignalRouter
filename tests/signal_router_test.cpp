/**
@file signal_router_test.cpp
@brief Unit-тесты для stc::signals::SignalRouter.
@version 1.0.0
@date 2026-07-20
*/
#include "stc/signals/signal_router.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

#include "stc/signals/system_calls.hpp"

using namespace stc::signals;
using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

// --- Mock системных вызовов ---

class MockSystemCalls : public ISystemCalls {
 public:
  MOCK_METHOD(int, Sigprocmask,
              (int how, const sigset_t* set, sigset_t* oldset), (override));
  MOCK_METHOD(int, Signalfd, (int fd, const sigset_t* mask, int flags),
              (override));
  MOCK_METHOD(int, EpollCreate1, (int flags), (override));
  MOCK_METHOD(int, EpollCtl,
              (int epfd, int op, int fd, struct epoll_event* event),
              (override));
  MOCK_METHOD(int, EpollWait,
              (int epfd, struct epoll_event* events, int maxevents,
               int timeout),
              (override));
  MOCK_METHOD(ssize_t, Read, (int fd, void* buf, size_t count), (override));
  MOCK_METHOD(int, Close, (int fd), (override));
};

// --- Тестовый фикстур ---

class SignalRouterTest : public ::testing::Test {
 protected:
  std::unique_ptr<NiceMock<MockSystemCalls>> mock_sys_;

  void SetUp() override {
    mock_sys_ = std::make_unique<NiceMock<MockSystemCalls>>();

    ON_CALL(*mock_sys_, Sigprocmask(_, _, _)).WillByDefault(Return(0));
    ON_CALL(*mock_sys_, Signalfd(_, _, _)).WillByDefault(Return(3));
    ON_CALL(*mock_sys_, EpollCreate1(_)).WillByDefault(Return(4));
    ON_CALL(*mock_sys_, EpollCtl(_, _, _, _)).WillByDefault(Return(0));
    ON_CALL(*mock_sys_, EpollWait(_, _, _, _)).WillByDefault(Return(0));
    ON_CALL(*mock_sys_, Close(_)).WillByDefault(Return(0));
  }
};

// =====================================================================
// 1. Покрытие API: Конструктор, Деструктор, Глобальный синглтон
// =====================================================================

TEST_F(SignalRouterTest, Constructor_Success) {
  EXPECT_NO_THROW({ SignalRouter router(std::move(mock_sys_)); });
}

TEST_F(SignalRouterTest, Constructor_FailsOnSigprocmask) {
  EXPECT_CALL(*mock_sys_, Sigprocmask(0, nullptr, _)).WillOnce(Return(-1));
  EXPECT_THROW(SignalRouter(std::move(mock_sys_)), std::system_error);
}

TEST_F(SignalRouterTest, Constructor_FailsOnSignalfd) {
  EXPECT_CALL(*mock_sys_, Signalfd(-1, _, _)).WillOnce(Return(-1));
  EXPECT_THROW(SignalRouter(std::move(mock_sys_)), std::system_error);
}

TEST_F(SignalRouterTest, Global_ReturnsSameInstance) {
  SignalRouter& ref1 = SignalRouter::Global();
  SignalRouter& ref2 = SignalRouter::Global();
  EXPECT_EQ(&ref1, &ref2);
}

// =====================================================================
// 2. Покрытие API: RegisterHandler, UnregisterHandler
// =====================================================================

TEST_F(SignalRouterTest, RegisterHandler_InvalidSignals) {
  SignalRouter router(std::move(mock_sys_));

  EXPECT_THROW(router.RegisterHandler(SIGKILL, [](int) {}),
               std::invalid_argument);
  EXPECT_THROW(router.RegisterHandler(SIGSTOP, [](int) {}),
               std::invalid_argument);
  EXPECT_THROW(router.RegisterHandler(0, [](int) {}), std::invalid_argument);
  EXPECT_THROW(router.RegisterHandler(-1, [](int) {}), std::invalid_argument);
  EXPECT_THROW(router.RegisterHandler(NSIG, [](int) {}), std::invalid_argument);
}

TEST_F(SignalRouterTest, RegisterHandler_UpdatesMask) {
  MockSystemCalls* mock_ptr = mock_sys_.get();
  SignalRouter router(std::move(mock_sys_));

  // Ожидаем вызов Sigprocmask с флагом SIG_BLOCK (0) при регистрации
  EXPECT_CALL(*mock_ptr, Sigprocmask(0, _, nullptr)).WillOnce(Return(0));
  EXPECT_CALL(*mock_ptr, Signalfd(3, _, _)).WillOnce(Return(0));

  // РАЗРЕШАЕМ вызов Sigprocmask(2, ...) из деструктора (SIG_SETMASK == 2)
  EXPECT_CALL(*mock_ptr, Sigprocmask(2, _, nullptr)).WillRepeatedly(Return(0));

  EXPECT_NO_THROW(router.RegisterHandler(SIGUSR1, [](int) {}));
}

TEST_F(SignalRouterTest, RegisterHandler_FailsOnMaskUpdate) {
  MockSystemCalls* mock_ptr = mock_sys_.get();
  SignalRouter router(std::move(mock_sys_));

  EXPECT_CALL(*mock_ptr, Sigprocmask(0, _, nullptr)).WillOnce(Return(-1));

  // РАЗРЕШАЕМ вызов Sigprocmask(2, ...) из деструктора
  EXPECT_CALL(*mock_ptr, Sigprocmask(2, _, nullptr)).WillRepeatedly(Return(0));

  EXPECT_THROW(router.RegisterHandler(SIGUSR1, [](int) {}), std::system_error);
}

TEST_F(SignalRouterTest, UnregisterHandler_RemovesHandlers) {
  SignalRouter router(std::move(mock_sys_));
  router.RegisterHandler(SIGUSR1, [](int) {});

  EXPECT_NO_THROW(router.UnregisterHandler(SIGUSR1));
  EXPECT_THROW(router.UnregisterHandler(SIGKILL), std::invalid_argument);
}

// =====================================================================
// 3. Покрытие API: Start, Stop, IsRunning
// =====================================================================

TEST_F(SignalRouterTest, Start_Stop_IsRunning) {
  MockSystemCalls* mock_ptr = mock_sys_.get();
  SignalRouter router(std::move(mock_sys_));

  // РАЗРЕШАЕМ фоновому потоку вызывать EpollWait без провала теста
  EXPECT_CALL(*mock_ptr, EpollWait(_, _, _, _)).WillRepeatedly(Return(0));
  // РАЗРЕШАЕМ деструктору восстанавливать маску
  EXPECT_CALL(*mock_ptr, Sigprocmask(2, _, nullptr)).WillRepeatedly(Return(0));

  EXPECT_FALSE(router.IsRunning());

  router.Start();
  EXPECT_TRUE(router.IsRunning());

  EXPECT_THROW(router.Start(), std::runtime_error);

  router.Stop();
  EXPECT_FALSE(router.IsRunning());

  EXPECT_NO_THROW(router.Stop());
}

// =====================================================================
// 4. Покрытие строк: WorkerLoop, Dispatch, EINTR, Deadlock Prevention
// =====================================================================

TEST_F(SignalRouterTest, WorkerLoop_DispatchesSignal) {
  MockSystemCalls* mock_ptr = mock_sys_.get();
  SignalRouter router(std::move(mock_sys_));
  std::atomic<bool> handler_called{false};

  router.RegisterHandler(SIGUSR1, [&](int sig) {
    EXPECT_EQ(sig, SIGUSR1);
    handler_called = true;
  });

  EXPECT_CALL(*mock_ptr, EpollWait(_, _, _, _))
      .WillRepeatedly(Invoke([&](int, struct epoll_event* events, int, int) {
        static bool first_call = true;
        if (first_call) {
          first_call = false;
          events[0].events = EPOLLIN;
          events[0].data.fd = 3;
          return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 0;
      }));

  struct signalfd_siginfo mock_fdsi = {};
  mock_fdsi.ssi_signo = SIGUSR1;

  EXPECT_CALL(*mock_ptr, Read(3, _, sizeof(struct signalfd_siginfo)))
      .WillOnce(Invoke([&](int, void* buf, size_t) {
        std::memcpy(buf, &mock_fdsi, sizeof(mock_fdsi));
        return sizeof(mock_fdsi);
      }));

  router.Start();

  auto start = std::chrono::steady_clock::now();
  while (!handler_called &&
         (std::chrono::steady_clock::now() - start) < std::chrono::seconds(1)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  router.Stop();
  EXPECT_TRUE(handler_called);
}

TEST_F(SignalRouterTest, WorkerLoop_HandlesEINTR) {
  MockSystemCalls* mock_ptr = mock_sys_.get();
  SignalRouter router(std::move(mock_sys_));

  EXPECT_CALL(*mock_ptr, EpollWait(_, _, _, _))
      .WillOnce(Invoke([](int, struct epoll_event*, int, int) {
        errno = EINTR;
        return -1;
      }))
      .WillRepeatedly(Invoke([](int, struct epoll_event*, int, int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 0;
      }));

  router.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  router.Stop();
  EXPECT_FALSE(router.IsRunning());
}

TEST_F(SignalRouterTest, WorkerLoop_IgnoresPartialRead) {
  MockSystemCalls* mock_ptr = mock_sys_.get();
  SignalRouter router(std::move(mock_sys_));
  std::atomic<bool> handler_called{false};

  router.RegisterHandler(SIGUSR1, [&](int) { handler_called = true; });

  EXPECT_CALL(*mock_ptr, EpollWait(_, _, _, _))
      .WillOnce(Invoke([](int, struct epoll_event* events, int, int) {
        events[0].data.fd = 3;
        return 1;
      }))
      .WillRepeatedly(Invoke([](int, struct epoll_event*, int, int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 0;
      }));

  EXPECT_CALL(*mock_ptr, Read(3, _, _)).WillOnce(Return(1));

  router.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  router.Stop();

  EXPECT_FALSE(handler_called);
}

TEST_F(SignalRouterTest, Dispatch_PreventsDeadlock) {
  MockSystemCalls* mock_ptr = mock_sys_.get();
  SignalRouter router(std::move(mock_sys_));
  std::atomic<bool> handler_finished{false};

  router.RegisterHandler(SIGUSR1, [&](int) {
    router.RegisterHandler(SIGUSR2, [](int) {});
    handler_finished = true;
  });

  EXPECT_CALL(*mock_ptr, EpollWait(_, _, _, _))
      .WillOnce(Invoke([](int, struct epoll_event* events, int, int) {
        events[0].data.fd = 3;
        return 1;
      }))
      .WillRepeatedly(Invoke([](int, struct epoll_event*, int, int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 0;
      }));

  struct signalfd_siginfo mock_fdsi = {};
  mock_fdsi.ssi_signo = SIGUSR1;
  EXPECT_CALL(*mock_ptr, Read(3, _, _))
      .WillOnce(Invoke([&](int, void* buf, size_t) {
        std::memcpy(buf, &mock_fdsi, sizeof(mock_fdsi));
        return sizeof(mock_fdsi);
      }));

  router.Start();

  auto start = std::chrono::steady_clock::now();
  while (!handler_finished &&
         (std::chrono::steady_clock::now() - start) < std::chrono::seconds(1)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  router.Stop();
  EXPECT_TRUE(handler_finished);
}

// =====================================================================
// 5. Покрытие строк: RAII и гарантии деструктора
// =====================================================================

TEST_F(SignalRouterTest, Destructor_RestoresMaskAndClosesFd) {
  sigset_t original_mask;
  sigemptyset(&original_mask);
  sigaddset(&original_mask, SIGPIPE);

  EXPECT_CALL(*mock_sys_, Sigprocmask(0, nullptr, _))
      .WillOnce(Invoke([&](int, const sigset_t*, sigset_t* oldset) {
        *oldset = original_mask;
        return 0;
      }));

  EXPECT_CALL(*mock_sys_, Signalfd(-1, _, _)).WillOnce(Return(5));
  EXPECT_CALL(*mock_sys_, Close(5)).WillOnce(Return(0));
  EXPECT_CALL(*mock_sys_, Sigprocmask(SIG_SETMASK, _, nullptr))
      .WillOnce(Invoke([&](int, const sigset_t* set, sigset_t*) {
        EXPECT_TRUE(sigismember(set, SIGPIPE));
        return 0;
      }));

  { SignalRouter router(std::move(mock_sys_)); }
}

// =====================================================================
// 6. Покрытие NativeSystemCalls (реальные системные вызовы)
// =====================================================================

TEST(SignalRouterNativeTest, NativeSystemCalls_RealSysCalls) {
    // Создаем реальный SignalRouter без моков.
    // Это заставит Impl инстанцировать NativeSystemCalls.
    stc::signals::SignalRouter router;
    
    std::atomic<bool> handler_called{false};
    router.RegisterHandler(SIGUSR1, [&](int sig) {
        handler_called = true;
    });
    
    // Запуск покроет EpollCreate1 и EpollCtl
    router.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // Цикл EpollWait покроется
    
    // Отправляем сигнал самому процессу.
    // SignalRouter заблокировал SIGUSR1 через sigprocmask, 
    // поэтому сигнал не убьет процесс, а будет прочитан через signalfd.
    // Это покроет NativeSystemCalls::Read
    kill(getpid(), SIGUSR1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Ждем обработки и Dispatch
    
    router.Stop();
    
    EXPECT_TRUE(handler_called);
}

// =====================================================================
// 7. Покрытие ветки ошибки переконфигурации signalfd (строки 136-137)
// =====================================================================

TEST_F(SignalRouterTest, RegisterHandler_FailsOnSignalfdReconfigure) {
    MockSystemCalls* mock_ptr = mock_sys_.get();
    SignalRouter router(std::move(mock_sys_));
    
    EXPECT_CALL(*mock_ptr, Sigprocmask(SIG_BLOCK, _, nullptr)).WillOnce(Return(0));
    EXPECT_CALL(*mock_ptr, Signalfd(3, _, _)).WillOnce(Return(-1));
    
    // РАЗРЕШАЕМ вызов Sigprocmask(2, ...) из деструктора (SIG_SETMASK == 2)
    EXPECT_CALL(*mock_ptr, Sigprocmask(2, _, nullptr)).WillRepeatedly(Return(0));
    
    EXPECT_THROW(router.RegisterHandler(SIGUSR1, [](int){}), std::system_error);
}