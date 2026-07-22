# stc::signals

Инфраструктурная библиотека для асинхронной и потокобезопасной маршрутизации POSIX-сигналов в Linux-приложениях на C++20. 
Библиотека транслирует асинхронные сигналы ядра в синхронные события ввода-вывода, позволяя обрабатывать их в пользовательском потоке без ограничений `async-signal-safety`.

## Особенности

- **Обход ограничений POSIX**: Использование `signalfd(2)` и `epoll(7)` элиминирует необходимость соблюдения `async-signal-safety` (запрет на `malloc`, `std::mutex`, STL) в обработчиках сигналов.
- **Инкапсуляция и тестируемость (PIMPL + DIP)**: Все системные вызовы скрыты за интерфейсом `ISystemCalls`. Это обеспечивает чистоту публичного API и возможность инъекции Mock-объектов для изолированного unit-тестирования.
- **RAII-управление жизненным циклом**: Использование `std::jthread` и `std::stop_token` (C++20) гарантирует безопасную остановку и присоединение фонового потока при разрушении объекта.
- **Реентерабельность**: Паттерн Copy-on-Dispatch позволяет безопасно вызывать методы регистрации/отмены сигналов непосредственно из тела пользовательского обработчика без риска взаимоблокировки (deadlock).
- **Автоматическое восстановление состояния**: Деструктор гарантирует восстановление исходной маски сигналов процесса, предотвращая побочные эффекты для других компонентов.

## Требования

| Компонент      | Версия      | Обоснование                                      |
|----------------|-------------|--------------------------------------------------|
| Компилятор C++ | GCC 11+ / Clang 14+ | Поддержка C++20 (`std::jthread`, `std::stop_token`) |
| ОС             | Linux       | Использование `signalfd`, `epoll`, `pthread_sigmask` |
| CMake          | 3.20+       | Базовые возможности, `FetchContent`              |

## Интеграция (CMake)

Библиотека подключается через `FetchContent`. Согласно стандарту проекта, при интеграции необходимо принудительно отключать сборку тестов и санитайзеров, чтобы не засорять граф целей основного проекта.

```cmake
include(FetchContent)

# Принудительное отключение побочных целей библиотеки
set(STC_SIGNALS_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(STC_SIGNALS_ENABLE_SANITIZERS OFF CACHE BOOL "" FORCE)
set(STC_SIGNALS_ENABLE_COVERAGE   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    stc_signals
    GIT_REPOSITORY https://git.stc.local/stc/signals.git
    GIT_TAG        v1.0.0
)
FetchContent_MakeAvailable(stc_signals)

target_link_libraries(your_target PRIVATE stc::signals)
```

## Быстрый старт

Минимальный пример перехвата сигналов завершения процесса (`SIGTERM`, `SIGINT`) с использованием глобального синглтона.

```cpp
#include <stc/signals/signal_router.hpp>
#include <iostream>
#include <csignal>

int main() {
    auto& router = stc::signals::SignalRouter::Global();

    // Регистрация обработчика
    router.RegisterHandler(SIGTERM, [](int signum) {
        std::cout << "Received SIGTERM. Initiating graceful shutdown...\n";
    });

    router.RegisterHandler(SIGINT, [](int signum) {
        std::cout << "Received SIGINT. Stopping...\n";
    });

    // Запуск фонового потока мониторинга
    router.Start();

    // ... основная логика приложения ...
    
    // Деструктор синглтона автоматически вызовет Stop() и восстановит маски
    return 0;
}
```

## Сборка и верификация

Система сборки предоставляет набор агрегированных целей для автоматизации проверок.

**Базовая сборка:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target stc_signals -j$(nproc)
```

**Пользовательские цели (Custom Targets):**

| Цель            | Назначение                                                                 |
|-----------------|----------------------------------------------------------------------------|
| `linter-test`   | Проверка стиля (`clang-format`, Google Style) и статический анализ (`cppcheck`). |
| `unit-test`     | Сборка с флагами покрытия (`--coverage`) и запуск GoogleTest.              |
| `mem-test`      | Проверка утечек памяти: фаза 1 (ASan/UBSan), фаза 2 (Valgrind).            |
| `gcov-report`   | Генерация HTML-отчета о покрытии кода (`lcov` + `genhtml`).                |
| `test-all`      | Последовательный запуск `linter-test` -> `unit-test` -> `mem-test` -> `gcov-report`. |
| `docs`          | Генерация API-документации через Doxygen (требует наличия `Doxyfile`).     |

Запуск полного цикла верификации:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target test-all -j$(nproc)
```

## Документация

Для генерации локальной HTML-документации необходимо установить `Doxygen` и `Graphviz`, затем выполнить:
```bash
cmake -B build
cmake --build build --target docs
```
Точка входа: `build/docs/html/index.html`.

Подробные архитектурные обоснования, справочник API и примеры интеграции доступны в сгенерированной документации (страницы `page_signal_router_arch`, `page_build_system`, `page_testing`).
