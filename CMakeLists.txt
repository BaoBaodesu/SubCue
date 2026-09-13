cmake_minimum_required(VERSION 3.28)

project(SubCue VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOGEN_PARALLEL 1)

option(SUBCUE_BUILD_APP "Build the Qt Quick application" ON)
option(SUBCUE_ENABLE_CUDA "Enable CUDA acceleration for local Whisper" OFF)
set(SUBCUE_WHISPER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/whisper.cpp"
    CACHE PATH "Path to the pinned whisper.cpp source tree")

set(CMAKE_PREFIX_PATH "C:/Qt/6.10.3/msvc2022_64")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Test)

set(SUBCUE_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows")
find_path(SUBCUE_FFMPEG_INCLUDE_DIR libavformat/avformat.h
    PATHS "${SUBCUE_FFMPEG_ROOT}/include"
    NO_DEFAULT_PATH
    REQUIRED
)
foreach(SUBCUE_FFMPEG_LIBRARY avcodec avformat avutil swresample swscale)
    string(TOUPPER "${SUBCUE_FFMPEG_LIBRARY}" SUBCUE_FFMPEG_LIBRARY_UPPER)
    find_library(SUBCUE_${SUBCUE_FFMPEG_LIBRARY_UPPER}_RELEASE
        NAMES "${SUBCUE_FFMPEG_LIBRARY}"
        PATHS "${SUBCUE_FFMPEG_ROOT}/lib"
        NO_DEFAULT_PATH
        REQUIRED
    )
    find_library(SUBCUE_${SUBCUE_FFMPEG_LIBRARY_UPPER}_DEBUG
        NAMES "${SUBCUE_FFMPEG_LIBRARY}"
        PATHS "${SUBCUE_FFMPEG_ROOT}/debug/lib"
        NO_DEFAULT_PATH
        REQUIRED
    )
endforeach()

add_library(SubCueFFmpeg INTERFACE)
target_include_directories(SubCueFFmpeg INTERFACE "${SUBCUE_FFMPEG_INCLUDE_DIR}")
foreach(SUBCUE_FFMPEG_LIBRARY avcodec avformat avutil swresample swscale)
    string(TOUPPER "${SUBCUE_FFMPEG_LIBRARY}" SUBCUE_FFMPEG_LIBRARY_UPPER)
    target_link_libraries(SubCueFFmpeg INTERFACE
        optimized "${SUBCUE_${SUBCUE_FFMPEG_LIBRARY_UPPER}_RELEASE}"
        debug "${SUBCUE_${SUBCUE_FFMPEG_LIBRARY_UPPER}_DEBUG}"
    )
endforeach()
add_library(SubCue::FFmpeg ALIAS SubCueFFmpeg)

set(SUBCUE_HAS_WHISPER OFF)
if(EXISTS "${SUBCUE_WHISPER_ROOT}/CMakeLists.txt")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static third-party libraries" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    if(SUBCUE_ENABLE_CUDA AND NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES native CACHE STRING "CUDA architectures for local Whisper")
    endif()
    set(GGML_CUDA ${SUBCUE_ENABLE_CUDA} CACHE BOOL "" FORCE)
    set(GGML_CUDA_FA OFF CACHE BOOL "" FORCE)
    set(GGML_CUDA_FORCE_CUBLAS ${SUBCUE_ENABLE_CUDA} CACHE BOOL "" FORCE)
    set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
    add_subdirectory("${SUBCUE_WHISPER_ROOT}" "${CMAKE_BINARY_DIR}/third_party/whisper.cpp"
        EXCLUDE_FROM_ALL)
    set(SUBCUE_HAS_WHISPER ON)
endif()

qt_standard_project_setup(REQUIRES 6.8)

include(CTest)

qt_add_library(SubCueCore STATIC
    src/core/alignment/alignment_engine.cpp
    src/core/alignment/alignment_engine.h
    src/core/alignment/alignment_pipeline.cpp
    src/core/alignment/alignment_pipeline.h
    src/core/alignment/alignment_preflight.cpp
    src/core/alignment/alignment_preflight.h
    src/core/alignment/alignment_result.h
    src/core/alignment/anchors.cpp
    src/core/alignment/anchors.h
    src/core/alignment/forced_align.cpp
    src/core/alignment/forced_align.h
    src/core/alignment/fuzz_ratio.cpp
    src/core/alignment/fuzz_ratio.h
    src/core/alignment/normalizer.cpp
    src/core/alignment/normalizer.h
    src/core/alignment/python_round.h
    src/core/alignment/transcript.cpp
    src/core/alignment/transcript.h
    src/core/alignment/unicode_filter_data.h
    src/core/ai/ai_provider.h
    src/core/ai/ai_provider_factory.cpp
    src/core/ai/ai_provider_factory.h
    src/core/ai/ai_types.h
    src/core/ai/openai_compatible_provider.cpp
    src/core/ai/openai_compatible_provider.h
    src/core/asr/asr_provider_factory.cpp
    src/core/asr/asr_provider_factory.h
    src/core/asr/asr_service.h
    src/core/asr/asr_types.h
    src/core/asr/audio_chunk_extractor.cpp
    src/core/asr/audio_chunk_extractor.h
    src/core/asr/audio_chunk_plan.cpp
    src/core/asr/audio_chunk_plan.h
    src/core/asr/dashscope_asr_service.cpp
    src/core/asr/dashscope_asr_service.h
    src/core/asr/http_client.cpp
    src/core/asr/http_client.h
    src/core/asr/local_python_asr_service.cpp
    src/core/asr/local_python_asr_service.h
    src/core/asr/model_downloader.cpp
    src/core/asr/model_downloader.h
    src/core/asr/whisper_cpp_service.cpp
    src/core/asr/whisper_cpp_service.h
    src/core/asr/whisper_engine.cpp
    src/core/asr/whisper_engine.h
    src/core/asr/whisper_model_catalog.cpp
    src/core/asr/whisper_model_catalog.h
    src/core/common/app_error.cpp
    src/core/common/app_error.h
    src/core/common/logging.cpp
    src/core/common/logging.h
    src/core/common/media_time.h
    src/core/common/provider_types.h
    src/core/cache/cache_key.cpp
    src/core/cache/cache_key.h
    src/core/cache/lru_cache.cpp
    src/core/cache/lru_cache.h
    src/core/cache/preview_cache.cpp
    src/core/cache/preview_cache.h
    src/core/media/audio_resampler.cpp
    src/core/media/audio_resampler.h
    src/core/media/bounded_queue.h
    src/core/media/decoder.cpp
    src/core/media/decoder.h
    src/core/media/demuxer.cpp
    src/core/media/demuxer.h
    src/core/media/ffmpeg_error.cpp
    src/core/media/ffmpeg_error.h
    src/core/media/ffmpeg_raii.h
    src/core/media/ffmpeg_time.h
    src/core/media/hw_accel.cpp
    src/core/media/hw_accel.h
    src/core/media/media_engine.cpp
    src/core/media/media_engine.h
    src/core/media/media_probe.cpp
    src/core/media/media_probe.h
    src/core/media/media_types.h
    src/core/media/thumbnail_loader.cpp
    src/core/media/thumbnail_loader.h
    src/core/media/video_frame_converter.cpp
    src/core/media/video_frame_converter.h
    src/core/playback/audio_clock.cpp
    src/core/playback/audio_clock.h
    src/core/playback/audio_device.cpp
    src/core/playback/audio_device.h
    src/core/playback/audio_output.cpp
    src/core/playback/audio_output.h
    src/core/playback/frame_index.cpp
    src/core/playback/frame_index.h
    src/core/playback/frame_stepper.cpp
    src/core/playback/frame_stepper.h
    src/core/playback/playback_engine.cpp
    src/core/playback/playback_engine.h
    src/core/playback/seek_controller.cpp
    src/core/playback/seek_controller.h
    src/core/playback/video_scheduler.cpp
    src/core/playback/video_scheduler.h
    src/core/waveform/waveform_generator.cpp
    src/core/waveform/waveform_generator.h
    src/core/waveform/waveform_pyramid.cpp
    src/core/waveform/waveform_pyramid.h
    src/core/project/project.h
    src/core/project/project_serializer.cpp
    src/core/project/project_serializer.h
    src/core/preview/preview_renderer.h
    src/core/preview/preview_renderer_factory.cpp
    src/core/preview/software_preview_renderer.cpp
    src/core/settings/settings_manager.cpp
    src/core/settings/settings_manager.h
    src/core/subtitle/ass_writer.cpp
    src/core/subtitle/ass_writer.h
    src/core/subtitle/srt_parser.cpp
    src/core/subtitle/srt_parser.h
    src/core/subtitle/srt_writer.cpp
    src/core/subtitle/srt_writer.h
    src/core/subtitle/subtitle.h
    src/core/subtitle/subtitle_command_manager.cpp
    src/core/subtitle/subtitle_command_manager.h
    src/core/subtitle/subtitle_document.cpp
    src/core/subtitle/subtitle_document.h
    src/core/subtitle/subtitle_model.cpp
    src/core/subtitle/subtitle_model.h
    src/core/subtitle/timecode.cpp
    src/core/subtitle/timecode.h
    src/core/subtitle/txt_importer.cpp
    src/core/subtitle/txt_importer.h
    src/core/timeline/snap_engine.cpp
    src/core/timeline/snap_engine.h
    src/core/timeline/timeline_editor.cpp
    src/core/timeline/timeline_editor.h
    src/core/timeline/timeline_scene.cpp
    src/core/timeline/timeline_scene.h
    src/core/timeline/timeline_viewport.cpp
    src/core/timeline/timeline_viewport.h
    src/platform/windows/dpapi_credential_store.cpp
    src/platform/windows/dpapi_credential_store.h
    src/platform/windows/wasapi_audio_device.cpp
    src/platform/windows/wasapi_audio_device.h
    src/platform/windows/d3d11_preview_adapter.cpp
    src/platform/windows/d3d11_preview_adapter.h
    src/core/settings/credential_store.h
)
target_include_directories(SubCueCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)
target_link_libraries(SubCueCore PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::Quick SubCue::FFmpeg)
target_compile_definitions(SubCueCore PUBLIC
    SUBCUE_PROJECT_WHISPER_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/models/whisper"
    SUBCUE_PROJECT_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/models"
)
if(SUBCUE_HAS_WHISPER)
    target_link_libraries(SubCueCore PRIVATE whisper)
    target_compile_definitions(SubCueCore PUBLIC SUBCUE_HAS_WHISPER=1)
    if(SUBCUE_ENABLE_CUDA)
        target_compile_definitions(SubCueCore PUBLIC SUBCUE_HAS_CUDA=1)
    endif()
endif()
if(WIN32)
    target_link_libraries(SubCueCore PRIVATE Crypt32 Advapi32 ole32 uuid d3d11 dxgi avrt)
endif()
target_compile_definitions(SubCueCore PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
add_library(SubCue::Core ALIAS SubCueCore)

if(SUBCUE_BUILD_APP)
    qt_add_executable(SubCue
        src/app/main.cpp
        src/app/app_controller.cpp
        src/app/app_controller.h
        src/app/application_context.cpp
        src/app/application_context.h
        src/app/native_theme.cpp
        src/app/native_theme.h
        src/app/ui_theme.h
    )
    set_source_files_properties(src/app/qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    qt_add_qml_module(SubCue
        URI SubCue
        VERSION 1.0
        NO_IMPORT_SCAN
        QML_FILES
            src/app/qml/Main.qml
            src/app/qml/Theme.qml
            src/app/qml/SettingsWindow.qml
            src/app/qml/AboutWindow.qml
            src/app/qml/MenuButton.qml
            src/app/qml/PrimaryButton.qml
            src/app/qml/SubButton.qml
            src/app/qml/SubToolButton.qml
            src/app/qml/SubTextField.qml
            src/app/qml/SubScrollBar.qml
            src/app/qml/TimelineZoomBar.qml
            src/app/qml/SubComboBox.qml
            src/app/qml/SubSpinBox.qml
            src/app/qml/SubCheckBox.qml
            src/app/qml/SubMenu.qml
            src/app/qml/SubToolTip.qml
        RESOURCES
            src/app/qml/icons/play.svg
            src/app/qml/icons/pause.svg
            src/app/qml/icons/step-back.svg
            src/app/qml/icons/step-forward.svg
            src/app/qml/icons/minus.svg
            src/app/qml/icons/plus.svg
        SOURCES
            src/app/timeline_scene_item.cpp
            src/app/timeline_scene_item.h
            src/app/video_preview_item.cpp
            src/app/video_preview_item.h
    )
    target_link_libraries(SubCue PRIVATE
        SubCue::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
    add_custom_command(TARGET SubCue POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/asr_python_worker.py"
            "$<TARGET_FILE_DIR:SubCue>/asr_python_worker.py")
    if(WIN32)
        target_link_libraries(SubCue PRIVATE dwmapi)
    endif()
    target_include_directories(SubCue PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    set_target_properties(SubCue PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME "SubCue")
    if(WIN32)
        # VS 调试（F5）时补齐运行时目录：Qt 的调试 DLL 与发行版同目录，FFmpeg 的
        # 调试 DLL 单独位于 debug/bin，二者都不在默认 PATH 上。
        set_target_properties(SubCue PROPERTIES
            VS_DEBUGGER_ENVIRONMENT "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$<IF:$<CONFIG:Debug>,${SUBCUE_FFMPEG_ROOT}/debug/bin,${SUBCUE_FFMPEG_ROOT}/bin>;%PATH%")
        if(SUBCUE_ENABLE_CUDA)
            set_target_properties(SubCue PROPERTIES
                VS_DEBUGGER_ENVIRONMENT "PATH=${CUDAToolkit_BIN_DIR}/x64;${CUDAToolkit_BIN_DIR};$<TARGET_FILE_DIR:Qt6::Core>;${SUBCUE_FFMPEG_ROOT}/bin;%PATH%")
        endif()
        set(SUBCUE_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
        add_custom_command(
            OUTPUT "${SUBCUE_PACKAGE_DIR}/package.stamp"
            COMMAND ${CMAKE_COMMAND}
                "-DSUBCUE_EXE=$<TARGET_FILE:SubCue>"
                "-DSUBCUE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "-DSUBCUE_PACKAGE_DIR=${SUBCUE_PACKAGE_DIR}"
                "-DQT_BIN_DIR=$<TARGET_FILE_DIR:Qt6::Core>"
                "-DFFMPEG_BIN_DIR=$<IF:$<CONFIG:Debug>,${SUBCUE_FFMPEG_ROOT}/debug/bin,${SUBCUE_FFMPEG_ROOT}/bin>"
                "-DFFMPEG_SHARE_DIR=${SUBCUE_FFMPEG_ROOT}/share/ffmpeg"
                "-DCUDA_BIN_DIR=${CUDAToolkit_BIN_DIR}"
                "-DSUBCUE_BUILD_TYPE=$<CONFIG>"
                -P "${CMAKE_SOURCE_DIR}/cmake/PackageWindows.cmake"
            DEPENDS
                SubCue
                "${CMAKE_SOURCE_DIR}/cmake/PackageWindows.cmake"
                "${CMAKE_SOURCE_DIR}/licenses/NOTICE.txt"
                "${CMAKE_SOURCE_DIR}/src/app/qml/Main.qml"
                "${CMAKE_SOURCE_DIR}/src/app/qml/AboutWindow.qml"
            COMMENT "Assembling portable Windows package"
            VERBATIM
        )
        add_custom_target(subcue-windows-package DEPENDS "${SUBCUE_PACKAGE_DIR}/package.stamp")
    endif()
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# TODO(temp): 临时诊断目标，定位完成后删除。
add_subdirectory(.test_tmp/prime_probe)
