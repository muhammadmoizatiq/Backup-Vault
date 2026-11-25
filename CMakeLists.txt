cmake_minimum_required(VERSION 3.16)
project(BackupVault VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)

include_directories(${CMAKE_SOURCE_DIR}/include)

# Collect every .cpp under src/ (new files are picked up when you re-run CMake)
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

add_executable(BackupVault ${APP_SOURCES})
target_link_libraries(BackupVault PRIVATE Qt6::Widgets Qt6::Sql)

# ---- Tests (each tests/test_*.cpp becomes its own executable) ----
# Tests link every source except main.cpp, so keep main() only in src/main.cpp.
enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
set(LIB_SOURCES ${APP_SOURCES})
list(FILTER LIB_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")
foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src} ${LIB_SOURCES})
    target_link_libraries(${test_name} PRIVATE Qt6::Widgets Qt6::Sql)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
