cmake_minimum_required(VERSION 3.16)

# clipconv — clipparse と psdparse の**両方を参照する側**のサンプル。
#
# 単体でビルドする:
#   cmake -S examples/clipconv -B build-conv -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-conv --config Release
#
# 依存リポジトリ (clipparse / psdparse) の解決順:
#   1. ローカルにあればそれを使う
#      (既定は CLIPPARSE_DIR = このサンプルを含むリポジトリ、
#       PSDPARSE_DIR = その隣の ../psdparse。-D で明示指定もできる)
#   2. 無ければ GitHub から FetchContent で取得する
#      (取得させたくない場合は -DCLIPCONV_FETCH_MISSING=OFF。その場合は
#       パスが見つからなければエラーで止まる)
#
# どちらのライブラリも外部パッケージマネージャを要求しない
# (zlib と sqlite3 は FetchContent がソースから取る)。zlib は両方が
# 宣言するが、FetchContent は同名の宣言を 1 回しか populate しないので
# 実体はひとつになる。

project(clipconv LANGUAGES C CXX)

add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")

include(FetchContent)

set(CLIPPARSE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "clipparse リポジトリの場所 (無ければ GitHub から取得)")
set(PSDPARSE_DIR "${CLIPPARSE_DIR}/../psdparse" CACHE PATH
    "psdparse リポジトリの場所 (無ければ GitHub から取得)")

option(CLIPCONV_FETCH_MISSING
    "ローカルに見つからない依存リポジトリを GitHub から取得する" ON)
set(CLIPPARSE_GIT_REPOSITORY "https://github.com/wamsoft/clipparse.git" CACHE STRING
    "clipparse の取得元")
set(PSDPARSE_GIT_REPOSITORY "https://github.com/wamsoft/psdparse.git" CACHE STRING
    "psdparse の取得元")
set(CLIPPARSE_GIT_TAG "master" CACHE STRING "取得する clipparse の branch / tag")
set(PSDPARSE_GIT_TAG "master" CACHE STRING "取得する psdparse の branch / tag")

# ローカルの checkout があればそれを add_subdirectory し、無ければ取得する。
# macro なので FetchContent が定義する変数はこのディレクトリスコープに残る。
macro(clipconv_use_repo name dir marker repo tag binary_dir)
    if(EXISTS "${dir}/${marker}")
        message(STATUS "clipconv: ${name} はローカルのものを使う — ${dir}")
        add_subdirectory("${dir}" "${binary_dir}")
    elseif(CLIPCONV_FETCH_MISSING)
        message(STATUS
            "clipconv: ${name} がローカルに見つからない (${dir}) — "
            "${repo} (${tag}) から取得する")
        FetchContent_Declare(${name}
            GIT_REPOSITORY "${repo}"
            GIT_TAG "${tag}"
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(${name})
    else()
        message(FATAL_ERROR
            "見つからない: ${dir}/${marker}\n"
            "-DCLIPPARSE_DIR= / -DPSDPARSE_DIR= でリポジトリの場所を指定するか、"
            "-DCLIPCONV_FETCH_MISSING=ON で GitHub から取得してください。")
    endif()
endmacro()

set(CLIPPARSE_BUILD_CLI OFF CACHE BOOL "" FORCE)   # このサンプルには不要
clipconv_use_repo(clipparse "${CLIPPARSE_DIR}" "clipparse/clipfile.h"
    "${CLIPPARSE_GIT_REPOSITORY}" "${CLIPPARSE_GIT_TAG}" clipparse-build)
clipconv_use_repo(psdparse "${PSDPARSE_DIR}" "psdparse/psdparse.h"
    "${PSDPARSE_GIT_REPOSITORY}" "${PSDPARSE_GIT_TAG}" psdparse-build)

add_executable(clipconv clipconv.cpp)
target_link_libraries(clipconv PRIVATE clipparse psdparse)
target_compile_features(clipconv PRIVATE cxx_std_17)
if(MSVC)
    target_compile_definitions(clipconv PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
