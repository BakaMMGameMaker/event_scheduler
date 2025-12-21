cmake_minimum_required(VERSION 3.16)

project(event_scheduler
    VERSION 0.1
    LANGUAGES CXX
)

# ========= C++ 标准 =========
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ========= 编译选项 =========
if (MSVC)
    add_compile_options(
        /W4 
        /permissive-
        /utf-8)
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
    )
endif()

# ========= 可执行文件 =========
add_executable(event_scheduler_demo
    example.cpp
)

# ========= 头文件路径 =========
target_include_directories(event_scheduler_demo
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ========= 调试信息（可选） =========
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(event_scheduler_demo PRIVATE ES_DEBUG)
endif()
