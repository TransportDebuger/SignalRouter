/**
@file signal_router.cpp
@brief Реализация асинхронного маршрутизатора POSIX-сигналов.
@version 5.0.0
@date 2026-07-20
*/
#include "stc/signals/signal_router.hpp"

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>

#include "stc/signals/system_calls.hpp"

namespace stc::signals {

/**
 * @internal
 * @class NativeSystemCalls
 * @brief Нативная реализация ISystemCalls, делегирующая вызовы ядру Linux.
 */
class NativeSystemCalls final : public ISystemCalls {
public:
    /**
    @brief Изменяет или возвращает маску сигналов текущего потока.
    @param[in] how Флаг операции (SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK).
    @param[in] set Указатель на набор сигналов.
    @param[out] oldset Указатель для сохранения предыдущей маски.
    @return int 0 при успехе, -1 при ошибке (с установкой errno).
    */
    int Sigprocmask(int how, const sigset_t* set, sigset_t* oldset) override {
        return pthread_sigmask(how, set, oldset);
    }
    
    /**
    @brief Создает или обновляет файловый дескриптор для приема сигналов.
    @param[in] fd Существующий файловый дескриптор или -1 для создания нового.
    @param[in] mask Маска сигналов для отслеживания.
    @param[in] flags Флаги (SFD_CLOEXEC, SFD_NONBLOCK).
    @return int Файловый дескриптор (>= 0) при успехе, -1 при ошибке.
    */
    int Signalfd(int fd, const sigset_t* mask, int flags) override {
        return signalfd(fd, mask, flags);
    }
    
    /**
    @brief Создает экземпляр epoll.
    @param[in] flags Флаги создания (EPOLL_CLOEXEC).
    @return int Файловый дескриптор epoll (>= 0) при успехе, -1 при ошибке.
    */
    int EpollCreate1(int flags) override { 
        return epoll_create1(flags); 
    }
    
    /**
    @brief Управляет интересующими событиями для файлового дескриптора в epoll.
    @param[in] epfd Файловый дескриптор epoll.
    @param[in] op Операция (EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLL_CTL_DEL).
    @param[in] fd Целевой файловый дескриптор.
    @param[in] event Указатель на структуру события.
    @return int 0 при успехе, -1 при ошибке.
    */
    int EpollCtl(int epfd, int op, int fd, struct epoll_event* event) override {
        return epoll_ctl(epfd, op, fd, event);
    }
    
    /**
    @brief Ожидает события на экземпляре epoll.
    @param[in] epfd Файловый дескриптор epoll.
    @param[out] events Массив для приема произошедших событий.
    @param[in] maxevents Максимальное количество возвращаемых событий.
    @param[in] timeout Таймаут в миллисекундах (-1 для бесконечного ожидания).
    @return int Количество обработанных событий (>= 0), -1 при ошибке или прерывании.
    */
    int EpollWait(int epfd, struct epoll_event* events, int maxevents, int timeout) override {
        return epoll_wait(epfd, events, maxevents, timeout);
    }
    
    /**
    @brief Читает данные из файлового дескриптора.
    @param[in] fd Файловый дескриптор.
    @param[out] buf Буфер для чтения.
    @param[in] count Максимальное количество байт для чтения.
    @return ssize_t Количество прочитанных байт (>= 0), -1 при ошибке.
    */
    ssize_t Read(int fd, void* buf, size_t count) override {
        return read(fd, buf, count);
    }
    
    /**
    @brief Закрывает файловый дескриптор.
    @param[in] fd Файловый дескриптор.
    @return int 0 при успехе, -1 при ошибке.
    */
    int Close(int fd) override { 
        return close(fd); 
    }
};

/**
@class SignalRouter::Impl
@brief Скрытая реализация маршрутизатора (PIMPL).
*/
class SignalRouter::Impl {
 public:

  /**
    @brief Конструирует скрытую реализацию маршрутизатора и инициализирует системные ресурсы.
    @param[in] sys_calls Указатель на интерфейс системных вызовов. Если равен nullptr, автоматически инстанцируется NativeSystemCalls.
    @throw std::system_error При ошибке получения исходной маски сигналов или создания файлового дескриптора signalfd.
    */
  explicit Impl(std::unique_ptr<ISystemCalls> sys_calls)
      : sys_(std::move(sys_calls)) {

    if (!sys_) {
      sys_ = std::make_unique<NativeSystemCalls>();
    }

    sigemptyset(&original_mask_);
    sigemptyset(&blocked_mask_);
    if (sys_->Sigprocmask(0, nullptr, &original_mask_) == -1) {
      throw std::system_error(errno, std::system_category(),
                              "Failed to get original signal mask");
    }
    InitSignalFd();
  }

  /**
    @brief Деструктор скрытой реализации. 
    Гарантирует остановку фонового потока, закрытие файловых дескрипторов 
    и восстановление исходной маски сигналов потока.
    */
  ~Impl() {
    Stop();
    if (signal_fd_ >= 0) sys_->Close(signal_fd_);
    sys_->Sigprocmask(SIG_SETMASK, &original_mask_, nullptr);
  }

  /**
    @brief Регистрирует обработчик во внутреннем реестре и обновляет системную маску сигналов.
    @param[in] signum Номер сигнала. Валидация выполняется до захвата мьютекса.
    @param[in] handler Функция-обработчик, сохраняемая в вектор callback-функций.
    @throw std::invalid_argument Если signum недопустим (SIGKILL, SIGSTOP, <= 0, >= NSIG).
    @throw std::system_error Если системные вызовы sigprocmask или signalfd в UpdateMask() завершились с ошибкой.
    */
  void RegisterHandler(int signum, Handler handler) {
    if (signum <= 0 || signum >= NSIG || signum == SIGKILL ||
        signum == SIGSTOP) {
      throw std::invalid_argument("Invalid signal number");
    }
    std::lock_guard lock(mutex_);
    if (!sigismember(&blocked_mask_, signum)) {
      sigaddset(&blocked_mask_, signum);
      UpdateMask();
    }
    handlers_[signum].push_back(std::move(handler));
  }

  /**
    @brief Удаляет все зарегистрированные обработчики для указанного сигнала из внутреннего реестра.
    @param[in] signum Номер сигнала. Валидация выполняется до захвата мьютекса.
    @throw std::invalid_argument Если signum недопустим (SIGKILL, SIGSTOP, <= 0, >= NSIG).
    */
  void UnregisterHandler(int signum) {
    if (signum <= 0 || signum >= NSIG || signum == SIGKILL ||
        signum == SIGSTOP) {
      throw std::invalid_argument("Invalid signal number");
    }
    std::lock_guard lock(mutex_);
    handlers_.erase(signum);
  }

  /**
    @brief Запускает фоновый поток мониторинга событий epoll.
    @throw std::runtime_error Если фоновый поток уже запущен (находится в состоянии joinable).
    */
  void Start() {
    if (worker_thread_.joinable()) {
      throw std::runtime_error("SignalRouter already running");
    }
    worker_thread_ =
        std::jthread([this](std::stop_token stoken) { WorkerLoop(stoken); });
  }

  /**
    @brief Останавливает фоновый поток мониторинга событий и блокирует вызывающий поток до его завершения.
    @note Метод идемпотентен: повторные вызовы для уже остановленного маршрутизатора безопасны.
    */
  void Stop() noexcept {
    if (worker_thread_.joinable()) {
      worker_thread_.request_stop();
      worker_thread_.join();
    }
  }

  /**
    @brief Проверяет, находится ли фоновый поток мониторинга событий в активном состоянии.
    @return true Если поток запущен и еще не присоединен.
    */
  bool IsRunning() const noexcept { return worker_thread_.joinable(); }

 private:

  /**
    @private
    @brief Инициализирует файловый дескриптор signalfd с начальными флагами и пустой маской.
    @throw std::system_error Если системный вызов signalfd завершился с ошибкой.
    */
  void InitSignalFd() {
    signal_fd_ = sys_->Signalfd(-1, &blocked_mask_, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_fd_ == -1) {
      throw std::system_error(errno, std::system_category(),
                              "Failed to create signalfd");
    }
  }

  /**
    @private
    @brief Синхронизирует маску сигналов потока с внутренним состоянием и переконфигурирует файловый дескриптор signalfd.
    @throw std::system_error Если системный вызов sigprocmask или signalfd завершился с ошибкой.
    */
  void UpdateMask() {
    if (sys_->Sigprocmask(SIG_BLOCK, &blocked_mask_, nullptr) == -1) {
      throw std::system_error(errno, std::system_category(),
                              "Failed to block signal");
    }
    if (sys_->Signalfd(signal_fd_, &blocked_mask_,
                       SFD_CLOEXEC | SFD_NONBLOCK) == -1) {
      throw std::system_error(errno, std::system_category(),
                              "Failed to reconfigure signalfd");
    }
  }

  /**
    @private
    @brief Фоновый цикл мониторинга событий epoll и диспетчеризации сигналов.
    @param[in] stoken Токен остановки для кооперативного завершения потока.
    */
  void WorkerLoop(std::stop_token stoken) {
    int epoll_fd = sys_->EpollCreate1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) return;

    struct epoll_event ev {};
    ev.events = EPOLLIN;
    ev.data.fd = signal_fd_;
    sys_->EpollCtl(epoll_fd, EPOLL_CTL_ADD, signal_fd_, &ev);

    struct epoll_event events[10];
    while (!stoken.stop_requested()) {
      int nfds = sys_->EpollWait(epoll_fd, events, 10,
                                 50);
      if (nfds == -1 && errno == EINTR) continue;

      for (int i = 0; i < nfds; ++i) {
        if (events[i].data.fd == signal_fd_) {
          struct signalfd_siginfo fdsi;
          if (sys_->Read(signal_fd_, &fdsi, sizeof(fdsi)) == sizeof(fdsi)) {
            Dispatch(fdsi.ssi_signo);
          }
        }
      }
    }
    sys_->Close(epoll_fd);
  }

  /**
    @private
    @brief Читает сигнал из дескриптора signalfd и диспетчеризирует зарегистрированные обработчики.
    */
  void Dispatch(int signum) {
    std::vector<Handler> local_handlers;
    {
      std::lock_guard lock(mutex_);
      if (auto it = handlers_.find(signum); it != handlers_.end()) {
        local_handlers = it->second;
      }
    }
    for (const auto& handler : local_handlers) {
      handler(signum);
    }
  }

  /// @private Указатель на интерфейс системных вызовов. Обеспечивает инъекцию зависимостей (DIP) и изоляцию от POSIX API.
    std::unique_ptr<ISystemCalls> sys_;

    /// @private Реестр зарегистрированных пользовательских обработчиков, сгруппированных по номеру сигнала.
    std::unordered_map<int, std::vector<Handler>> handlers_;

    /// @private Мьютекс для защиты внутреннего реестра обработчиков и маски сигналов от гонок данных (race conditions).
    std::mutex mutex_;

    /// @private Фоновый поток (C++20), выполняющий цикл epoll_wait и делегирующий чтение в Dispatch().
    std::jthread worker_thread_;

    /// @private Файловый дескриптор signalfd, используемый для чтения структур signalfd_siginfo из ядра.
    int signal_fd_ = -1;

    /// @private Маска сигналов, сохраненная на момент конструирования. Гарантированно восстанавливается в деструкторе (RAII).
    sigset_t original_mask_;

    /// @private Текущая маска сигналов, заблокированных данным экземпляром маршрутизатора и переданных в signalfd.
    sigset_t blocked_mask_{};
};

// === SignalRouter Forwarding ===

SignalRouter::SignalRouter(std::unique_ptr<ISystemCalls> sys_calls)
    : impl_(std::make_unique<Impl>(std::move(sys_calls))) {}

SignalRouter::~SignalRouter() = default;

void SignalRouter::RegisterHandler(int signum, Handler handler) {
  impl_->RegisterHandler(signum, std::move(handler));
}

void SignalRouter::UnregisterHandler(int signum) {
  impl_->UnregisterHandler(signum);
}

void SignalRouter::Start() { impl_->Start(); }

void SignalRouter::Stop() noexcept { impl_->Stop(); }

bool SignalRouter::IsRunning() const noexcept { return impl_->IsRunning(); }

SignalRouter& SignalRouter::Global() {
  static SignalRouter instance;
  return instance;
}

}  // namespace stc::signals