cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/utf-8>")
    add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>")
    add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")
    project(clipparse LANGUAGES C CXX)
endif()

# Python バインディング (オプション)
option(CLIPPARSE_BUILD_PYTHON "Build pybind11 Python bindings" OFF)
if(CLIPPARSE_BUILD_PYTHON)
    # 拡張モジュールは静的ライブラリを .pyd へリンクするので PIC が要る
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

add_subdirectory(clipparse)

if(CLIPPARSE_BUILD_PYTHON)
    add_subdirectory(python)
endif()
