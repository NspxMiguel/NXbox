cmake_minimum_required(VERSION 3.20)
project(SwitchXboxPortTests LANGUAGES CXX)

enable_testing()
add_executable(demand-commit-tests demand_commit.cpp)
target_compile_features(demand-commit-tests PRIVATE cxx_std_20)
target_include_directories(demand-commit-tests PRIVATE ../../src)
if(MSVC)
  target_compile_options(demand-commit-tests PRIVATE /W4 /WX)
else()
  target_compile_options(demand-commit-tests PRIVATE -Wall -Wextra -Werror
                                                     -pedantic)
endif()
add_test(NAME demand-commit-policy COMMAND demand-commit-tests)
