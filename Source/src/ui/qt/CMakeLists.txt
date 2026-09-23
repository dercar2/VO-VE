set(VOVE_UPDATE_MANIFEST_URL "" CACHE STRING "HTTPS URL of the signed VO-VE update manifest")
set(VOVE_UPDATE_PUBLIC_KEY_BASE64 "" CACHE STRING "SEC1 P-256 update public key in canonical base64")
set(VOVE_REPOSITORY_URL "https://github.com/dercar2/VO-VE" CACHE STRING
    "HTTPS URL of the public VO-VE repository")
set(VOVE_RELEASES_API_URL "https://api.github.com/repos/dercar2/VO-VE/releases/latest" CACHE STRING
    "HTTPS URL of the latest published VO-VE release metadata")
set(VOVE_DONATE_URL "" CACHE STRING "Confirmed HTTPS project support page")
set(VOVE_ISSUE_URL "" CACHE STRING "Confirmed HTTPS project issue page")
set(VOVE_BUILD_LABEL "development" CACHE STRING "Human-readable VO-VE build label")
foreach(vove_release_url IN ITEMS VOVE_UPDATE_MANIFEST_URL VOVE_REPOSITORY_URL
        VOVE_RELEASES_API_URL VOVE_DONATE_URL VOVE_ISSUE_URL)
    if(NOT "${${vove_release_url}}" STREQUAL "" AND
       NOT "${${vove_release_url}}" MATCHES "^https://")
        message(FATAL_ERROR "${vove_release_url} must be empty or use HTTPS")
    endif()
endforeach()
if((VOVE_UPDATE_MANIFEST_URL STREQUAL "" AND NOT VOVE_UPDATE_PUBLIC_KEY_BASE64 STREQUAL "") OR
   (NOT VOVE_UPDATE_MANIFEST_URL STREQUAL "" AND VOVE_UPDATE_PUBLIC_KEY_BASE64 STREQUAL ""))
    message(FATAL_ERROR "Update manifest URL and public key must be configured together")
endif()
string(LENGTH "${VOVE_UPDATE_PUBLIC_KEY_BASE64}" vove_update_public_key_length)
if(NOT VOVE_UPDATE_PUBLIC_KEY_BASE64 STREQUAL "" AND
   (NOT vove_update_public_key_length EQUAL 88 OR
    NOT VOVE_UPDATE_PUBLIC_KEY_BASE64 MATCHES "^B[A-Za-z0-9+/]+=$"))
    message(FATAL_ERROR "Update public key must be a canonical 65-byte SEC1 P-256 key")
endif()
foreach(vove_release_value IN ITEMS
        VOVE_UPDATE_MANIFEST_URL VOVE_UPDATE_PUBLIC_KEY_BASE64 VOVE_REPOSITORY_URL
        VOVE_RELEASES_API_URL VOVE_DONATE_URL VOVE_ISSUE_URL)
    string(FIND "${${vove_release_value}}" ")vove_config\"" vove_delimiter_position)
    if(NOT vove_delimiter_position EQUAL -1)
        message(FATAL_ERROR "${vove_release_value} contains the generated-header delimiter")
    endif()
endforeach()
configure_file(release_configuration_data.hpp.in release_configuration_data.hpp @ONLY)

add_library(vove-update-qt STATIC
    release_configuration.cpp
    release_configuration.hpp
    update_checker.cpp
    update_checker.hpp
    update_manifest.cpp
    update_manifest.hpp
    update_signature.hpp)
if(WIN32)
    target_sources(vove-update-qt PRIVATE update_signature_windows.cpp)
    target_link_libraries(vove-update-qt PRIVATE bcrypt)
else()
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
    target_sources(vove-update-qt PRIVATE update_signature_posix.cpp)
    target_link_libraries(vove-update-qt PRIVATE OpenSSL::Crypto)
endif()
target_include_directories(vove-update-qt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(vove-update-qt PUBLIC Qt6::Core Qt6::Network)
vove_enable_warnings(vove-update-qt)

add_executable(vove-open-helper WIN32 open_helper_main.cpp directory_watch_helper.cpp)
target_link_libraries(vove-open-helper PRIVATE Qt6::Gui)
set_target_properties(vove-open-helper PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
vove_enable_warnings(vove-open-helper)

add_executable(vove-ui-qt WIN32
    diagnostic_sink.cpp
    diagnostic_sink.hpp
    components_panel.cpp
    components_panel.hpp
    about_dialog.cpp
    about_dialog.hpp
    lazy_directory_tree_model.cpp
    lazy_directory_tree_model.hpp
    drag_views.cpp
    drag_views.hpp
    scrollbar_style.cpp
    scrollbar_style.hpp
    smooth_scroll.cpp
    smooth_scroll.hpp
    application_icon.cpp
    application_icon.hpp
    batch_rename_dialog.cpp
    batch_rename_dialog.hpp
    rename_dialog.cpp
    rename_dialog.hpp
    launch_request.cpp
    launch_request.hpp
    main.cpp
    main_menu_dialog.cpp
    main_menu_dialog.hpp
    object_transfer_queue.cpp
    main_window.cpp
    main_window.hpp
    startup_registration.cpp
    startup_registration.hpp
    directory_monitor.cpp
    directory_monitor.hpp
    window_controls.cpp
    window_controls.hpp
    window_frame.cpp
    window_frame.hpp
    navigation_button.cpp
    navigation_button.hpp
    page_button.cpp
    page_button.hpp
    round_button_artwork.hpp
    create_folder_button.cpp
    create_folder_button.hpp
    add_favorite_button.cpp
    add_favorite_button.hpp
    catalog_tool_button.cpp
    catalog_tool_button.hpp
    catalog_tool_artwork.hpp
    toolbar_fields.cpp
    toolbar_fields.hpp
    interface_font.cpp
    interface_font.hpp
    theme_switch.cpp
    theme_switch.hpp
    thumbnail_scale.cpp
    thumbnail_scale.hpp
    directory_list_model.cpp
    directory_list_model.hpp
    directory_item_delegate.cpp
    directory_item_delegate.hpp
    default_application_name.cpp
    default_application_name.hpp
    preview_modified_label.cpp
    preview_modified_label.hpp
    preview_name_label.cpp
    preview_name_label.hpp
    zoom_preview.cpp
    zoom_preview.hpp
    operation_notice.cpp
    operation_notice.hpp
    external_component_detection.cpp
    external_component_detection.hpp
    external_application_registry.cpp
    external_application_registry.hpp
    external_open_coordinator.cpp
    external_open_coordinator.hpp
    format_style.cpp
    format_style.hpp
    global_search_client.cpp
    global_search_client.hpp
    folder_mosaic_controller.cpp
    folder_mosaic_controller.hpp
    preview_client.cpp
    preview_client.hpp
    trash_dialog.cpp
    trash_dialog.hpp
    translation_manager.cpp
    translation_manager.hpp
    worker_startup_text.hpp
    update_dialog.cpp
    update_dialog.hpp
    ui_text.cpp
    ui_text.hpp
    window_placement.cpp
    window_placement.hpp
)

if(BUILD_TESTING AND VOVE_BUILD_TESTS)
    add_executable(vove-zoom-preview-tests
        ${PROJECT_SOURCE_DIR}/tests/ui/zoom_preview_test.cpp
        zoom_preview.cpp)
    target_include_directories(vove-zoom-preview-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-zoom-preview-tests PRIVATE Qt6::Widgets)
    vove_copy_cxx_runtime(vove-zoom-preview-tests)
    vove_enable_warnings(vove-zoom-preview-tests)
    add_test(NAME vove-zoom-preview-tests COMMAND vove-zoom-preview-tests)
    set_tests_properties(vove-zoom-preview-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    if(TARGET Qt6::QGifPlugin)
        set_property(TEST vove-zoom-preview-tests APPEND PROPERTY ENVIRONMENT
            "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QGifPlugin>/..")
    endif()
    if(WIN32)
        set_tests_properties(vove-zoom-preview-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    add_executable(vove-operation-notice-tests
        ${PROJECT_SOURCE_DIR}/tests/ui/operation_notice_test.cpp
        operation_notice.cpp)
    target_include_directories(vove-operation-notice-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-operation-notice-tests PRIVATE Qt6::Widgets)
    vove_copy_cxx_runtime(vove-operation-notice-tests)
    vove_enable_warnings(vove-operation-notice-tests)
    add_test(NAME vove-operation-notice-tests COMMAND vove-operation-notice-tests)
    set_tests_properties(vove-operation-notice-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    if(WIN32)
        set_tests_properties(vove-operation-notice-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
endif()
target_compile_definitions(vove-ui-qt PRIVATE
    VOVE_VERSION="${PROJECT_VERSION}"
    VOVE_BUILD_LABEL="${VOVE_BUILD_LABEL}")
set(vove_application_icons
    "${PROJECT_SOURCE_DIR}/resources/artwork/application-icon-dark.png"
    "${PROJECT_SOURCE_DIR}/resources/artwork/application-icon-light.png")
qt_add_resources(vove-ui-qt "vove-application-icons"
    PREFIX "/artwork"
    BASE "${PROJECT_SOURCE_DIR}/resources/artwork"
    FILES ${vove_application_icons})
set(vove_author_artwork
    artwork/empty-file-north.png
    artwork/empty-file-vanilla.png
    artwork/empty-file-breeze.png
    artwork/empty-file-twilight.png
    artwork/empty-folder-north.png
    artwork/empty-folder-vanilla.png
    artwork/empty-folder-breeze.png
    artwork/empty-folder-twilight.png
    artwork/logo-north.png
    artwork/logo-vanilla.png
    artwork/logo-breeze.png
    artwork/logo-twilight.png)
qt_add_resources(vove-ui-qt "vove-author-artwork"
    PREFIX "/artwork"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/artwork"
    FILES ${vove_author_artwork})
qt_add_resources(vove-ui-qt "vove-license"
    PREFIX "/licenses"
    BASE "${PROJECT_SOURCE_DIR}"
    FILES "${PROJECT_SOURCE_DIR}/LICENSE")
if(WIN32)
    set_source_files_properties(vo-ve.rc PROPERTIES
        OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/packaging/windows/vo-ve.ico")
    target_sources(vove-ui-qt PRIVATE vo-ve.rc)
endif()

set(VOVE_TRANSLATION_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../translations/vove_ru.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../translations/vove_zh_CN.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../translations/vove_ar.ts)
qt_add_lupdate(
    SOURCE_TARGETS vove-ui-qt
    TS_FILES ${VOVE_TRANSLATION_FILES}
    LUPDATE_TARGET vove-update-translations
    OPTIONS -no-obsolete)
set(VOVE_TRANSLATION_QM_FILES)
foreach(vove_ts_file IN LISTS VOVE_TRANSLATION_FILES)
    get_filename_component(vove_catalog_name "${vove_ts_file}" NAME_WE)
    string(REGEX REPLACE "^vove_" "" vove_language "${vove_catalog_name}")
    set(vove_qt_catalog
        "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}/qtbase_${vove_language}.qm")
    if(NOT EXISTS "${vove_qt_catalog}")
        message(FATAL_ERROR
            "Qt base translation is required for ${vove_language}: ${vove_qt_catalog}")
    endif()
    set(vove_qm_file
        "${CMAKE_CURRENT_BINARY_DIR}/translations/${vove_catalog_name}.qm")
    add_custom_command(
        OUTPUT "${vove_qm_file}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/translations"
        COMMAND $<TARGET_FILE:Qt6::lrelease>
            -nounfinished "${vove_ts_file}" "${vove_qt_catalog}" -qm "${vove_qm_file}"
        DEPENDS Qt6::lrelease "${vove_ts_file}" "${vove_qt_catalog}"
        VERBATIM)
    list(APPEND VOVE_TRANSLATION_QM_FILES "${vove_qm_file}")
endforeach()
add_custom_target(vove-translations ALL DEPENDS ${VOVE_TRANSLATION_QM_FILES})
add_dependencies(vove-ui-qt vove-translations)
add_custom_command(TARGET vove-ui-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:vove-ui-qt>/translations
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${VOVE_TRANSLATION_QM_FILES}
        $<TARGET_FILE_DIR:vove-ui-qt>/translations
    VERBATIM)
set(VOVE_PREVIEW_RUNTIME_TARGETS
    vove-preview-helper
    vove-raster-worker
    vove-xcf-worker
    vove-cdr-worker)
set(VOVE_PREVIEW_RUNTIME_FILES
    $<TARGET_FILE:vove-preview-helper>
    $<TARGET_FILE:vove-raster-worker>
    $<TARGET_FILE:vove-xcf-worker>
    $<TARGET_FILE:vove-cdr-worker>)
if(TARGET vove-document-worker)
    list(APPEND VOVE_PREVIEW_RUNTIME_TARGETS vove-document-worker)
    list(APPEND VOVE_PREVIEW_RUNTIME_FILES $<TARGET_FILE:vove-document-worker>)
endif()
if(TARGET vove-svg-worker)
    list(APPEND VOVE_PREVIEW_RUNTIME_TARGETS vove-svg-worker)
    list(APPEND VOVE_PREVIEW_RUNTIME_FILES $<TARGET_FILE:vove-svg-worker>)
endif()
if(WIN32)
    set(VOVE_PREVIEW_RUNTIME_TEST_COMMANDS)
    if(BUILD_TESTING)
        list(APPEND VOVE_PREVIEW_RUNTIME_TEST_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:vove-ui-qt>/platforms
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>
                $<TARGET_FILE_DIR:vove-ui-qt>/platforms/$<TARGET_FILE_NAME:Qt6::QOffscreenIntegrationPlugin>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>
                $<TARGET_FILE_DIR:vove-ui-qt>/platforms/$<TARGET_FILE_NAME:Qt6::QWindowsIntegrationPlugin>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt6::QTiffPlugin>
                $<TARGET_FILE_DIR:vove-ui-qt>/imageformats/$<TARGET_FILE_NAME:Qt6::QTiffPlugin>)
    endif()
    add_custom_target(vove-preview-runtime-files ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:vove-ui-qt>/imageformats
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${VOVE_PREVIEW_RUNTIME_FILES}
            $<TARGET_FILE:Qt6::Core>
            $<TARGET_FILE:Qt6::Gui>
            $<TARGET_FILE_DIR:vove-ui-qt>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Qt6::QJpegPlugin>
            $<TARGET_FILE_DIR:vove-ui-qt>/imageformats/$<TARGET_FILE_NAME:Qt6::QJpegPlugin>
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:vove-ui-qt>/tls
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Qt6::QSchannelBackendPlugin>
            $<TARGET_FILE_DIR:vove-ui-qt>/tls/$<TARGET_FILE_NAME:Qt6::QSchannelBackendPlugin>
        ${VOVE_PREVIEW_RUNTIME_TEST_COMMANDS}
        DEPENDS ${VOVE_PREVIEW_RUNTIME_TARGETS}
        VERBATIM)
else()
    add_custom_target(vove-preview-runtime-files ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${VOVE_PREVIEW_RUNTIME_FILES}
            $<TARGET_FILE_DIR:vove-ui-qt>
        DEPENDS ${VOVE_PREVIEW_RUNTIME_TARGETS}
        VERBATIM)
endif()
add_dependencies(vove-ui-qt vove-catalog-helper vove-fileop-helper vove-open-helper
    vove-preview-runtime-files)
if(TARGET vove-everything-helper)
    add_dependencies(vove-ui-qt vove-everything-helper)
endif()
if(TARGET vove-plocate-helper)
    add_dependencies(vove-ui-qt vove-plocate-helper)
endif()
add_custom_command(TARGET vove-ui-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:vove-catalog-helper>
        $<TARGET_FILE_DIR:vove-ui-qt>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:vove-fileop-helper>
        $<TARGET_FILE_DIR:vove-ui-qt>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:vove-open-helper>
        $<TARGET_FILE_DIR:vove-ui-qt>
    VERBATIM
)
if(TARGET vove-everything-helper)
    add_custom_command(TARGET vove-ui-qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:vove-everything-helper>
            $<TARGET_FILE_DIR:vove-ui-qt>
        VERBATIM)
endif()
if(TARGET vove-plocate-helper)
    add_custom_command(TARGET vove-ui-qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:vove-plocate-helper>
            $<TARGET_FILE_DIR:vove-ui-qt>
        VERBATIM)
endif()
vove_copy_cxx_runtime(vove-ui-qt)


target_link_libraries(vove-ui-qt PRIVATE
    vove::catalog
    vove::core
    vove::color
    vove::fileops
    vove::platform
    vove::preview
    vove::preview_protocol
    vove::search
    vove-update-qt
    Qt6::Network
    Qt6::Widgets
)
if(WIN32)
    target_link_libraries(vove-ui-qt PRIVATE shlwapi dwmapi)
    target_compile_definitions(vove-ui-qt PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
target_include_directories(vove-ui-qt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../search)

set_target_properties(vove-ui-qt PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

vove_enable_warnings(vove-ui-qt)

if(VOVE_BUILD_BENCHMARKS)
    add_executable(vove-preview-bench
        ${CMAKE_CURRENT_SOURCE_DIR}/../../bench/preview_pipeline_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../bench/process_memory.cpp
        diagnostic_sink.cpp
        diagnostic_sink.hpp
        preview_client.cpp
        preview_client.hpp
    )
    target_include_directories(vove-preview-bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../bench)
    target_link_libraries(vove-preview-bench PRIVATE
        vove::color
        vove::platform
        vove::preview_protocol
        Qt6::Gui
        Qt6::Network
    )
    add_dependencies(vove-preview-bench vove-preview-runtime-files)
    vove_copy_cxx_runtime(vove-preview-bench)
    vove_enable_warnings(vove-preview-bench)
endif()

if(BUILD_TESTING AND VOVE_BUILD_TESTS)
    find_package(Threads REQUIRED)
    add_executable(vove-diagnostic-sink-tests
        ${PROJECT_SOURCE_DIR}/tests/ui/diagnostic_sink_test.cpp
        diagnostic_sink.cpp
        diagnostic_sink.hpp)
    target_include_directories(vove-diagnostic-sink-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-diagnostic-sink-tests PRIVATE Threads::Threads)
    vove_enable_warnings(vove-diagnostic-sink-tests)
    add_test(NAME vove-diagnostic-sink-tests COMMAND vove-diagnostic-sink-tests)
    set_tests_properties(vove-diagnostic-sink-tests PROPERTIES TIMEOUT 30)

    if(WIN32)
        add_executable(vove-installed-preview-smoke
            ${PROJECT_SOURCE_DIR}/tests/packaging/installed_preview_smoke.cpp)
        target_link_libraries(vove-installed-preview-smoke PRIVATE
            vove::platform vove::preview_protocol Qt6::Gui Qt6::Network ole32 windowscodecs)
        target_compile_definitions(vove-installed-preview-smoke PRIVATE
            _WIN32_WINNT=0x0A00 WINVER=0x0A00)
        vove_enable_warnings(vove-installed-preview-smoke)
        # Invoked explicitly by the isolated installer test, never shipped with the application.

        add_executable(vove-test-legacy-preview-helper
            ${PROJECT_SOURCE_DIR}/tests/packaging/legacy_preview_helper_fixture.cpp)
        vove_enable_warnings(vove-test-legacy-preview-helper)
        if(MINGW)
            target_link_options(vove-test-legacy-preview-helper PRIVATE -static)
        endif()
        # Simulates an installed older helper that does not understand the shutdown CLI.
    endif()
    add_executable(vove-launch-request-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/launch_request_test.cpp
        launch_request.cpp
        launch_request.hpp)
    target_include_directories(vove-launch-request-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-launch-request-tests PRIVATE Qt6::Core Qt6::Network)
    vove_copy_cxx_runtime(vove-launch-request-tests)
    vove_enable_warnings(vove-launch-request-tests)
    add_test(NAME vove-launch-request-tests COMMAND vove-launch-request-tests)
    set_tests_properties(vove-launch-request-tests PROPERTIES TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-launch-request-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-lazy-directory-tree-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/lazy_directory_tree_model_test.cpp
        lazy_directory_tree_model.cpp
        lazy_directory_tree_model.hpp
        drag_views.cpp)
    target_include_directories(vove-lazy-directory-tree-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(vove-lazy-directory-tree-tests PRIVATE Qt6::Widgets)
    vove_copy_cxx_runtime(vove-lazy-directory-tree-tests)
    vove_enable_warnings(vove-lazy-directory-tree-tests)
    add_test(NAME vove-lazy-directory-tree-tests COMMAND vove-lazy-directory-tree-tests)
    set_tests_properties(vove-lazy-directory-tree-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-lazy-directory-tree-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-drag-views-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/drag_views_test.cpp
        drag_views.cpp
        drag_views.hpp)
    if(WIN32)
        # Run --native separately on an interactive desktop after Qt/DnD changes.
        # It checks OLE and a fresh Qt process; see docs/WINDOWS_FILE_DRAG.md.
        add_executable(vove-windows-file-drag-tests
            ${PROJECT_SOURCE_DIR}/tests/ui/windows_file_drag_test.cpp)
        target_include_directories(vove-windows-file-drag-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(vove-windows-file-drag-tests PRIVATE Qt6::Widgets ole32 shell32 uuid)
        vove_copy_cxx_runtime(vove-windows-file-drag-tests)
        vove_enable_warnings(vove-windows-file-drag-tests)
        add_test(NAME vove-windows-file-drag-tests COMMAND vove-windows-file-drag-tests)
        set_tests_properties(vove-windows-file-drag-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=windows" TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    target_include_directories(vove-drag-views-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-drag-views-tests PRIVATE Qt6::Widgets)
    vove_copy_cxx_runtime(vove-drag-views-tests)
    vove_enable_warnings(vove-drag-views-tests)
    add_test(NAME vove-drag-views-tests COMMAND vove-drag-views-tests)
    set_tests_properties(vove-drag-views-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-drag-views-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-smooth-scroll-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/smooth_scroll_test.cpp
        smooth_scroll.cpp
        smooth_scroll.hpp)
    target_include_directories(vove-smooth-scroll-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-smooth-scroll-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-smooth-scroll-tests)
    vove_enable_warnings(vove-smooth-scroll-tests)
    add_test(NAME vove-smooth-scroll-tests COMMAND vove-smooth-scroll-tests)
    set_tests_properties(vove-smooth-scroll-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-smooth-scroll-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-scrollbar-style-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/scrollbar_style_test.cpp
        scrollbar_style.cpp
        scrollbar_style.hpp)
    target_include_directories(vove-scrollbar-style-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-scrollbar-style-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-scrollbar-style-tests)
    vove_enable_warnings(vove-scrollbar-style-tests)
    add_test(NAME vove-scrollbar-style-tests COMMAND vove-scrollbar-style-tests
        ${CMAKE_CURRENT_BINARY_DIR}/scrollbar-states.png)
    set_tests_properties(vove-scrollbar-style-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-scrollbar-style-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-toolbar-fields-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/toolbar_fields_test.cpp
        toolbar_fields.cpp
        toolbar_fields.hpp)
    target_include_directories(vove-toolbar-fields-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-toolbar-fields-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-toolbar-fields-tests)
    vove_enable_warnings(vove-toolbar-fields-tests)
    add_test(NAME vove-toolbar-fields-tests COMMAND vove-toolbar-fields-tests
        ${CMAKE_CURRENT_BINARY_DIR}/toolbar-field-states.png)
    set_tests_properties(vove-toolbar-fields-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-toolbar-fields-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-create-folder-button-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/create_folder_button_test.cpp
        create_folder_button.cpp
        create_folder_button.hpp
        round_button_artwork.hpp)
    target_include_directories(vove-create-folder-button-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-create-folder-button-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-create-folder-button-tests)
    vove_enable_warnings(vove-create-folder-button-tests)
    add_test(NAME vove-create-folder-button-tests COMMAND vove-create-folder-button-tests
        ${CMAKE_CURRENT_BINARY_DIR}/create-folder-states.png)
    set_tests_properties(vove-create-folder-button-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-create-folder-button-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-add-favorite-button-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/add_favorite_button_test.cpp
        add_favorite_button.cpp
        add_favorite_button.hpp)
    target_include_directories(vove-add-favorite-button-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-add-favorite-button-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-add-favorite-button-tests)
    vove_enable_warnings(vove-add-favorite-button-tests)
    add_test(NAME vove-add-favorite-button-tests COMMAND vove-add-favorite-button-tests
        ${CMAKE_CURRENT_BINARY_DIR}/add-favorite-states.png)
    set_tests_properties(vove-add-favorite-button-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-add-favorite-button-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-catalog-tool-button-tests
        ${PROJECT_SOURCE_DIR}/tests/ui/catalog_tool_button_test.cpp
        catalog_tool_button.cpp catalog_tool_button.hpp catalog_tool_artwork.hpp)
    target_include_directories(vove-catalog-tool-button-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-catalog-tool-button-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-catalog-tool-button-tests)
    vove_enable_warnings(vove-catalog-tool-button-tests)
    add_test(NAME vove-catalog-tool-button-tests COMMAND vove-catalog-tool-button-tests
        ${CMAKE_CURRENT_BINARY_DIR}/catalog-tools)
    set_tests_properties(vove-catalog-tool-button-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-catalog-tool-button-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    set(VOVE_TEST_INTERFACE_FONT "" CACHE FILEPATH
        "Optional LINE Seed JP font for glyph tests; never included in the runtime")
    add_executable(vove-interface-font-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/interface_font_test.cpp
        interface_font.cpp
        interface_font.hpp)
    target_include_directories(vove-interface-font-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-interface-font-tests PRIVATE Qt6::Widgets)
    vove_copy_cxx_runtime(vove-interface-font-tests)
    vove_enable_warnings(vove-interface-font-tests)
    add_test(NAME vove-interface-font-tests COMMAND vove-interface-font-tests
        ${VOVE_TEST_INTERFACE_FONT})
    set_tests_properties(vove-interface-font-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        # The Windows offscreen backend does not expose installed system fonts.
        set_tests_properties(vove-interface-font-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-navigation-button-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/navigation_button_test.cpp
        navigation_button.cpp
        navigation_button.hpp)
    target_include_directories(vove-navigation-button-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-navigation-button-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-navigation-button-tests)
    vove_enable_warnings(vove-navigation-button-tests)
    add_test(NAME vove-navigation-button-tests COMMAND vove-navigation-button-tests
        ${CMAKE_CURRENT_BINARY_DIR}/navigation-states.png)
    set_tests_properties(vove-navigation-button-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-navigation-button-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-page-button-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/page_button_test.cpp
        page_button.cpp
        page_button.hpp)
    target_include_directories(vove-page-button-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-page-button-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-page-button-tests)
    vove_enable_warnings(vove-page-button-tests)
    add_test(NAME vove-page-button-tests COMMAND vove-page-button-tests
        ${CMAKE_CURRENT_BINARY_DIR}/page-button-states.png)
    set_tests_properties(vove-page-button-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-page-button-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-application-icon-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/application_icon_test.cpp
        application_icon.cpp
        application_icon.hpp)
    target_include_directories(vove-application-icon-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-application-icon-tests PRIVATE Qt6::Widgets)
    if(WIN32)
        target_link_libraries(vove-application-icon-tests PRIVATE ole32 shell32 uuid)
    endif()
    qt_add_resources(vove-application-icon-tests "vove-test-application-icons"
        PREFIX "/artwork"
        BASE "${PROJECT_SOURCE_DIR}/resources/artwork"
        FILES ${vove_application_icons})
    vove_copy_cxx_runtime(vove-application-icon-tests)
    vove_enable_warnings(vove-application-icon-tests)
    add_test(NAME vove-application-icon-tests COMMAND vove-application-icon-tests)
    set_tests_properties(vove-application-icon-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-application-icon-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-window-controls-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/window_controls_test.cpp
        window_controls.cpp
        window_controls.hpp
        ui_text.cpp
        ui_text.hpp)
    target_include_directories(vove-window-controls-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-window-controls-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-window-controls-tests)
    vove_enable_warnings(vove-window-controls-tests)
    add_test(NAME vove-window-controls-tests COMMAND vove-window-controls-tests)
    set_tests_properties(vove-window-controls-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        set_tests_properties(vove-window-controls-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-window-frame-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/window_frame_test.cpp
        window_frame.cpp
        window_frame.hpp)
    target_include_directories(vove-window-frame-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-window-frame-tests PRIVATE Qt6::Widgets)
    vove_copy_cxx_runtime(vove-window-frame-tests)
    vove_enable_warnings(vove-window-frame-tests)
    add_test(NAME vove-window-frame-tests COMMAND vove-window-frame-tests)
    set_tests_properties(vove-window-frame-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        target_link_libraries(vove-window-frame-tests PRIVATE dwmapi)
        target_compile_definitions(vove-window-frame-tests PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
        set_tests_properties(vove-window-frame-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
        foreach(scale IN ITEMS 1 1.25 1.5 2)
            add_test(NAME vove-window-frame-native-${scale} COMMAND vove-window-frame-tests)
            set_tests_properties(vove-window-frame-native-${scale} PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_SCALE_FACTOR=${scale}"
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}"
                RUN_SERIAL TRUE TIMEOUT 30)
        endforeach()
    endif()

    add_executable(vove-thumbnail-scale-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/thumbnail_scale_test.cpp
        thumbnail_scale.cpp
        thumbnail_scale.hpp)
    target_include_directories(vove-thumbnail-scale-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-thumbnail-scale-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-thumbnail-scale-tests)
    vove_enable_warnings(vove-thumbnail-scale-tests)
    add_test(NAME vove-thumbnail-scale-tests COMMAND vove-thumbnail-scale-tests)
    set_tests_properties(vove-thumbnail-scale-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        set_tests_properties(vove-thumbnail-scale-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-theme-switch-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/theme_switch_test.cpp
        theme_switch.cpp
        theme_switch.hpp)
    target_include_directories(vove-theme-switch-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-theme-switch-tests PRIVATE Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-theme-switch-tests)
    vove_enable_warnings(vove-theme-switch-tests)
    add_test(NAME vove-theme-switch-tests COMMAND vove-theme-switch-tests)
    set_tests_properties(vove-theme-switch-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        set_tests_properties(vove-theme-switch-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-update-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/update_test.cpp)
    target_link_libraries(vove-update-tests PRIVATE vove-update-qt)
    if(WIN32)
        target_link_libraries(vove-update-tests PRIVATE bcrypt)
    endif()
    vove_copy_cxx_runtime(vove-update-tests)
    vove_enable_warnings(vove-update-tests)
    add_test(NAME vove-update-tests COMMAND vove-update-tests)
    if(WIN32)
        set_tests_properties(vove-update-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-main-menu-dialog-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/main_menu_dialog_test.cpp
        main_menu_dialog.cpp window_frame.cpp)
    target_include_directories(vove-main-menu-dialog-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-main-menu-dialog-tests PRIVATE Qt6::Widgets Qt6::Test)
    if(WIN32)
        target_link_libraries(vove-main-menu-dialog-tests PRIVATE dwmapi)
        target_compile_definitions(vove-main-menu-dialog-tests PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    endif()
    vove_copy_cxx_runtime(vove-main-menu-dialog-tests)
    vove_enable_warnings(vove-main-menu-dialog-tests)
    add_test(NAME vove-main-menu-dialog-tests COMMAND vove-main-menu-dialog-tests)
    set_tests_properties(vove-main-menu-dialog-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-main-menu-dialog-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-update-dialog-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/update_dialog_test.cpp
        update_dialog.cpp ui_text.cpp)
    target_link_libraries(vove-update-dialog-tests PRIVATE vove-update-qt Qt6::Widgets Qt6::Test)
    vove_copy_cxx_runtime(vove-update-dialog-tests)
    vove_enable_warnings(vove-update-dialog-tests)
    add_test(NAME vove-update-dialog-tests COMMAND vove-update-dialog-tests)
    set_tests_properties(vove-update-dialog-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    if(WIN32)
        set_tests_properties(vove-update-dialog-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-translation-catalog-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/translation_catalog_test.cpp)
    target_include_directories(vove-translation-catalog-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-translation-catalog-tests PRIVATE Qt6::Core)
    vove_copy_cxx_runtime(vove-translation-catalog-tests)
    vove_enable_warnings(vove-translation-catalog-tests)
    add_test(NAME vove-translation-catalog-tests
        COMMAND vove-translation-catalog-tests
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../translations/vove_ru.ts ru
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../translations/vove_zh_CN.ts zh-CN
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../translations/vove_ar.ts ar
            $<TARGET_FILE:Qt6::lupdate>
            ${CMAKE_CURRENT_SOURCE_DIR})
    if(WIN32)
        set_tests_properties(vove-translation-catalog-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-translation-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/translation_manager_test.cpp
        translation_manager.cpp
        translation_manager.hpp
        ui_text.cpp
        ui_text.hpp)
    target_include_directories(vove-translation-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-translation-tests PRIVATE Qt6::Core)
    add_dependencies(vove-translation-tests vove-translations)
    add_custom_command(TARGET vove-translation-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:vove-translation-tests>/translations
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${VOVE_TRANSLATION_QM_FILES}
            $<TARGET_FILE_DIR:vove-translation-tests>/translations
        VERBATIM)
    vove_copy_cxx_runtime(vove-translation-tests)
    vove_enable_warnings(vove-translation-tests)
    add_test(NAME vove-translation-tests
        COMMAND vove-translation-tests $<TARGET_FILE:Qt6::lrelease>)
    if(WIN32)
        set_tests_properties(vove-translation-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-window-placement-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/window_placement_test.cpp
        window_placement.cpp
        window_placement.hpp)
    target_include_directories(vove-window-placement-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-window-placement-tests PRIVATE Qt6::Core Qt6::Gui)
    vove_copy_cxx_runtime(vove-window-placement-tests)
    vove_enable_warnings(vove-window-placement-tests)
    add_test(NAME vove-window-placement-tests COMMAND vove-window-placement-tests)
    if(WIN32)
        set_tests_properties(vove-window-placement-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-external-application-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/external_application_registry_test.cpp
        external_application_registry.cpp
        external_application_registry.hpp)
    target_include_directories(vove-external-application-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-external-application-tests PRIVATE Qt6::Core)
    vove_copy_cxx_runtime(vove-external-application-tests)
    vove_enable_warnings(vove-external-application-tests)
    add_test(NAME vove-external-application-tests COMMAND vove-external-application-tests)
    if(WIN32)
        set_tests_properties(vove-external-application-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-startup-registration-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/startup_registration_test.cpp
        startup_registration.cpp
        startup_registration.hpp)
    target_include_directories(vove-startup-registration-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-startup-registration-tests PRIVATE Qt6::Core)
    vove_copy_cxx_runtime(vove-startup-registration-tests)
    vove_enable_warnings(vove-startup-registration-tests)
    add_test(NAME vove-startup-registration-tests COMMAND vove-startup-registration-tests)
    if(WIN32)
        set_tests_properties(vove-startup-registration-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-open-helper-fixture
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/open_helper_fixture.cpp)
    target_link_libraries(vove-open-helper-fixture PRIVATE Qt6::Core)
    vove_enable_warnings(vove-open-helper-fixture)

    add_executable(vove-components-panel-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/components_panel_test.cpp
        components_panel.cpp ui_text.cpp)
    target_include_directories(vove-components-panel-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-components-panel-tests PRIVATE Qt6::Widgets)
    vove_copy_cxx_runtime(vove-components-panel-tests)
    vove_enable_warnings(vove-components-panel-tests)
    add_test(NAME vove-components-panel-tests COMMAND vove-components-panel-tests)
    set_tests_properties(vove-components-panel-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-components-panel-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-directory-watch-fixture
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/directory_watch_fixture.cpp)
    target_link_libraries(vove-directory-watch-fixture PRIVATE Qt6::Core)
    vove_enable_warnings(vove-directory-watch-fixture)
    add_executable(vove-directory-monitor-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/directory_monitor_test.cpp
        directory_monitor.cpp)
    target_include_directories(vove-directory-monitor-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-directory-monitor-tests PRIVATE Qt6::Core)
    target_compile_definitions(vove-directory-monitor-tests PRIVATE
        "VOVE_TEST_OPEN_HELPER_PATH=\"$<TARGET_FILE:vove-open-helper>\""
        "VOVE_TEST_DIRECTORY_WATCH_FIXTURE_PATH=\"$<TARGET_FILE:vove-directory-watch-fixture>\"")
    add_dependencies(vove-directory-monitor-tests vove-open-helper vove-directory-watch-fixture)
    vove_copy_cxx_runtime(vove-directory-monitor-tests)
    vove_enable_warnings(vove-directory-monitor-tests)
    add_test(NAME vove-directory-monitor-tests COMMAND vove-directory-monitor-tests)
    set_tests_properties(vove-directory-monitor-tests PROPERTIES TIMEOUT 60)
    if(WIN32)
        set_tests_properties(vove-directory-monitor-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-external-open-coordinator-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/external_open_coordinator_test.cpp
        external_open_coordinator.cpp
        external_open_coordinator.hpp)
    target_include_directories(vove-external-open-coordinator-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-external-open-coordinator-tests PRIVATE Qt6::Core)
    target_compile_definitions(vove-external-open-coordinator-tests PRIVATE
        "VOVE_TEST_OPEN_HELPER_FIXTURE_PATH=\"$<TARGET_FILE:vove-open-helper-fixture>\"")
    add_dependencies(vove-external-open-coordinator-tests vove-open-helper-fixture)
    vove_copy_cxx_runtime(vove-external-open-coordinator-tests)
    vove_enable_warnings(vove-external-open-coordinator-tests)
    add_test(NAME vove-external-open-coordinator-tests
        COMMAND vove-external-open-coordinator-tests)
    if(WIN32)
        set_tests_properties(vove-external-open-coordinator-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-global-search-helper-fixture
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/search/global_search_helper_fixture.cpp)
    target_include_directories(vove-global-search-helper-fixture PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../search)
    target_link_libraries(vove-global-search-helper-fixture PRIVATE vove::search)
    vove_enable_warnings(vove-global-search-helper-fixture)

    add_executable(vove-global-search-client-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/global_search_client_test.cpp
        global_search_client.cpp
        global_search_client.hpp)
    target_include_directories(vove-global-search-client-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../search)
    target_link_libraries(vove-global-search-client-tests PRIVATE
        vove::search Qt6::Core)
    target_compile_definitions(vove-global-search-client-tests PRIVATE
        "VOVE_TEST_GLOBAL_SEARCH_HELPER_PATH=\"$<TARGET_FILE:vove-global-search-helper-fixture>\"")
    add_dependencies(vove-global-search-client-tests vove-global-search-helper-fixture)
    vove_copy_cxx_runtime(vove-global-search-client-tests)
    vove_enable_warnings(vove-global-search-client-tests)
    add_test(NAME vove-global-search-client-tests COMMAND vove-global-search-client-tests)
    if(WIN32)
        set_tests_properties(vove-global-search-client-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-batch-rename-dialog-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/batch_rename_dialog_test.cpp
        batch_rename_dialog.cpp
        batch_rename_dialog.hpp
        format_style.cpp
        format_style.hpp)
    target_include_directories(vove-batch-rename-dialog-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-batch-rename-dialog-tests PRIVATE vove::fileops Qt6::Widgets)
    vove_copy_cxx_runtime(vove-batch-rename-dialog-tests)
    vove_enable_warnings(vove-batch-rename-dialog-tests)
    add_test(NAME vove-batch-rename-dialog-tests COMMAND vove-batch-rename-dialog-tests)
    set_tests_properties(vove-batch-rename-dialog-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        set_tests_properties(vove-batch-rename-dialog-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-rename-dialog-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/rename_dialog_test.cpp
        rename_dialog.cpp
        rename_dialog.hpp
        format_style.cpp
        format_style.hpp)
    target_include_directories(vove-rename-dialog-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-rename-dialog-tests PRIVATE vove::fileops Qt6::Widgets)
    vove_copy_cxx_runtime(vove-rename-dialog-tests)
    vove_enable_warnings(vove-rename-dialog-tests)
    add_test(NAME vove-rename-dialog-tests COMMAND vove-rename-dialog-tests)
    set_tests_properties(vove-rename-dialog-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-rename-dialog-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-hanging-preview-helper
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/hanging_preview_helper.cpp)
    target_link_libraries(vove-hanging-preview-helper PRIVATE
        vove::preview_protocol Qt6::Core Qt6::Network)
    vove_enable_warnings(vove-hanging-preview-helper)

    add_executable(vove-qt-model-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/directory_list_model_test.cpp
        directory_list_model.cpp
        directory_list_model.hpp
    )
    target_include_directories(vove-qt-model-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-qt-model-tests PRIVATE vove::catalog vove::core Qt6::Test Qt6::Widgets)
    vove_copy_cxx_runtime(vove-qt-model-tests)
    vove_enable_warnings(vove-qt-model-tests)
    add_test(NAME vove-qt-model-tests COMMAND vove-qt-model-tests)
    if(WIN32)
        set_tests_properties(vove-qt-model-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-preview-client-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/preview_client_test.cpp
        diagnostic_sink.cpp
        diagnostic_sink.hpp
        preview_client.cpp
        preview_client.hpp
    )
    target_include_directories(vove-preview-client-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-preview-client-tests PRIVATE
        vove::color vove::platform vove::preview_protocol Qt6::Gui Qt6::Network Qt6::Test)
    add_dependencies(vove-preview-client-tests vove-preview-runtime-files)
    add_dependencies(vove-preview-client-tests vove-hanging-preview-helper)
    target_compile_definitions(vove-preview-client-tests PRIVATE
        "VOVE_TEST_HANGING_PREVIEW_HELPER_PATH=\"$<TARGET_FILE:vove-hanging-preview-helper>\"")
    vove_copy_cxx_runtime(vove-preview-client-tests)
    vove_enable_warnings(vove-preview-client-tests)
    add_test(NAME vove-preview-client-tests COMMAND vove-preview-client-tests)
    if(WIN32)
        set_tests_properties(vove-preview-client-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-preview-network-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/preview_network_recovery_test.cpp
        diagnostic_sink.cpp
        diagnostic_sink.hpp
        preview_client.cpp
        preview_client.hpp
    )
    target_include_directories(vove-preview-network-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-preview-network-tests PRIVATE
        vove::color vove::platform vove::preview_protocol Qt6::Gui Qt6::Network)
    add_dependencies(vove-preview-network-tests vove-preview-runtime-files)
    vove_copy_cxx_runtime(vove-preview-network-tests)
    vove_enable_warnings(vove-preview-network-tests)

    add_executable(vove-folder-mosaic-controller-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/folder_mosaic_controller_test.cpp
        diagnostic_sink.cpp
        diagnostic_sink.hpp
        folder_mosaic_controller.cpp
        folder_mosaic_controller.hpp
        preview_client.cpp
        preview_client.hpp
    )
    target_include_directories(vove-folder-mosaic-controller-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-folder-mosaic-controller-tests PRIVATE
        vove::catalog
        vove::core
        vove::color
        vove::platform
        vove::preview
        vove::preview_protocol
        Qt6::Gui
        Qt6::Network
    )
    add_dependencies(vove-folder-mosaic-controller-tests
        vove-catalog-helper vove-preview-runtime-files vove-hanging-preview-helper)
    target_compile_definitions(vove-folder-mosaic-controller-tests PRIVATE
        "VOVE_TEST_HANGING_PREVIEW_HELPER_PATH=\"$<TARGET_FILE:vove-hanging-preview-helper>\"")
    add_custom_command(TARGET vove-folder-mosaic-controller-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:vove-catalog-helper>
            $<TARGET_FILE_DIR:vove-folder-mosaic-controller-tests>
        VERBATIM)
    vove_copy_cxx_runtime(vove-folder-mosaic-controller-tests)
    vove_enable_warnings(vove-folder-mosaic-controller-tests)
    add_test(NAME vove-folder-mosaic-controller-tests
        COMMAND vove-folder-mosaic-controller-tests)
    if(WIN32)
        set_tests_properties(vove-folder-mosaic-controller-tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    add_executable(vove-qt-shell-tests
        components_panel.cpp
        components_panel.hpp
        about_dialog.cpp
        about_dialog.hpp
        lazy_directory_tree_model.cpp
        lazy_directory_tree_model.hpp
        drag_views.cpp
        drag_views.hpp
        scrollbar_style.cpp
        scrollbar_style.hpp
        smooth_scroll.cpp
        smooth_scroll.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/main_window_test.cpp
        batch_rename_dialog.cpp
        batch_rename_dialog.hpp
        rename_dialog.cpp
        rename_dialog.hpp
        object_transfer_queue.cpp
        main_menu_dialog.cpp
        main_menu_dialog.hpp
        main_window.cpp
        main_window.hpp
        startup_registration.cpp
        startup_registration.hpp
        directory_monitor.cpp
        directory_monitor.hpp
        window_controls.cpp
        window_controls.hpp
        window_frame.cpp
        window_frame.hpp
        navigation_button.cpp
        navigation_button.hpp
        page_button.cpp
        page_button.hpp
        interface_font.cpp
        interface_font.hpp
        round_button_artwork.hpp
        create_folder_button.cpp
        create_folder_button.hpp
        add_favorite_button.cpp
        add_favorite_button.hpp
        catalog_tool_button.cpp
        catalog_tool_button.hpp
        catalog_tool_artwork.hpp
        theme_switch.cpp
        toolbar_fields.cpp
        toolbar_fields.hpp
        theme_switch.hpp
        thumbnail_scale.cpp
        thumbnail_scale.hpp
        directory_list_model.cpp
        directory_list_model.hpp
        directory_item_delegate.cpp
        directory_item_delegate.hpp
        default_application_name.cpp
        default_application_name.hpp
        preview_modified_label.cpp
        preview_modified_label.hpp
        preview_name_label.cpp
        preview_name_label.hpp
        zoom_preview.cpp
        zoom_preview.hpp
        operation_notice.cpp
        operation_notice.hpp
        external_component_detection.cpp
        external_component_detection.hpp
        external_application_registry.cpp
        external_application_registry.hpp
        external_open_coordinator.cpp
        external_open_coordinator.hpp
        format_style.cpp
        format_style.hpp
        global_search_client.cpp
        global_search_client.hpp
        folder_mosaic_controller.cpp
        folder_mosaic_controller.hpp
        diagnostic_sink.cpp
        diagnostic_sink.hpp
        preview_client.cpp
        preview_client.hpp
        trash_dialog.cpp
        trash_dialog.hpp
        translation_manager.cpp
        translation_manager.hpp
        update_dialog.cpp
        update_dialog.hpp
        ui_text.cpp
        ui_text.hpp
        window_placement.cpp
        window_placement.hpp
    )
    qt_add_resources(vove-qt-shell-tests "vove-test-author-artwork"
        PREFIX "/artwork"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/artwork"
        FILES ${vove_author_artwork})
    qt_add_resources(vove-qt-shell-tests "vove-test-license"
        PREFIX "/licenses"
        BASE "${PROJECT_SOURCE_DIR}"
        FILES "${PROJECT_SOURCE_DIR}/LICENSE")
    add_dependencies(vove-qt-shell-tests vove-translations)
    add_custom_command(TARGET vove-qt-shell-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:vove-qt-shell-tests>/translations
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${VOVE_TRANSLATION_QM_FILES}
            $<TARGET_FILE_DIR:vove-qt-shell-tests>/translations
        VERBATIM)
    add_dependencies(vove-qt-shell-tests vove-catalog-helper vove-catalog-helper-fixture vove-fileop-helper
        vove-fileop-helper-fixture vove-global-search-helper-fixture
        vove-preview-runtime-files)
    add_dependencies(vove-qt-shell-tests vove-hanging-preview-helper vove-open-helper-fixture
        vove-open-helper)
    target_compile_definitions(vove-qt-shell-tests PRIVATE
        "VOVE_TEST_HANGING_PREVIEW_HELPER_PATH=\"$<TARGET_FILE:vove-hanging-preview-helper>\""
        "VOVE_TEST_CATALOG_HELPER_FIXTURE_PATH=\"$<TARGET_FILE:vove-catalog-helper-fixture>\""
        "VOVE_TEST_FILEOP_FIXTURE_PATH=\"$<TARGET_FILE:vove-fileop-helper-fixture>\""
        "VOVE_TEST_GLOBAL_SEARCH_HELPER_PATH=\"$<TARGET_FILE:vove-global-search-helper-fixture>\""
        "VOVE_TEST_OPEN_HELPER_FIXTURE_PATH=\"$<TARGET_FILE:vove-open-helper-fixture>\""
        "VOVE_TEST_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
        VOVE_TEST_TRASH_BLOCKING_LOADER=1
        VOVE_TEST_ALLOW_SYNTHETIC_INTERNAL_DRAG=1
        VOVE_UI_TEST_HOOKS=1)
    add_custom_command(TARGET vove-qt-shell-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:vove-catalog-helper>
            $<TARGET_FILE_DIR:vove-qt-shell-tests>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:vove-fileop-helper>
            $<TARGET_FILE_DIR:vove-qt-shell-tests>
        VERBATIM
    )
    vove_copy_cxx_runtime(vove-qt-shell-tests)
    target_include_directories(vove-qt-shell-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-qt-shell-tests PRIVATE
        vove::catalog
        vove::core
        vove::color
        vove::fileops
        vove::platform
        vove::preview
        vove::preview_protocol
        vove::search
        vove-update-qt
        Qt6::Network
        Qt6::Test
        Qt6::Widgets
    )
    target_include_directories(vove-qt-shell-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../search)
    if(WIN32)
        target_link_libraries(vove-qt-shell-tests PRIVATE shlwapi dwmapi)
        target_compile_definitions(vove-qt-shell-tests PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    endif()
    vove_enable_warnings(vove-qt-shell-tests)
    foreach(language ru en zh-CN ar)
        set(font_test "vove-startup-font-${language}-tests")
        add_test(NAME ${font_test} COMMAND vove-qt-shell-tests)
        set_tests_properties(${font_test} PROPERTIES TIMEOUT 30
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VOVE_FONT_STARTUP_ONLY=1;VOVE_FONT_STARTUP_LANGUAGE=${language}")
        if(WIN32)
            set_tests_properties(${font_test} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
        endif()
    endforeach()
    add_test(NAME vove-qt-shell-tests COMMAND vove-qt-shell-tests)
    set_tests_properties(vove-qt-shell-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        set_tests_properties(vove-qt-shell-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    add_test(NAME vove-settings-ui-tests COMMAND vove-qt-shell-tests)
    add_test(NAME vove-file-in-use-ui-tests COMMAND vove-qt-shell-tests)
    add_test(NAME vove-trash-error-ui-tests COMMAND vove-qt-shell-tests)
    set_tests_properties(vove-trash-error-ui-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VOVE_TRASH_ERROR_ONLY=1" TIMEOUT 60)
    if(WIN32)
        set_tests_properties(vove-trash-error-ui-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    set_tests_properties(vove-file-in-use-ui-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VOVE_FILE_IN_USE_UI_ONLY=1" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(vove-file-in-use-ui-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    set_tests_properties(vove-settings-ui-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VOVE_SETTINGS_UI_ONLY=1")
    if(WIN32)
        set_tests_properties(vove-settings-ui-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    get_target_property(vove_pane_test_sources vove-qt-shell-tests SOURCES)
    add_test(NAME vove-navigation-tests COMMAND vove-qt-shell-tests)
    add_test(NAME vove-mixed-transfer-ui-tests COMMAND vove-qt-shell-tests)
    add_test(NAME vove-search-dnd-ui-tests COMMAND vove-qt-shell-tests)
    set_tests_properties(vove-search-dnd-ui-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VOVE_SEARCH_DND_ONLY=1" TIMEOUT 240)
    if(WIN32)
        set_tests_properties(vove-search-dnd-ui-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    add_test(NAME vove-folder-rename-ui-tests COMMAND vove-qt-shell-tests)
    set_tests_properties(vove-folder-rename-ui-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VOVE_RENAME_CLIPBOARD_ONLY=1" TIMEOUT 120)
    add_test(NAME vove-clipboard-ui-tests COMMAND vove-qt-shell-tests)
    set_tests_properties(vove-clipboard-ui-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VOVE_RENAME_CLIPBOARD_ONLY=2" TIMEOUT 120)
    if(WIN32)
        set_tests_properties(vove-folder-rename-ui-tests vove-clipboard-ui-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    set_tests_properties(vove-mixed-transfer-ui-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VOVE_MIXED_TRANSFER_ONLY=1" TIMEOUT 240)
    if(WIN32)
        set_tests_properties(vove-mixed-transfer-ui-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    add_executable(vove-object-transfer-tests
        object_transfer_queue.cpp ${PROJECT_SOURCE_DIR}/tests/ui/object_transfer_queue_test.cpp)
    target_include_directories(vove-object-transfer-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vove-object-transfer-tests PRIVATE vove::fileops vove::platform Qt6::Core)
    target_compile_definitions(vove-object-transfer-tests PRIVATE
        VOVE_TEST_CATALOG_HELPER_PATH="$<TARGET_FILE:vove-catalog-helper>"
        VOVE_TEST_FILEOP_HELPER_PATH="$<TARGET_FILE:vove-fileop-helper>")
    add_dependencies(vove-object-transfer-tests vove-catalog-helper vove-fileop-helper)
    vove_copy_cxx_runtime(vove-object-transfer-tests)
    vove_enable_warnings(vove-object-transfer-tests)
    add_test(NAME vove-object-transfer-tests COMMAND vove-object-transfer-tests)
    set_tests_properties(vove-object-transfer-tests PROPERTIES TIMEOUT 240)
    if(WIN32)
        set_tests_properties(vove-object-transfer-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    set_tests_properties(vove-navigation-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VOVE_NAVIGATION_ONLY=1" TIMEOUT 90)
    if(WIN32)
        set_tests_properties(vove-navigation-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    add_test(NAME vove-modified-time-tests COMMAND vove-qt-shell-tests)
    set_tests_properties(vove-modified-time-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VOVE_MODIFIED_TIME_ONLY=1"
        TIMEOUT 60)
    if(WIN32)
        set_tests_properties(vove-modified-time-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    list(FILTER vove_pane_test_sources EXCLUDE REGEX "main_window_test[.]cpp$")
    add_executable(vove-recursive-catalog-ui-tests
        ${PROJECT_SOURCE_DIR}/tests/ui/recursive_catalog_test.cpp
        ${vove_pane_test_sources})
    foreach(vove_recursive_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES)
        get_target_property(vove_recursive_value vove-qt-shell-tests ${vove_recursive_property})
        set_property(TARGET vove-recursive-catalog-ui-tests PROPERTY ${vove_recursive_property} "${vove_recursive_value}")
    endforeach()
    target_compile_definitions(vove-recursive-catalog-ui-tests PRIVATE
        VOVE_TEST_CATALOG_HELPER_PATH="$<TARGET_FILE:vove-catalog-helper>"
        VOVE_TEST_FILEOP_HELPER_PATH="$<TARGET_FILE:vove-fileop-helper>")
    add_dependencies(vove-recursive-catalog-ui-tests vove-qt-shell-tests)
    vove_copy_cxx_runtime(vove-recursive-catalog-ui-tests)
    vove_enable_warnings(vove-recursive-catalog-ui-tests)
    add_test(NAME vove-recursive-catalog-ui-tests COMMAND vove-recursive-catalog-ui-tests)
    set_tests_properties(vove-recursive-catalog-ui-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
    if(WIN32)
        set_tests_properties(vove-recursive-catalog-ui-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    add_executable(vove-preview-pane-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/ui/preview_pane_test.cpp
        ${vove_pane_test_sources})
    foreach(vove_pane_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES)
        get_target_property(vove_pane_value vove-qt-shell-tests ${vove_pane_property})
        set_property(TARGET vove-preview-pane-tests PROPERTY ${vove_pane_property} "${vove_pane_value}")
    endforeach()
    target_link_libraries(vove-preview-pane-tests PRIVATE vove::preview_pipeline vove::qoi_codec)
    add_dependencies(vove-preview-pane-tests vove-preview-runtime-files vove-hanging-preview-helper
        vove-catalog-helper vove-fileop-helper vove-translations)
    vove_copy_cxx_runtime(vove-preview-pane-tests)
    vove_enable_warnings(vove-preview-pane-tests)
    add_test(NAME vove-preview-pane-tests COMMAND vove-preview-pane-tests)
    set_tests_properties(vove-preview-pane-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        set_tests_properties(vove-preview-pane-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()

    if(WIN32)
        set(vove_visual_capture_platform windows)
    else()
        set(vove_visual_capture_platform offscreen)
    endif()
    add_test(NAME vove-preview-caption-tests COMMAND vove-preview-pane-tests)
    set_tests_properties(vove-preview-caption-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=${vove_visual_capture_platform};VOVE_TEST_CAPTION_ONLY=1"
        TIMEOUT 60)
    if(WIN32)
        set_tests_properties(vove-preview-caption-tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
    endif()
    foreach(vove_visual_theme IN ITEMS
            "white:themeWhite"
            "vanilla:themeVanilla"
            "blue:themeBlue"
            "dark-gray:themeDarkGray")
        string(REPLACE ":" ";" vove_visual_theme_parts "${vove_visual_theme}")
        list(GET vove_visual_theme_parts 0 vove_visual_label)
        list(GET vove_visual_theme_parts 1 vove_visual_object)
        if(vove_visual_label STREQUAL "blue")
            set(vove_visual_test vove-visual-capture-smoke)
        else()
            set(vove_visual_test "vove-visual-capture-${vove_visual_label}")
        endif()
        add_test(NAME ${vove_visual_test} COMMAND vove-qt-shell-tests)
        set_tests_properties(${vove_visual_test} PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=${vove_visual_capture_platform};VOVE_VISUAL_CAPTURE_ONLY=1;VOVE_TEST_EXPECT_FOLDER_MOSAICS=4;VOVE_TEST_WIDTH=1480;VOVE_TEST_HEIGHT=1080;VOVE_TEST_CAPTURE_THUMBNAIL=160;VOVE_TEST_CAPTURE_THEME=${vove_visual_object};VOVE_TEST_SCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/visual-capture-${vove_visual_label}.png")
        if(WIN32)
            set_tests_properties(${vove_visual_test} PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
        endif()
    endforeach()

    foreach(vove_dpi_scale IN ITEMS 1.25 1.50)
        string(REPLACE "." "" vove_dpi_label "${vove_dpi_scale}")
        set(vove_dpi_test "vove-qt-layout-${vove_dpi_label}-tests")
        add_test(NAME ${vove_dpi_test} COMMAND vove-qt-shell-tests)
        set_tests_properties(${vove_dpi_test} PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${vove_dpi_scale};VOVE_LAYOUT_ONLY=1;VOVE_EXPECTED_DPR=${vove_dpi_scale}")
        if(WIN32)
            set_tests_properties(${vove_dpi_test} PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${VOVE_CXX_RUNTIME_DIR}")
        endif()
    endforeach()
endif()
