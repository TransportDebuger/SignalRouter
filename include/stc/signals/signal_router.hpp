/**
@file signal_router.hpp
@brief Асинхронный маршрутизатор POSIX-сигналов на основе signalfd и epoll.
@version 5.0.0
@date 2026-07-20
*/
#pragma once

#include <functional>
#include <memory>

namespace stc::signals {

class ISystemCalls;  // Forward declaration для сохранения инкапсуляции

/**
@class SignalRouter
@brief Обеспечивает потокобезопасную регистрацию и асинхронную диспетчеризацию сигналов.
*/
class SignalRouter {
 public:
  /// @brief Тип функции-обработчика сигнала.
  using Handler = std::function<void(int)>;

  /**
    @brief Конструирует экземпляр маршрутизатора и инициализирует системные ресурсы.
    @param[in] sys_calls Указатель на интерфейс системных вызовов. Если равен nullptr, автоматически инстанцируется нативная реализация.
    @throw std::system_error При ошибке сохранения маски сигналов или создания файлового дескриптора signalfd.
    */
  explicit SignalRouter(std::unique_ptr<ISystemCalls> sys_calls = nullptr);

  /// @brief Деструктор. Гарантирует остановку фонового потока, закрытие дескрипторов и восстановление исходной маски сигналов.
  ~SignalRouter();

  SignalRouter(const SignalRouter&) = delete;
  SignalRouter& operator=(const SignalRouter&) = delete;

  /**
    @brief Регистрирует обработчик для указанного сигнала.
    @param[in] signum Номер сигнала.
    @param[in] handler Функция-обработчик.
    @throw std::invalid_argument Если номер сигнала недопустим (SIGKILL, SIGSTOP, <= 0, >= NSIG).
    @throw std::system_error При ошибке системного вызова sigprocmask или signalfd.
    */
  void RegisterHandler(int signum, Handler handler);

  /**
    @brief Удаляет все зарегистрированные обработчики для указанного сигнала из внутреннего реестра.
    @param[in] signum Номер сигнала.
    @throw std::invalid_argument Если номер сигнала недопустим (SIGKILL, SIGSTOP, <= 0, >= NSIG).
    */
  void UnregisterHandler(int signum);

  /**
    @brief Запускает фоновый поток мониторинга событий и диспетчеризации сигналов.
    @throw std::runtime_error Если фоновый поток уже запущен (находится в состоянии joinable).
    */
  void Start();

  /**
    @brief Останавливает фоновый поток мониторинга событий и блокирует вызывающий поток до его завершения.
    @throw Не выбрасывает исключений (noexcept).
    */
  void Stop() noexcept;

  /**
    @brief Проверяет, активен ли фоновый поток мониторинга событий.
    @return true Если поток запущен и еще не присоединен.
    */
  bool IsRunning() const noexcept;

  /**
    @brief Возвращает ссылку на глобальный экземпляр маршрутизатора (Meyers Singleton).
    @return SignalRouter& Ссылка на единственный экземпляр маршрутизатора в процессе.
    */
  static SignalRouter& Global();

 private:
    /// @private Предварительное объявление структуры скрытой реализации (PIMPL) для инкапсуляции POSIX-специфичных типов.
  class Impl;

  /// @private Умный указатель на скрытую реализацию, обеспечивающий строгую инкапсуляцию состояния и стабильность ABI.
  std::unique_ptr<Impl> impl_;
};

}  // namespace stc::signals