# Assembles a portable Windows runtime folder next to SubCue.exe.
# Invoked as: cmake -D... -P cmake/PackageWindows.cmake
#
# Required -D variables:
#   SUBCUE_EXE, SUBCUE_INFERENCE_EXE, SUBCUE_PYTHON_ROOT,
#   SUBCUE_INFERENCE_SITE_PACKAGES, SUBCUE_SOURCE_DIR, SUBCUE_PACKAGE_DIR, QT_BIN_DIR,
#   FFMPEG_BIN_DIR, FFMPEG_SHARE_DIR, SUBCUE_BUILD_TYPE

cmake_minimum_required(VERSION 3.28)

foreach(_var IN ITEMS
    SUBCUE_EXE SUBCUE_INFERENCE_EXE SUBCUE_PYTHON_ROOT SUBCUE_INFERENCE_SITE_PACKAGES
    SUBCUE_SOURCE_DIR SUBCUE_PACKAGE_DIR QT_BIN_DIR
    FFMPEG_BIN_DIR FFMPEG_SHARE_DIR SUBCUE_BUILD_TYPE)
    if(NOT ${_var})
        message(FATAL_ERROR "PackageWindows.cmake missing ${_var}")
    endif()
endforeach()

if(NOT EXISTS "${SUBCUE_EXE}")
    message(FATAL_ERROR "SubCue executable not found: ${SUBCUE_EXE}")
endif()

set(_windeployqt "${QT_BIN_DIR}/windeployqt.exe")
if(NOT EXISTS "${_windeployqt}")
    set(_windeployqt "${QT_BIN_DIR}/windeployqt6.exe")
endif()
if(NOT EXISTS "${_windeployqt}")
    message(FATAL_ERROR "windeployqt not found next to Qt: ${QT_BIN_DIR}")
endif()

get_filename_component(_qt_prefix "${QT_BIN_DIR}" DIRECTORY)
set(_qt_qml "${_qt_prefix}/qml")
set(_qt_plugins "${_qt_prefix}/plugins")

file(REMOVE_RECURSE "${SUBCUE_PACKAGE_DIR}")
file(MAKE_DIRECTORY "${SUBCUE_PACKAGE_DIR}")
file(COPY "${SUBCUE_EXE}" DESTINATION "${SUBCUE_PACKAGE_DIR}")
set(_inference_dir "${SUBCUE_PACKAGE_DIR}/inference")
set(_inference_runtime "${_inference_dir}/runtime")
file(MAKE_DIRECTORY "${_inference_runtime}/Lib")
file(COPY "${SUBCUE_INFERENCE_EXE}" DESTINATION "${_inference_dir}")
file(COPY "${SUBCUE_SOURCE_DIR}/tools/asr_python_worker.py" DESTINATION "${_inference_dir}")
file(COPY "${SUBCUE_PYTHON_ROOT}/python312.dll" "${SUBCUE_PYTHON_ROOT}/python3.dll"
    DESTINATION "${_inference_dir}")
file(COPY "${SUBCUE_PYTHON_ROOT}/python312.dll" "${SUBCUE_PYTHON_ROOT}/python3.dll"
    DESTINATION "${_inference_runtime}")
file(COPY "${SUBCUE_PYTHON_ROOT}/DLLs" DESTINATION "${_inference_runtime}")
foreach(_runtime_dll IN ITEMS vcruntime140.dll vcruntime140_1.dll)
    if(EXISTS "${SUBCUE_PYTHON_ROOT}/${_runtime_dll}")
        file(COPY "${SUBCUE_PYTHON_ROOT}/${_runtime_dll}" DESTINATION "${_inference_dir}")
    endif()
endforeach()
file(COPY "${SUBCUE_PYTHON_ROOT}/Lib/" DESTINATION "${_inference_runtime}/Lib"
    PATTERN "site-packages" EXCLUDE PATTERN "venv" EXCLUDE
    PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE
    PATTERN "ensurepip" EXCLUDE PATTERN "idlelib" EXCLUDE
    PATTERN "lib2to3" EXCLUDE PATTERN "turtledemo" EXCLUDE)
file(GLOB_RECURSE _development_files LIST_DIRECTORIES false
    "${SUBCUE_INFERENCE_SITE_PACKAGES}/*.lib")
set(_development_bytes 0)
foreach(_file IN LISTS _development_files)
    file(SIZE "${_file}" _file_size)
    math(EXPR _development_bytes "${_development_bytes} + ${_file_size}")
endforeach()
file(COPY "${SUBCUE_INFERENCE_SITE_PACKAGES}/" DESTINATION "${_inference_runtime}/Lib/site-packages"
    PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE PATTERN "tests" EXCLUDE
    PATTERN "test" EXCLUDE PATTERN "*.lib" EXCLUDE PATTERN "*.pdb" EXCLUDE)
# Torch C++ 头文件只供扩展编译使用，推理运行时无需携带。
file(REMOVE_RECURSE "${_inference_runtime}/Lib/site-packages/torch/include")
# Worker 运行时未 import 的转移依赖（qwen-asr 演示 UI / numba JIT、已移除的学习框架），不打进正式包。
foreach(_unused_python IN ITEMS gradio gradio_client llvmlite numba xgboost sklearn scikit_learn)
    file(REMOVE_RECURSE "${_inference_runtime}/Lib/site-packages/${_unused_python}")
    file(GLOB _unused_meta LIST_DIRECTORIES true
        "${_inference_runtime}/Lib/site-packages/${_unused_python}-*"
        "${_inference_runtime}/Lib/site-packages/${_unused_python}.*")
    foreach(_meta IN LISTS _unused_meta)
        file(REMOVE_RECURSE "${_meta}")
    endforeach()
endforeach()

set(_system_root "$ENV{SystemRoot}")
if(NOT _system_root)
    set(_system_root "C:/Windows")
endif()
set(ENV{PATH} "${QT_BIN_DIR};${_system_root}/System32")
unset(ENV{PYTHONPATH})
unset(ENV{PYTHONHOME})
unset(ENV{VIRTUAL_ENV})
unset(ENV{QML_IMPORT_PATH})
unset(ENV{QML2_IMPORT_PATH})
unset(ENV{QT_PLUGIN_PATH})

set(_windeploy_args
    --compiler-runtime
    --no-translations
    --force
    --qmldir "${SUBCUE_SOURCE_DIR}/src/app/qml"
)
if(EXISTS "${_qt_qml}")
    list(APPEND _windeploy_args --qmlimport "${_qt_qml}")
endif()
if(SUBCUE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND _windeploy_args --debug)
else()
    list(APPEND _windeploy_args --release)
endif()

execute_process(
    COMMAND "${_windeployqt}" ${_windeploy_args} "SubCue.exe"
    WORKING_DIRECTORY "${SUBCUE_PACKAGE_DIR}"
    RESULT_VARIABLE _windeploy_result
    OUTPUT_VARIABLE _windeploy_out
    ERROR_VARIABLE _windeploy_err
)
if(NOT _windeploy_result EQUAL 0)
    message(FATAL_ERROR
        "windeployqt failed (${_windeploy_result})\n${_windeploy_out}\n${_windeploy_err}")
endif()

# 应用启动时固定使用 Basic 风格，删除 windeployqt 额外收集的其他控件主题。
foreach(_style IN ITEMS FluentWinUI3 Fusion Imagine Material Universal Windows)
    file(REMOVE_RECURSE "${SUBCUE_PACKAGE_DIR}/qml/QtQuick/Controls/${_style}")
endforeach()
foreach(_style_dll IN ITEMS
    Qt6QuickControls2FluentWinUI3StyleImpl.dll
    Qt6QuickControls2Fusion.dll
    Qt6QuickControls2FusionStyleImpl.dll
    Qt6QuickControls2Imagine.dll
    Qt6QuickControls2ImagineStyleImpl.dll
    Qt6QuickControls2Material.dll
    Qt6QuickControls2MaterialStyleImpl.dll
    Qt6QuickControls2Universal.dll
    Qt6QuickControls2UniversalStyleImpl.dll
    Qt6QuickControls2WindowsStyleImpl.dll)
    file(REMOVE "${SUBCUE_PACKAGE_DIR}/${_style_dll}")
endforeach()

# 已部署具体 VC Runtime DLL，无需再重复携带完整安装器。
file(REMOVE "${SUBCUE_PACKAGE_DIR}/vc_redist.x64.exe")

set(_ffmpeg_runtime
    avcodec-63.dll
    avformat-63.dll
    avutil-61.dll
    swresample-7.dll
    swscale-10.dll
)
foreach(_dll IN LISTS _ffmpeg_runtime)
    if(NOT EXISTS "${FFMPEG_BIN_DIR}/${_dll}")
        message(FATAL_ERROR "Missing FFmpeg runtime DLL: ${FFMPEG_BIN_DIR}/${_dll}")
    endif()
    file(COPY "${FFMPEG_BIN_DIR}/${_dll}" DESTINATION "${SUBCUE_PACKAGE_DIR}")
endforeach()

if(CUDA_BIN_DIR)
    foreach(_dll IN ITEMS cublas64_13.dll cublasLt64_13.dll cudart64_13.dll)
        if(NOT EXISTS "${CUDA_BIN_DIR}/x64/${_dll}")
            message(FATAL_ERROR "Missing CUDA runtime DLL: ${CUDA_BIN_DIR}/x64/${_dll}")
        endif()
        file(COPY "${CUDA_BIN_DIR}/x64/${_dll}" DESTINATION "${SUBCUE_PACKAGE_DIR}")
    endforeach()
endif()

function(_subcue_copy_config_plugins plugin_dir dest_dir)
    if(NOT EXISTS "${plugin_dir}")
        return()
    endif()
    file(GLOB _matches "${plugin_dir}/*.dll")
    if(NOT _matches)
        return()
    endif()
    file(MAKE_DIRECTORY "${dest_dir}")
    foreach(_file IN LISTS _matches)
        get_filename_component(_name "${_file}" NAME)
        if(SUBCUE_BUILD_TYPE STREQUAL "Debug")
            if(_name MATCHES "d\\.dll$")
                file(COPY "${_file}" DESTINATION "${dest_dir}")
            endif()
        else()
            if(NOT _name MATCHES "d\\.dll$")
                file(COPY "${_file}" DESTINATION "${dest_dir}")
            endif()
        endif()
    endforeach()
endfunction()

set(_platforms_dest "${SUBCUE_PACKAGE_DIR}/platforms")
if(NOT EXISTS "${_platforms_dest}")
    set(_platforms_dest "${SUBCUE_PACKAGE_DIR}/plugins/platforms")
endif()
_subcue_copy_config_plugins("${_qt_plugins}/platforms" "${_platforms_dest}")

set(_tls_dest "${SUBCUE_PACKAGE_DIR}/tls")
if(EXISTS "${SUBCUE_PACKAGE_DIR}/plugins/tls")
    set(_tls_dest "${SUBCUE_PACKAGE_DIR}/plugins/tls")
endif()
_subcue_copy_config_plugins("${_qt_plugins}/tls" "${_tls_dest}")

set(_sql_dest "${SUBCUE_PACKAGE_DIR}/sqldrivers")
if(EXISTS "${SUBCUE_PACKAGE_DIR}/plugins/sqldrivers")
    set(_sql_dest "${SUBCUE_PACKAGE_DIR}/plugins/sqldrivers")
endif()
_subcue_copy_config_plugins("${_qt_plugins}/sqldrivers" "${_sql_dest}")

set(_license_dir "${SUBCUE_PACKAGE_DIR}/licenses")
file(MAKE_DIRECTORY "${_license_dir}")
file(COPY "${SUBCUE_SOURCE_DIR}/licenses/NOTICE.txt" DESTINATION "${_license_dir}")
file(COPY "${SUBCUE_SOURCE_DIR}/docs/third-party-licenses.md" DESTINATION "${_license_dir}")
file(COPY "${SUBCUE_SOURCE_DIR}/licenses/qt" DESTINATION "${_license_dir}")
if(EXISTS "${FFMPEG_SHARE_DIR}/copyright")
    file(MAKE_DIRECTORY "${_license_dir}/FFmpeg")
    file(COPY "${FFMPEG_SHARE_DIR}/copyright" DESTINATION "${_license_dir}/FFmpeg")
endif()

file(WRITE "${SUBCUE_PACKAGE_DIR}/README.txt"
"SubCue Windows x64
==================

Run SubCue.exe. Qt, FFmpeg, and the MSVC runtime are in this folder.
Qwen3-ASR and Fun-ASR require a compatible local Python CUDA environment.

Third-party licenses are in licenses/.
")

if(NOT EXISTS "${SUBCUE_PACKAGE_DIR}/qt.conf")
    file(WRITE "${SUBCUE_PACKAGE_DIR}/qt.conf" "[Paths]\nPrefix=.\n")
endif()

file(REMOVE_RECURSE "${SUBCUE_PACKAGE_DIR}/qmltooling")

set(_crt_ok FALSE)
foreach(_crt IN ITEMS vcruntime140.dll vcruntime140d.dll VCRUNTIME140.dll VCRUNTIME140D.dll)
    if(EXISTS "${SUBCUE_PACKAGE_DIR}/${_crt}")
        set(_crt_ok TRUE)
    endif()
endforeach()
if(NOT _crt_ok)
    set(_redist_root "$ENV{VCToolsRedistDir}")
    if(NOT _redist_root)
        file(GLOB _redist_candidates
            "C:/Program Files/Microsoft Visual Studio/*/Community/VC/Redist/MSVC/*")
        list(SORT _redist_candidates)
        list(REVERSE _redist_candidates)
        foreach(_candidate IN LISTS _redist_candidates)
            if(IS_DIRECTORY "${_candidate}/x64")
                set(_redist_root "${_candidate}")
                break()
            endif()
        endforeach()
    endif()
    if(_redist_root)
        file(GLOB _crt_dlls "${_redist_root}/x64/Microsoft.VC*.CRT/*.dll")
        foreach(_dll IN LISTS _crt_dlls)
            file(COPY "${_dll}" DESTINATION "${SUBCUE_PACKAGE_DIR}")
        endforeach()
    endif()
    foreach(_crt IN ITEMS vcruntime140.dll vcruntime140d.dll VCRUNTIME140.dll VCRUNTIME140D.dll)
        if(EXISTS "${SUBCUE_PACKAGE_DIR}/${_crt}")
            set(_crt_ok TRUE)
        endif()
    endforeach()
endif()
if(NOT _crt_ok)
    message(FATAL_ERROR "Package missing MSVC runtime DLL")
endif()

file(GLOB_RECURSE _package_files LIST_DIRECTORIES false "${SUBCUE_PACKAGE_DIR}/*")
set(_forbidden_hits "")
foreach(_path IN LISTS _package_files)
    get_filename_component(_name "${_path}" NAME)
    string(TOLOWER "${_name}" _lower)
    file(RELATIVE_PATH _relative "${SUBCUE_PACKAGE_DIR}" "${_path}")
    string(REPLACE "\\" "/" _relative "${_relative}")
    if((_lower MATCHES "^python" AND NOT _relative MATCHES "^inference/")
        OR _lower STREQUAL "python.exe"
        OR (_lower MATCHES "avdevice" AND NOT _relative MATCHES "^inference/")
        OR _lower STREQUAL "ffmpeg.exe"
        OR _lower STREQUAL "ffprobe.exe"
        OR _lower STREQUAL "ffplay.exe"
        OR (_lower MATCHES "^avfilter" AND NOT _relative MATCHES "^inference/")
        OR _lower MATCHES "pyside")
        string(APPEND _forbidden_hits "${_path}\n")
    endif()
endforeach()
if(_forbidden_hits)
    message(FATAL_ERROR "Package contains forbidden runtime files:\n${_forbidden_hits}")
endif()

set(_required
    "${SUBCUE_PACKAGE_DIR}/SubCue.exe"
    "${SUBCUE_PACKAGE_DIR}/inference/SubCueInference.exe"
    "${SUBCUE_PACKAGE_DIR}/inference/python312.dll"
    "${SUBCUE_PACKAGE_DIR}/inference/runtime/python312.dll"
    "${SUBCUE_PACKAGE_DIR}/inference/asr_python_worker.py"
    "${SUBCUE_PACKAGE_DIR}/avcodec-63.dll"
    "${SUBCUE_PACKAGE_DIR}/avformat-63.dll"
    "${SUBCUE_PACKAGE_DIR}/avutil-61.dll"
    "${SUBCUE_PACKAGE_DIR}/swresample-7.dll"
    "${SUBCUE_PACKAGE_DIR}/swscale-10.dll"
    "${SUBCUE_PACKAGE_DIR}/licenses/NOTICE.txt"
    "${SUBCUE_PACKAGE_DIR}/licenses/qt/LICENSE.LGPLv3"
)
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "Package missing required file: ${_file}")
    endif()
endforeach()

set(_qt_core_ok FALSE)
foreach(_qt_core IN ITEMS Qt6Core.dll Qt6Cored.dll)
    if(EXISTS "${SUBCUE_PACKAGE_DIR}/${_qt_core}")
        set(_qt_core_ok TRUE)
    endif()
endforeach()
if(NOT _qt_core_ok)
    message(FATAL_ERROR "Package missing Qt6Core runtime DLL")
endif()

set(_platform_ok FALSE)
foreach(_platform IN ITEMS
    "${SUBCUE_PACKAGE_DIR}/platforms/qwindows.dll"
    "${SUBCUE_PACKAGE_DIR}/platforms/qwindowsd.dll"
    "${SUBCUE_PACKAGE_DIR}/plugins/platforms/qwindows.dll"
    "${SUBCUE_PACKAGE_DIR}/plugins/platforms/qwindowsd.dll")
    if(EXISTS "${_platform}")
        set(_platform_ok TRUE)
    endif()
endforeach()
if(NOT _platform_ok)
    message(FATAL_ERROR "Package missing qwindows platform plugin")
endif()

set(_sqlite_ok FALSE)
foreach(_sqlite IN ITEMS
    "${SUBCUE_PACKAGE_DIR}/sqldrivers/qsqlite.dll"
    "${SUBCUE_PACKAGE_DIR}/sqldrivers/qsqlited.dll"
    "${SUBCUE_PACKAGE_DIR}/plugins/sqldrivers/qsqlite.dll"
    "${SUBCUE_PACKAGE_DIR}/plugins/sqldrivers/qsqlited.dll")
    if(EXISTS "${_sqlite}")
        set(_sqlite_ok TRUE)
    endif()
endforeach()
if(NOT _sqlite_ok)
    message(FATAL_ERROR "Package missing QSQLITE driver plugin")
endif()

set(_manifest "")
foreach(_path IN LISTS _package_files)
    file(RELATIVE_PATH _rel "${SUBCUE_PACKAGE_DIR}" "${_path}")
    string(APPEND _manifest "${_rel}\n")
endforeach()
file(WRITE "${SUBCUE_PACKAGE_DIR}/package-manifest.txt" "${_manifest}")
set(_package_bytes 0)
foreach(_path IN LISTS _package_files)
    file(SIZE "${_path}" _file_size)
    math(EXPR _package_bytes "${_package_bytes} + ${_file_size}")
endforeach()
file(WRITE "${SUBCUE_PACKAGE_DIR}/package-size.txt"
    "Package bytes: ${_package_bytes}\nExcluded development library bytes: ${_development_bytes}\n")
file(WRITE "${SUBCUE_PACKAGE_DIR}/package.stamp" "SubCue ${SUBCUE_BUILD_TYPE} package\n")
# CUDA 便携包必须内置 PyTorch CUDA 与 CUDA 13 三件套，体积会显著超过 610 MiB 政策目标。
message(STATUS "Windows package ready: ${SUBCUE_PACKAGE_DIR} (${_package_bytes} bytes; ${_development_bytes} development library bytes excluded)")
