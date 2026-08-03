cmake_minimum_required(VERSION 3.20)
project(prosched)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/heads/main.zip
)
FetchContent_MakeAvailable(googletest)
enable_testing()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/config.txt"
    "${CMAKE_CURRENT_BINARY_DIR}/config.txt"
    COPYONLY
)


add_executable(prosched
  # src/
  src/main.cpp
  src/controller/Controller.cpp
  src/view/View.cpp
  src/view/page/Menu.cpp

  # commands
  src/commands/Interpreter.cpp
  src/commands/Statement.cpp

  # scheduler
  src/scheduler/Scheduler.h
  src/scheduler/process/Process.h
  src/scheduler/worker/Worker.h

  # memory
  src/memory/PagingManager.h
)
target_include_directories(prosched PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(prosched PRIVATE -Wall -Wextra -Wpedantic -g -D_GLIBCXX_ASSERTIONS -fsanitize=address)
target_link_options(prosched PRIVATE -fsanitize=address)

add_executable(prosched_tests
  tests/ProcessTest.cpp
  tests/WorkerTest.cpp
  tests/SchedulerTest.cpp
  tests/InterpreterTest.cpp
  tests/ControllerTest.cpp
  tests/ConfigTest.cpp
  tests/PagingManagerTest.cpp
  tests/MemoryAdmissionTest.cpp
  tests/PinLifecycleTest.cpp
  tests/UserProgramPagingTest.cpp
  # Controller, View and the interpreter are not header-only; their
  # implementations must be linked
  src/controller/Controller.cpp
  src/view/View.cpp
  src/view/page/Menu.cpp
  src/commands/Interpreter.cpp
  src/commands/Statement.cpp
)
target_include_directories(prosched_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(prosched_tests GTest::gtest_main)
# Turns libstdc++ precondition violations (e.g. .front() on an empty string)
# into an abort instead of silent UB, so tests can actually catch them.
# The prosched exe itself is built with this too (see target_compile_options
# above) - without it here, the test binary is LESS strict than the shipped
# program and such bugs pass CI unnoticed.
target_compile_definitions(prosched_tests PRIVATE _GLIBCXX_ASSERTIONS)

include(GoogleTest)
# WORKING_DIRECTORY set to source root so fromFile() can resolve "prosched/config.txt"
gtest_discover_tests(prosched_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
