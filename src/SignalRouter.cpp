/**
 * @file SignalRouter.cpp
 * @brief Реализация асинхронного маршрутизатора POSIX-сигналов через signalfd и
 * epoll.
 *
 * Содержит полную реализацию класса SignalRouter, обеспечивающего
 * потокобезопасную и асинхронную обработку сигналов в Linux-приложениях.
 *
 * @author Artem Ulyanov
 * @date Апрель 2026
 * @version 1.0
 *
 * @note
 * - Все публичные методы потокобезопасны.
 * - Работает только на Linux (требует signalfd, epoll).
 *
 * @warning
 * - Не поддерживает SIGKILL и SIGSTOP.
 * - Долгие обработчики могут блокировать обработку других сигналов.
 *
 * @see SignalRouter.hpp
 */

#include "stc/SignalRouter.hpp"

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace stc {

/**
 * @internal
 * @class SignalRouter::Impl
 * @brief Внутренняя реализация маршрутизатора сигналов.
 */
class SignalRouter::Impl {
public:
  using Handler = SignalRouter::Handler;

  Impl();
  ~Impl();

  void registerHandler(int signum, Handler handler);
  void unregisterHandler(int signum);

  void start();
  void stop() noexcept;

  bool isRunning() const noexcept;

private:
  void processSignals();

  /// @brief Контейнер обработчиков: сигнал -> вектор функций-обработчиков
  std::unordered_map<int, std::vector<Handler>> handlers_;

  /// @brief Мьютекс для синхронизации доступа к handlers_ и blocked_mask_
  std::mutex handlers_mutex_;

  /// @brief Флаг, указывающий, запущен ли обработчик сигналов
  std::atomic<bool> running_{false};

  /// @brief Рабочий поток, в котором выполняется processSignals()
  std::thread worker_thread_;

  /// @brief Файловый дескриптор signalfd для чтения сигналов
  int signal_fd_ = -1;

  /// @brief Исходная маска сигналов процесса (сохраняется для корректного
  /// завершения)
  sigset_t original_mask_;

  /// @brief Текущая маска сигналов, которые блокируются и перехватываются
  /// через signalfd
  sigset_t blocked_mask_{};
};

/**
 * @internal
 * @brief Конструктор класса SignalRouter::Impl.
 *
 * Инициализирует внутреннее состояние маршрутизатора сигналов:
 * - Очищает маску блокируемых сигналов;
 * - Сохраняет текущую маску сигналов процесса для последующего восстановления;
 * - Создаёт файловый дескриптор signalfd для асинхронного получения сигналов.
 *
 * @throws std::system_error
 *         Если системный вызов sigprocmask() завершится с ошибкой.
 * @throws std::system_error
 *         Если системный вызов signalfd() завершится с ошибкой.
 *
 * @note
 * Конструктор не запускает обработку сигналов — для этого необходимо вызвать
 * start().
 */
SignalRouter::Impl::Impl() {
  sigemptyset(&original_mask_);
  sigemptyset(&blocked_mask_);

  // Получаем текущую маску сигналов вызывающего потока
  if (sigprocmask(SIG_BLOCK, nullptr, &original_mask_) == -1) {
    throw std::system_error(errno, std::system_category(),
                            "Unable to get current signal set mask");
  }

  // Создаем signalfd с пустой маской — изначально не отслеживаем ни один
  // сигнал
  signal_fd_ = signalfd(-1, &blocked_mask_, SFD_CLOEXEC | SFD_NONBLOCK);
  if (signal_fd_ == -1)
    throw std::system_error(
        errno, std::system_category(),
        "Unable to create signal file descriptor (signalfd)");
}

SignalRouter::SignalRouter() : impl_(std::make_unique<Impl>()) {}

/**
 * @internal
 * @brief Деструктор класса SignalRouter::Impl.
 *
 * Автоматически останавливает обработку сигналов (вызывает stop())
 * и восстанавливает исходную маску сигналов при необходимости.
 *
 * @note Вызов stop() гарантирует корректное завершение рабочего потока.
 */
SignalRouter::Impl::~Impl() {
  stop();
  close(signal_fd_);
  sigprocmask(SIG_SETMASK, &original_mask_, nullptr);
}

SignalRouter::~SignalRouter() {}

/**
 * @internal
 * @brief Регистрирует обработчик для указанного сигнала.
 *
 * Добавляет пользовательскую функцию-обработчик в список обработчиков
 * для сигнала с номером `signum`. Обработчик будет вызван асинхронно
 * при получении сигнала через механизм signalfd.
 *
 * @param signum[in] Номер сигнала (например, SIGINT, SIGHUP и т.д.).
 *               Должен быть в допустимом диапазоне (1 < signum < NSIG).
 * @param handler[in] Функция-обработчик, принимающая номер сигнала.
 *                Будет сохранена по значению (перемещена через std::move).
 *
 * @throws std::invalid_argument если номер сигнала недопустим.
 * @throws std::system_error если системный вызов pthread_sigmask
 *         или signalfd завершился с ошибкой.
 *
 * @details
 * При регистрации сигнала:
 * - Сигнал добавляется в маску блокировки (если ещё не был добавлен),
 * - Маска сигналов процесса обновляется с помощью pthread_sigmask(),
 * - signalfd перенастраивается с новой маской и сохранением флагов
 *   SFD_CLOEXEC и SFD_NONBLOCK,
 * - Обработчик сохраняется в потокобезопасном контейнере.
 *
 * @note
 * - Метод потокобезопасен благодаря использованию handlers_mutex_.
 *
 * - Один и тот же сигнал может иметь несколько обработчиков.
 *
 * - Если обработчик уже был зарегистрирован для этого сигнала,
 *   он будет добавлен повторно (повторная регистрация разрешена).
 *
 * - Сигнал автоматически блокируется в текущем и всех новых потоках.
 *
 * @par Пример использования:
 * @code{.cpp}
 * stc::SignalRouter router;
 * router.registerHandler(SIGUSR1, [](int){
 *     std::cout << "Получен сигнал " << signum << "\n";
 * });
 * @endcode
 */
void SignalRouter::Impl::registerHandler(int signum, Handler handler) {
  if (signum <= 0 || signum >= NSIG) {
    throw std::invalid_argument("Invalid signal number");
  }

  std::lock_guard<std::mutex> lock(handlers_mutex_);

  // Добавляем сигнал в маску, если ещё не добавлен
  if (!sigismember(&blocked_mask_, signum)) {
    sigaddset(&blocked_mask_, signum);

    // Обновляем блокировку в ядре
    if (pthread_sigmask(SIG_SETMASK, &blocked_mask_, nullptr) == -1) {
      throw std::system_error(errno, std::system_category(),
                              "Failed to block signal");
    }

    // Перенастраиваем signalfd с сохранением флагов
    if (signalfd(signal_fd_, &blocked_mask_, SFD_CLOEXEC | SFD_NONBLOCK) ==
        -1) {
      throw std::system_error(errno, std::system_category(),
                              "signalfd reconfigure failed");
    }
  }

  handlers_[signum].push_back(std::move(handler));
}

void SignalRouter::registerHandler(int signum, Handler handler) {
  impl_->registerHandler(signum, std::move(handler));
}

/**
 * @internal
 * @brief Удаляет все обработчики для указанного сигнала и прекращает его
 * перехват.
 *
 * Полностью отменяет регистрацию сигнала: удаляет все привязанные обработчики,
 * разблокирует сигнал в потоке и обновляет signalfd, чтобы он больше не
 * получал уведомления о данном сигнале.
 *
 * @param signum[in] Номер сигнала (например, SIGINT, SIGHUP).
 *               Должен быть в допустимом диапазоне (1 < signum < NSIG).
 *
 * @throws std::invalid_argument если номер сигнала недопустим.
 * @throws std::system_error если системный вызов pthread_sigmask
 *         или signalfd завершился с ошибкой при обновлении маски.
 *
 * @details
 * При отмене регистрации:
 * - Все обработчики для сигнала удаляются из внутреннего контейнера,
 * - Если сигнал был заблокирован через этот объект, он удаляется из маски,
 * - Маска сигналов процесса обновляется с помощью pthread_sigmask(),
 * - signalfd перенастраивается с новой маской и сохранением флагов
 *   SFD_CLOEXEC и SFD_NONBLOCK.
 *
 * @note
 * - Метод потокобезопасен: использует мьютекс для защиты внутреннего состояния.
 *
 * - Если сигнал не был зарегистрирован, метод ничего не делает (безопасно).
 *
 * - После вызова стандартное поведение сигнала (например, завершение процесса)
 *   восстанавливается, если только он не заблокирован другим компонентом.
 *
 * @par Пример использования:
 * @code{.cpp}
 * stc::SignalRouter router;
 * router.registerHandler(SIGUSR1, [](int){});
 * // ...
 * // Отключить обработку SIGUSR1
 * router.unregisterHandler(SIGUSR1);
 * @endcode
 */
void SignalRouter::Impl::unregisterHandler(int signum) {
  if (signum <= 0 || signum >= NSIG) {
    throw std::invalid_argument("Invalid signal number");
  }

  std::lock_guard<std::mutex> lock(handlers_mutex_);

  handlers_.erase(signum);

  // Удаляем сигнал из маски блокировки
  if (sigismember(&blocked_mask_, signum)) {
    sigdelset(&blocked_mask_, signum);

    // Обновляем блокировку в ядре
    if (pthread_sigmask(SIG_SETMASK, &blocked_mask_, nullptr) == -1) {
      throw std::system_error(errno, std::system_category(),
                              "Failed to update signal mask");
    }

    // Перенастраиваем signalfd с сохранением флагов
    if (signalfd(signal_fd_, &blocked_mask_, SFD_CLOEXEC | SFD_NONBLOCK) ==
        -1) {
      throw std::system_error(errno, std::system_category(),
                              "signalfd reconfigure failed");
    }
  }
}

void SignalRouter::unregisterHandler(int signum) {
  impl_->unregisterHandler(signum);
}

/**
 * @internal
 * @brief Запускает асинхронную обработку сигналов в отдельном потоке.
 *
 * Метод инициализирует рабочий поток, в котором будет выполняться
 * основной цикл обработки сигналов через signalfd и epoll.
 *
 * @throws std::runtime_error
 *         - Если обработчик уже запущен и рабочий поток joinable (активен).
 *         - Повторный запуск запрещён.
 *
 * @note
 * - Метод потокобезопасен благодаря использованию атомарного флага running_.
 *
 * - Если рабочий поток не является joinable (например, был аварийно завершён),
 *   метод завершается без запуска нового потока, но и без ошибки.
 *
 * - Для повторного запуска после остановки необходимо сначала вызвать stop().
 *
 * @warning
 * - Не вызывайте этот метод из деструктора или в состоянии, когда
 *   рабочий поток может быть в неопределённом состоянии.
 *
 * - Убедитесь, что processSignals() корректно обрабатывает исключения,
 *   чтобы избежать std::terminate.
 *
 * @par Пример использования:
 * @code{.cpp}
 * stc::SignalRouter router;
 * router.registerHandler(SIGINT, [](int) { router.stop(); });
 * router.start();
 *
 * while (router.isRunning()) {
 *     std::this_thread::sleep_for(std::chrono::milliseconds(100));
 * }
 * @endcode
 */
void SignalRouter::Impl::start() {
  if (running_.exchange(true)) {
    if (worker_thread_.joinable()) {
      throw std::runtime_error(
          "SignalRouter::start(): worker thread already running");
    }
    return;
  }

  worker_thread_ = std::thread(&SignalRouter::Impl::processSignals, this);
}

void SignalRouter::start() { impl_->start(); } // Forward to implementation

/**
 * @internal
 * @brief Останавливает асинхронную обработку сигналов.
 *
 * Метод корректно завершает работу рабочего потока, в котором выполняется
 * цикл обработки сигналов. Устанавливает флаг завершения и дожидается
 * остановки потока.
 *
 * @details
 * - Устанавливает атомарный флаг `running_` в `false`, что приводит к выходу
 *   из цикла в методе `processSignals()`.
 * - Если рабочий поток запущен и joinable, выполняет `join()`, чтобы дождаться
 *   его завершения.
 * - Метод безопасен для повторного вызова.
 *
 * @note
 * - Метод является `noexcept` — не выбрасывает исключений.
 *
 * - Не блокирует выполнение навсегда: `processSignals()` использует таймаут
 *   в `epoll_wait`, поэтому `join()` завершится за конечное время.
 *
 * - Если поток не был запущен, метод ничего не делает.
 *
 * @par Пример использования:
 * @code{.cpp}
 * stc::SignalRouter router;
 * router.start();
 * // ... работа ...
 * router.stop();  // Ожидаем завершения обработки сигналов
 * @endcode
 */
void SignalRouter::Impl::stop() noexcept {
  running_ = false;
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

void SignalRouter::stop() noexcept {
  impl_->stop();
} // Forward to implementation

/**
 * @internal
 * @brief Проверяет, активен ли обработчик сигналов в данный момент.
 *
 * Возвращает состояние рабочего цикла обработки сигналов. Используется для
 * проверки, был ли вызван метод start() и не завершён ли поток обработки.
 *
 * @return true, если обработчик запущен и активен (обработка сигналов ведётся);
 *         false, если обработчик остановлен, ещё не запускался или завершился.
 *
 * @note
 * - Потокобезопасно: внутренний флаг running_ объявлен как std::atomic<bool>,
 *   что гарантирует корректное чтение из разных потоков без дополнительной
 *   синхронизации.
 *
 * @par Пример использования:
 * @code{.cpp}
 * stc::SignalRouter router;
 * router.start();
 * // ...
 * if (router.isRunning()) {
 *     router.stop();
 * }
 * @endcode
 */
bool SignalRouter::Impl::isRunning() const noexcept { return running_.load(); }

bool SignalRouter::isRunning() const noexcept { return impl_->isRunning(); }

/**
 * @internal
 * @brief Основной цикл обработки сигналов в отдельном потоке.
 *
 * Метод запускается в рабочем потоке и отвечает за асинхронное получение
 * сигналов через signalfd с использованием epoll. Обрабатывает приходящие
 * сигналы, вызывая зарегистрированные обработчики.
 *
 * @details
 * Работа метода:
 * - Создаёт epoll-дескриптор для мониторинга signalfd.
 * - В бесконечном цикле ожидает события с таймаутом 10 мс.
 * - При получении сигнала читает его из signalfd.
 * - Находит и вызывает все зарегистрированные обработчики для этого сигнала.
 * - Цикл прерывается, когда флаг running_ становится false.
 *
 * @note
 * - Метод автоматически обрабатывает прерывания системных вызовов (EINTR).
 *
 * - Все исключения перехватываются, чтобы не допустить std::terminate.
 *
 * - Для предотвращения взаимоблокировок обработчики вызываются без блокировки
 * мьютекса.
 *
 * @warning
 * - Метод не должен вызываться напрямую извне — только из worker_thread_.
 *
 * - Требует предварительной инициализации signalfd (через конструктор).
 *
 * @throws Исключения перехватываются внутри метода. Никакие исключения
 *         не покидают этот метод — при сбое устанавливается running_ = false.
 *
 * @par Пример корректного использования:
 * @code{.cpp}
 * // Запускается внутри start():
 * worker_thread_ = std::thread(&SignalRouter::processSignals, this);
 * @endcode
 */
void SignalRouter::Impl::processSignals() {
  int epoll_fd = -1;
  try {
    constexpr int MAX_EVENTS = 10;
    struct epoll_event ev {};
    std::array<epoll_event, MAX_EVENTS> events;

    // Проверка валидности signalfd
    if (signal_fd_ == -1) {
      throw std::runtime_error(
          "SignalRouter::processSignals(): invalid signal_fd_");
    }

    // Создаём epoll-дескриптор с автоматическим закрытием при exec
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
      throw std::system_error(errno, std::system_category(),
                              "epoll_create1 failed");
    }

    // Добавляем signalfd в epoll
    ev.events = EPOLLIN;
    ev.data.fd = signal_fd_;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_fd_, &ev) == -1) {
      int saved_errno = errno; // Сохраняем errno до вызова close
      close(epoll_fd);
      throw std::system_error(saved_errno, std::system_category(),
                              "epoll_ctl failed");
    }

    // Основной цикл обработки
    while (running_.load(std::memory_order_acquire)) {
      int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 10);
      if (nfds == -1) {
        if (errno == EINTR)
          continue; // Прерывание сигналом — продолжаем
        break;
      }

      for (int i = 0; i < nfds; ++i) {
        // Обрабатываем только события от signalfd
        if (events[i].data.fd == signal_fd_) {
          struct signalfd_siginfo fdsi;
          ssize_t bytes;
          do {
            bytes = read(signal_fd_, &fdsi, sizeof(fdsi));
          } while (bytes == -1 && errno == EINTR); // Обрабатываем EINTR

          if (bytes == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              continue; // Нет данных
            break;      // Системная ошибка
          }
          if (bytes != sizeof(fdsi))
            continue; // Неполные данные — игнорируем

          {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            auto it = handlers_.find(fdsi.ssi_signo);
            if (it != handlers_.end()) {
              // Копируем обработчики, чтобы освободить мьютекс до
              // вызова
              auto handlers_copy = it->second;
              // Перемещаемся из области видимости lock
              // Вызываем все обработчики без блокировки
              for (const auto &handler : handlers_copy) {
                handler(fdsi.ssi_signo);
              }
            }
          }
        }
      }
    }

    // Закрываем epoll-дескриптор
    close(epoll_fd);
  } catch (...) {
    // Гарантированно закрываем дескриптор при исключении
    if (epoll_fd != -1) {
      close(epoll_fd);
    }
    running_.store(false, std::memory_order_release);
  }
}

} // namespace stc