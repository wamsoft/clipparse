cmake_minimum_required(VERSION 3.16)

# pybind11 は pip 経由 (`pip install pybind11`) で導入する。
# Python は modern FindPython で探す (psdparse/python/CMakeLists.txt と同じ流儀)。
if(NOT TARGET pybind11::module)
    set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)

    if(NOT Python3_EXECUTABLE)
        find_program(_clip_python NAMES python python3 REQUIRED)
        set(Python3_EXECUTABLE "${_clip_python}" CACHE FILEPATH "")
    endif()
    # 拡張モジュールに要るのは Development.Module のみ。full Development を
    # 要求すると libpython の無い環境で落ちる。
    find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)

    if(NOT pybind11_DIR)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
            OUTPUT_VARIABLE _pybind11_cmakedir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _pybind11_rv)
        if(NOT _pybind11_rv EQUAL 0)
            message(FATAL_ERROR
                "Could not locate pybind11 via '${Python3_EXECUTABLE} -m pybind11 --cmakedir'. "
                "Install with: pip install pybind11")
        endif()
        set(pybind11_DIR "${_pybind11_cmakedir}" CACHE PATH "pybind11 cmake dir")
    endif()
    find_package(pybind11 CONFIG REQUIRED)
endif()

pybind11_add_module(clipparse_py MODULE clipparse_module.cpp)
set_target_properties(clipparse_py PROPERTIES OUTPUT_NAME "clipparse")
target_link_libraries(clipparse_py PRIVATE clipparse)

install(TARGETS clipparse_py
        LIBRARY DESTINATION "."
        RUNTIME DESTINATION ".")

# 検証・修復・変換ツールを `clipparse_tools` パッケージとして同梱する。
# **ソースの正本は tools/** (仕様検証の参照実装) で、ここでは取り込むだけ。
# 依存が要るものも入れておき、ライブラリが見つかったときだけ有効になる
# (extras 方式。pyproject.toml の [project.optional-dependencies]):
#   依存なし          : clip_probe / clip_validate / clip_doctor / clip_export
#   [image] numpy+PIL : clip_write の setpixels / addlayer (clip_encode / clip_build)
#   [psd]   psdparse  : clip_to_psd / psd_to_clip / imgdoc / clip_lazy_demo
install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/clipparse_tools/__init__.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/clip_probe.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/clip_write.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/clip_validate.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/clip_doctor.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/clip_export.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/clip_encode.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/clip_build.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/clip_lazy_demo.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/imgdoc.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/clip_to_psd.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/psd_to_clip.py"
        DESTINATION clipparse_tools)

# psd-to-clip の雛形 (テンプレート方式の種)。手元にあるときだけ同梱する
# — samples/ は gitignore なので、CI ビルドには入らない。その場合
# psd-to-clip は --template を要求する (案内を出して終了する)。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../samples/emptyimage.clip")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../samples/emptyimage.clip"
            DESTINATION clipparse_tools/data)
else()
    message(STATUS "clipparse_tools: samples/emptyimage.clip not found — "
                   "psd-to-clip will require --template")
endif()
