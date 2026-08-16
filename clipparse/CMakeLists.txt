cmake_minimum_required(VERSION 3.16)

add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")

set(PROJECT_NAME clipparse)
set(PROJECT_VERSION 0.10)
project(${PROJECT_NAME} VERSION ${PROJECT_VERSION} LANGUAGES C CXX)

include(FetchContent)

# --- zlib: システムにあればそれを、無ければソースから取る (psdparse と同じ方針) ---
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    message(STATUS "clipparse: system zlib not found — fetching zlib 1.3.1 source")
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
    FetchContent_MakeAvailable(zlib)
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
    target_include_directories(zlibstatic PUBLIC
        $<BUILD_INTERFACE:${zlib_SOURCE_DIR}>
        $<BUILD_INTERFACE:${zlib_BINARY_DIR}>)
endif()

# --- sqlite3: amalgamation をソースから取って同梱ビルドする ---
# パッケージマネージャ不要。psdparse が zlib でやっているのと同じ扱い。
FetchContent_Declare(sqlite3_amalg
    URL https://sqlite.org/2024/sqlite-amalgamation-3450100.zip
    URL_HASH SHA256=5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4)
FetchContent_MakeAvailable(sqlite3_amalg)

add_library(sqlite3_static STATIC ${sqlite3_amalg_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3_static PUBLIC ${sqlite3_amalg_SOURCE_DIR})
target_compile_definitions(sqlite3_static PUBLIC
    SQLITE_ENABLE_DESERIALIZE=1     # sqlite3_deserialize (mmap 上をゼロコピー参照)
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0)
if(MSVC)
    target_compile_options(sqlite3_static PRIVATE /wd4996 /wd4267 /wd4244)
endif()

add_library(${PROJECT_NAME} STATIC
    clipbase.h
    clipfile.h
    clipfile.cpp
    clipcomposite.cpp
)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${PROJECT_NAME} PUBLIC ZLIB::ZLIB sqlite3_static)

option(CLIPPARSE_BUILD_CLI "Build clipparse standalone CLI" ON)
if(CLIPPARSE_BUILD_CLI)
    add_executable(clip_cli clip_cli.cpp)
    target_link_libraries(clip_cli PRIVATE ${PROJECT_NAME})
    if(MSVC)
        target_compile_definitions(clip_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
