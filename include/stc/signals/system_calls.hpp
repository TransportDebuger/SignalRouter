/**
@file system_calls.hpp
@brief Абстракция над системными вызовами POSIX для инкапсуляции и
тестируемости.
@version 1.0.0
@date 2026-07-20
*/

#pragma once

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/types.h>

#include <csignal>
#include <cstddef>

namespace stc::signals {

/**
@class ISystemCalls
@brief Интерфейс абстракции системных вызовов ядра Linux (epoll, signalfd,
sigprocmask).
*/
class ISystemCalls {
 public:
  /// @brief Виртуальный деструктор.
  virtual ~ISystemCalls() = default;

  /**
  @brief Изменяет или возвращает маску сигналов.
  @param[in] how Флаг операции (SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK).
  @param[in] set Указатель на набор сигналов.
  @param[out] oldset Указатель для сохранения предыдущей маски.
  @return int 0 при успехе, -1 при ошибке (с установкой errno).
  */
  virtual int Sigprocmask(int how, const sigset_t* set, sigset_t* oldset) = 0;

  /**
  @brief Создает или обновляет файловый дескриптор для приема сигналов.
  @param[in] fd Существующий файловый дескриптор или -1 для создания нового.
  @param[in] mask Маска сигналов для отслеживания.
  @param[in] flags Флаги (SFD_CLOEXEC, SFD_NONBLOCK).
  @return int Файловый дескриптор при успехе, -1 при ошибке.
  */
  virtual int Signalfd(int fd, const sigset_t* mask, int flags) = 0;

  /**
  @brief Создает экземпляр epoll.
  @param[in] flags Флаги создания (EPOLL_CLOEXEC).
  @return int Файловый дескриптор epoll при успехе, -1 при ошибке.
  */
  virtual int EpollCreate1(int flags) = 0;

  /**
  @brief Управляет интересующими событиями для файлового дескриптора в epoll.
  @param[in] epfd Файловый дескриптор epoll.
  @param[in] op Операция (EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLL_CTL_DEL).
  @param[in] fd Целевой файловый дескриптор.
  @param[in] event Указатель на структуру события.
  @return int 0 при успехе, -1 при ошибке.
  */
  virtual int EpollCtl(int epfd, int op, int fd, struct epoll_event* event) = 0;

  /**
  @brief Ожидает события на экземпляре epoll.
  @param[in] epfd Файловый дескриптор epoll.
  @param[out] events Массив для приема произошедших событий.
  @param[in] maxevents Максимальное количество возвращаемых событий.
  @param[in] timeout Таймаут в миллисекундах (-1 для бесконечного ожидания).
  @return int Количество обработанных событий, -1 при ошибке или прерывании.
  */
  virtual int EpollWait(int epfd, struct epoll_event* events, int maxevents,
                        int timeout) = 0;

  /**
  @brief Читает данные из файлового дескриптора.
  @param[in] fd Файловый дескриптор.
  @param[out] buf Буфер для чтения.
  @param[in] count Максимальное количество байт для чтения.
  @return ssize_t Количество прочитанных байт, -1 при ошибке.
  */
  virtual ssize_t Read(int fd, void* buf, size_t count) = 0;

  /**
  @brief Закрывает файловый дескриптор.
  @param[in] fd Файловый дескриптор.
  @return int 0 при успехе, -1 при ошибке.
  */
  virtual int Close(int fd) = 0;
};

}  // namespace stc::signals