# tests/CMakeLists.txt
include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Имя цели должно строго совпадать с тем, что вызывается в корневом CMakeLists.txt
add_executable(signal_router_test 
    signal_router_test.cpp
)

target_link_libraries(signal_router_test 
    PRIVATE 
        stc_signals 
        GTest::gtest_main 
        GTest::gmock
)

target_compile_features(signal_router_test PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(signal_router_test)