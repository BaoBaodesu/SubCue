# Assembles a portable Windows runtime folder next to SubCue.exe.
# Invoked as: cmake -D... -P cmake/PackageWindows.cmake
#
# Required -D variables:
#   SUBCUE_EXE, SUBCUE_SOURCE_DIR, SUBCUE_PACKAGE_DIR, QT_BIN_DIR,
#   FFMPEG_BIN_DIR, FFMPEG_SHARE_DIR, SUBCUE_BUILD_TYPE

cmake_minimum_required(VERSION 3.28)

foreach(_var IN ITEMS
    SUBCUE_EXE SUBCUE_SOURCE_DIR SUBCUE_PACKAGE_DIR QT_BIN_DIR
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
file(COPY "${SUBCUE_SOURCE_DIR}/tools/asr_python_worker.py" DESTINATION "${SUBCUE_PACKAGE_DIR}")

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

set(_license_dir "${SUBCUE_PACKAGE_DIR}/licenses")
file(MAKE_DIRECTORY "${_license_dir}")
file(COPY "${SUBCUE_SOURCE_DIR}/licenses/NOTICE.txt" DESTINATION "${_license_dir}")
file(COPY "${SUBCUE_SOURCE_DIR}/docs/third-party-licenses.md" DESTINATION "${_license_dir}")
file(COPY "${SUBCUE_SOURCE_DIR}/licenses/qt" DESTINATION "${_license_dir}")
file(COPY "${SUBCUE_SOURCE_DIR}/licenses/whisper.cpp" DESTINATION "${_license_dir}")
if(EXISTS "${FFMPEG_SHARE_DIR}/copyright")
    file(MAKE_DIRECTORY "${_license_dir}/FFmpeg")
    file(COPY "${FFMPEG_SHARE_DIR}/copyright" DESTINATION "${_license_dir}/FFmpeg")
endif()

file(WRITE "${SUBCUE_PACKAGE_DIR}/README.txt"
"SubCue Windows x64
==================

Run SubCue.exe. Qt, FFmpeg, and the MSVC runtime are in this folder.
Whisper runs without Python. Qwen3-ASR and Fun-ASR require a compatible local Python CUDA environment.

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
    if(_lower MATCHES "^python"
        OR _lower STREQUAL "python.exe"
        OR _lower MATCHES "avdevice"
        OR _lower STREQUAL "ffmpeg.exe"
        OR _lower STREQUAL "ffprobe.exe"
        OR _lower STREQUAL "ffplay.exe"
        OR _lower MATCHES "^avfilter"
        OR _lower MATCHES "pyside")
        string(APPEND _forbidden_hits "${_path}\n")
    endif()
endforeach()
if(_forbidden_hits)
    message(FATAL_ERROR "Package contains forbidden runtime files:\n${_forbidden_hits}")
endif()

set(_required
    "${SUBCUE_PACKAGE_DIR}/SubCue.exe"
    "${SUBCUE_PACKAGE_DIR}/avcodec-63.dll"
    "${SUBCUE_PACKAGE_DIR}/avformat-63.dll"
    "${SUBCUE_PACKAGE_DIR}/avutil-61.dll"
    "${SUBCUE_PACKAGE_DIR}/swresample-7.dll"
    "${SUBCUE_PACKAGE_DIR}/swscale-10.dll"
    "${SUBCUE_PACKAGE_DIR}/licenses/NOTICE.txt"
    "${SUBCUE_PACKAGE_DIR}/licenses/qt/LICENSE.LGPLv3"
    "${SUBCUE_PACKAGE_DIR}/licenses/whisper.cpp/LICENSE"
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

set(_manifest "")
foreach(_path IN LISTS _package_files)
    file(RELATIVE_PATH _rel "${SUBCUE_PACKAGE_DIR}" "${_path}")
    string(APPEND _manifest "${_rel}\n")
endforeach()
file(WRITE "${SUBCUE_PACKAGE_DIR}/package-manifest.txt" "${_manifest}")
file(WRITE "${SUBCUE_PACKAGE_DIR}/package.stamp" "SubCue ${SUBCUE_BUILD_TYPE} package\n")
message(STATUS "Windows package ready: ${SUBCUE_PACKAGE_DIR}")
