cmake_minimum_required(VERSION 3.16)

# clipconv — clipparse と psdparse の**両方を参照する側**のサンプル。
#
# 単体でビルドする:
#   cmake -S examples/clipconv -B build-conv -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-conv --config Release
#
# 別の場所に置いてある場合は 2 つのパスを渡す:
#   cmake -S . -B build -DCLIPPARSE_DIR=... -DPSDPARSE_DIR=...
#
# どちらのライブラリも外部パッケージマネージャを要求しない
# (zlib と sqlite3 は FetchContent がソースから取る)。zlib は両方が
# 宣言するが、FetchContent は同名の宣言を 1 回しか populate しないので
# 実体はひとつになる。

project(clipconv LANGUAGES C CXX)

set(CLIPPARSE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "clipparse リポジトリの場所")
set(PSDPARSE_DIR "${CLIPPARSE_DIR}/../psdparse" CACHE PATH
    "psdparse リポジトリの場所")

foreach(dir "${CLIPPARSE_DIR}/clipparse/clipfile.h" "${PSDPARSE_DIR}/psdparse/psdparse.h")
    if(NOT EXISTS "${dir}")
        message(FATAL_ERROR
            "見つからない: ${dir}\n"
            "-DCLIPPARSE_DIR= / -DPSDPARSE_DIR= でリポジトリの場所を指定してください。")
    endif()
endforeach()

add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")

set(CLIPPARSE_BUILD_CLI OFF CACHE BOOL "" FORCE)   # このサンプルには不要
add_subdirectory("${CLIPPARSE_DIR}" clipparse-build)
add_subdirectory("${PSDPARSE_DIR}" psdparse-build)

add_executable(clipconv clipconv.cpp)
target_link_libraries(clipconv PRIVATE clipparse psdparse)
target_compile_features(clipconv PRIVATE cxx_std_17)
if(MSVC)
    target_compile_definitions(clipconv PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
