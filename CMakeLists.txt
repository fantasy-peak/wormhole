cmake_minimum_required(VERSION 3.23.0)

project(wormhole)

set(CMAKE_CXX_STANDARD 20)
set(OPTFLAGS -gdwarf-2)
set(CMAKE_CXX_FLAGS "-fcoroutines")
set(WARNINGS -Wall -Wno-missing-field-initializers -Wno-ignored-qualifiers)
add_compile_options(${OPTFLAGS} ${WARNINGS})

if(NOT EXISTS "${CMAKE_BINARY_DIR}/xrepo.cmake")
    message(STATUS "Downloading xrepo.cmake from https://github.com/xmake-io/xrepo-cmake/")
    # mirror https://cdn.jsdelivr.net/gh/xmake-io/xrepo-cmake@main/xrepo.cmake
    file(DOWNLOAD "https://raw.githubusercontent.com/xmake-io/xrepo-cmake/main/xrepo.cmake"
                  "${CMAKE_BINARY_DIR}/xrepo.cmake"
                  TLS_VERIFY ON)
endif()

set(XREPO_PACKAGE_VERBOSE ON)
set(XREPO_BOOTSTRAP_XMAKE OFF)
set(XREPO_BUILD_PARALLEL_JOBS 12)
set(XMAKE_RELEASE_LATEST v2.6.8)

include(${CMAKE_BINARY_DIR}/xrepo.cmake)

if(NOT EXISTS "$ENV{HOME}/.xmake/repositories/my_private_repo/xmake.lua")
    message(STATUS "add my_private_repo https://github.com/fantasy-peak/xmake-repo.git")
    execute_process(COMMAND xmake repo -a -g my_private_repo https://github.com/fantasy-peak/xmake-repo.git)
endif()
#----------------------------------------------------------------------------------
xrepo_package("openssl")
xrepo_package("boost" CONFIGS "all = true")
xrepo_package("async_simple main")
xrepo_package("fmt")
xrepo_package("spdlog")
xrepo_package("yaml_cpp_struct main" DEPS "magic_enum" "visit_struct" "yaml-cpp")
#-----------------------------------------------------------------------------------
include_directories(include)
add_executable(wormhole_client src/wormhole_client.cpp)
xrepo_target_packages(wormhole_client PUBLIC yaml_cpp_struct openssl boost fmt spdlog async_simple NO_LINK_LIBRARIES)
target_link_libraries(wormhole_client PUBLIC yaml-cpp async_simple fmt -pthread dl ssl crypto aio boost_program_options-mt)

add_executable(wormhole_server src/wormhole_server.cpp)
xrepo_target_packages(wormhole_server PUBLIC yaml_cpp_struct openssl boost fmt spdlog async_simple NO_LINK_LIBRARIES)
target_link_libraries(wormhole_server PUBLIC yaml-cpp async_simple fmt -pthread dl ssl crypto aio boost_program_options-mt)

