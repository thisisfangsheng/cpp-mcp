# mcp-cpp-sdk: header-only C++ MCP server SDK + 示例服务
cmake_minimum_required(VERSION 3.20)
project(mcp_cpp_sdk VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MCP_BUILD_EXAMPLE "Build the example MCP server" ON)
option(MCP_STATIC_LINK   "Statically link the example binary (musl/static)" OFF)

# ---------------------------------------------------------------------------
# 依赖：Boost（仅头文件：Beast/Asio）+ nlohmann/json（header-only）
# C-4：find_package 优先，FetchContent 兜底。
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

set(Boost_NO_BOOST_CMAKE OFF)
find_package(Boost 1.74 REQUIRED)  # 无 components：仅头文件

find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json not found locally, fetching via FetchContent ...")
  include(FetchContent)
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# SDK：INTERFACE 库，100% header-only（NFR-1），不链接任何 Boost 编译库（NFR-2）
# ---------------------------------------------------------------------------
add_library(mcp_sdk INTERFACE)
target_include_directories(mcp_sdk INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                             $<INSTALL_INTERFACE:include>)
target_compile_features(mcp_sdk INTERFACE cxx_std_20)
if(TARGET Boost::headers)
  target_link_libraries(mcp_sdk INTERFACE Boost::headers)
else()
  target_include_directories(mcp_sdk INTERFACE ${Boost_INCLUDE_DIRS})
endif()
target_link_libraries(mcp_sdk INTERFACE nlohmann_json::nlohmann_json Threads::Threads)

if(MSVC)
  target_compile_options(mcp_sdk INTERFACE /W4)
else()
  target_compile_options(mcp_sdk INTERFACE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# 示例 server：唯一的编译产物
# ---------------------------------------------------------------------------
if(MCP_BUILD_EXAMPLE)
  add_executable(mcp_server src/main.cpp)
  target_link_libraries(mcp_server PRIVATE mcp_sdk)
  if(MCP_STATIC_LINK)
    target_link_options(mcp_server PRIVATE -static)
  endif()
  install(TARGETS mcp_server RUNTIME DESTINATION bin)
endif()

install(DIRECTORY include/ DESTINATION include)
