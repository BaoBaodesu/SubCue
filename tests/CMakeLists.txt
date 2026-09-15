# Qt 与 FFmpeg 的运行时必须在 PATH 中先于系统同名 DLL，否则测试会加载到错误版本。
set(SUBCUE_TEST_PATH_MODIFICATION
    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:$<IF:$<CONFIG:Debug>,${SUBCUE_FFMPEG_ROOT}/debug/bin,${SUBCUE_FFMPEG_ROOT}/bin>"
)

# 应用层测试需要 AppController 与 QML 场景项，集中声明避免逐目标重复。
set(SUBCUE_APP_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/src/app/app_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/app/app_controller.h
    ${CMAKE_SOURCE_DIR}/src/app/application_context.cpp
    ${CMAKE_SOURCE_DIR}/src/app/application_context.h
)
set(SUBCUE_APP_TEST_ITEM_SOURCES
    ${CMAKE_SOURCE_DIR}/src/app/timeline_scene_item.cpp
    ${CMAKE_SOURCE_DIR}/src/app/timeline_scene_item.h
    ${CMAKE_SOURCE_DIR}/src/app/video_preview_item.cpp
    ${CMAKE_SOURCE_DIR}/src/app/video_preview_item.h
)

# 目标名即 CTest 名称。ENVIRONMENT_MODIFICATION 的值本身含分号，必须在调用点整体加引号，
# 不能再包一层列表变量，否则展开时会被拆成多个参数。
function(subcue_register_test target)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        ENVIRONMENT_MODIFICATION "${SUBCUE_TEST_PATH_MODIFICATION}"
        ${ARGN}
    )
endfunction()

# 需要真正加载 QML 的测试：额外编译场景项并注册独立的 QML 模块。
function(subcue_add_qml_test target source)
    qt_add_executable(${target} ${source} ${SUBCUE_APP_TEST_SOURCES})
    qt_add_qml_module(${target}
        URI ${target}
        VERSION 1.0
        NO_IMPORT_SCAN
        SOURCES
            ${SUBCUE_APP_TEST_ITEM_SOURCES}
    )
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/core
    )
    target_compile_definitions(${target} PRIVATE
        SUBCUE_TEST_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/media"
        SUBCUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(${target} PRIVATE SubCue::Core Qt6::Test Qt6::Gui Qt6::Qml Qt6::Quick)
endfunction()

qt_add_executable(SubCueMediaTimeTests cpp/test_media_time.cpp)
target_link_libraries(SubCueMediaTimeTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueMediaTimeTests)

qt_add_executable(SubCueSubtitleProjectTests cpp/test_subtitle_project.cpp)
target_link_libraries(SubCueSubtitleProjectTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueSubtitleProjectTests)

qt_add_executable(SubCueMediaTests cpp/test_media.cpp)
target_compile_definitions(SubCueMediaTests PRIVATE
    SUBCUE_TEST_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/media"
)
target_link_libraries(SubCueMediaTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueMediaTests)

qt_add_executable(SubCuePlaybackTests cpp/test_playback.cpp)
target_compile_definitions(SubCuePlaybackTests PRIVATE
    SUBCUE_TEST_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/media"
)
target_link_libraries(SubCuePlaybackTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCuePlaybackTests TIMEOUT 30)

qt_add_executable(SubCueCacheWaveformTests cpp/test_cache_waveform.cpp)
target_compile_definitions(SubCueCacheWaveformTests PRIVATE
    SUBCUE_TEST_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/media"
)
target_link_libraries(SubCueCacheWaveformTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueCacheWaveformTests TIMEOUT 30)

qt_add_executable(SubCueTimelineTests cpp/test_timeline.cpp)
target_link_libraries(SubCueTimelineTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueTimelineTests)

qt_add_executable(SubCueRoughCutXmlTests cpp/test_roughcut_xml.cpp)
target_link_libraries(SubCueRoughCutXmlTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueRoughCutXmlTests)

qt_add_executable(SubCueRoughCutScriptTests cpp/test_roughcut_script.cpp)
target_link_libraries(SubCueRoughCutScriptTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueRoughCutScriptTests)

qt_add_executable(SubCueRoughCutAlignmentTests cpp/test_roughcut_alignment.cpp)
target_link_libraries(SubCueRoughCutAlignmentTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueRoughCutAlignmentTests)

qt_add_executable(SubCueRetakeDetectorTests cpp/test_retake_detector.cpp)
target_link_libraries(SubCueRetakeDetectorTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueRetakeDetectorTests)

qt_add_executable(SubCueRoughCutDecisionTests cpp/test_roughcut_decision.cpp)
target_link_libraries(SubCueRoughCutDecisionTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueRoughCutDecisionTests)

qt_add_executable(SubCueRoughCutTimelineTests cpp/test_roughcut_timeline.cpp)
target_link_libraries(SubCueRoughCutTimelineTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueRoughCutTimelineTests)

qt_add_executable(SubCueRoughCutResultModelTests
    cpp/test_rough_cut_result_model.cpp
    ${CMAKE_SOURCE_DIR}/src/app/rough_cut_result_model.cpp
    ${CMAKE_SOURCE_DIR}/src/app/rough_cut_result_model.h)
target_include_directories(SubCueRoughCutResultModelTests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
target_link_libraries(SubCueRoughCutResultModelTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueRoughCutResultModelTests)

qt_add_executable(SubCueRoughCutProjectTests cpp/test_roughcut_project.cpp)
target_link_libraries(SubCueRoughCutProjectTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueRoughCutProjectTests)

qt_add_executable(SubCueRoughCutAuxiliaryTests cpp/test_roughcut_auxiliary.cpp)
target_link_libraries(SubCueRoughCutAuxiliaryTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueRoughCutAuxiliaryTests)

qt_add_executable(SubCueAlignmentTests cpp/test_alignment.cpp)
target_compile_definitions(SubCueAlignmentTests PRIVATE
    SUBCUE_TEST_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
)
target_link_libraries(SubCueAlignmentTests PRIVATE SubCue::Core Qt6::Test)
subcue_register_test(SubCueAlignmentTests)

qt_add_executable(SubCueAsrTests cpp/test_asr.cpp)
target_compile_definitions(SubCueAsrTests PRIVATE
    SUBCUE_TEST_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/media"
    SUBCUE_TEST_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
)
target_link_libraries(SubCueAsrTests PRIVATE SubCue::Core Qt6::Test Qt6::Network)
subcue_register_test(SubCueAsrTests)

qt_add_executable(SubCueAiReviewTests cpp/test_ai_review.cpp)
target_link_libraries(SubCueAiReviewTests PRIVATE SubCue::Core Qt6::Test Qt6::Network)
subcue_register_test(SubCueAiReviewTests)

subcue_add_qml_test(SubCueAppControllerTests cpp/test_app_controller.cpp)
subcue_register_test(SubCueAppControllerTests
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 30
)

subcue_add_qml_test(SubCueEditorIntegrationTests cpp/test_editor_integration.cpp)
if(WIN32)
    target_link_libraries(SubCueEditorIntegrationTests PRIVATE Psapi)
endif()
subcue_register_test(SubCueEditorIntegrationTests
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 120
)

qt_add_executable(SubCueSourceHygieneTests cpp/test_source_hygiene.cpp)
target_compile_definitions(SubCueSourceHygieneTests PRIVATE
    SUBCUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(SUBCUE_BUILD_APP)
    target_compile_definitions(SubCueSourceHygieneTests PRIVATE
        SUBCUE_APP_PATH="$<TARGET_FILE:SubCue>"
    )
    add_dependencies(SubCueSourceHygieneTests SubCue)
endif()
target_link_libraries(SubCueSourceHygieneTests PRIVATE Qt6::Test)
subcue_register_test(SubCueSourceHygieneTests)

if(SUBCUE_BUILD_APP AND WIN32)
    qt_add_executable(SubCuePackagingTests cpp/test_packaging.cpp)
    target_compile_definitions(SubCuePackagingTests PRIVATE
        SUBCUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        SUBCUE_PACKAGE_DIR="${CMAKE_BINARY_DIR}/package"
    )
    if(SUBCUE_ENABLE_CUDA)
        target_compile_definitions(SubCuePackagingTests PRIVATE SUBCUE_PACKAGE_HAS_CUDA=1)
    endif()
    add_dependencies(SubCuePackagingTests subcue-windows-package)
    target_link_libraries(SubCuePackagingTests PRIVATE Qt6::Test)
    subcue_register_test(SubCuePackagingTests
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60
    )
endif()

if(SUBCUE_BUILD_APP)
    add_test(NAME SubCueStartupTest COMMAND SubCue --smoke-test)
    set_tests_properties(SubCueStartupTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "${SUBCUE_TEST_PATH_MODIFICATION}"
        TIMEOUT 10
    )
    add_test(NAME SubCueRoughCutStartupTest COMMAND SubCue --roughcut-workspace --smoke-test)
    set_tests_properties(SubCueRoughCutStartupTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "${SUBCUE_TEST_PATH_MODIFICATION}"
        TIMEOUT 10
    )
endif()
