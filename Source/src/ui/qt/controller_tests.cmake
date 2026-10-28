# Included by this directory's CMakeLists.txt after the regular test/helper targets.
include_guard(DIRECTORY)
if(NOT BUILD_TESTING OR NOT VOVE_BUILD_TESTS)
    return()
endif()

function(vove_configure_focused_controller_target target)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}"
        "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    vove_enable_warnings(${target})
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    endif()
endfunction()

function(vove_register_focused_controller_test target)
    vove_configure_focused_controller_target(${target})
    add_test(NAME ${target} COMMAND ${target} ${ARGN})
    # Existing helpers supply the Windows C++ runtime/PATH and conditional Qt plugin root.
    vove_set_qt_test_runtime(${target})
    vove_set_qt_image_plugins(${target})
    set_property(TEST ${target} APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(${target} PROPERTIES LABELS "ui-focused" TIMEOUT 60)
endfunction()

# WindowSettings uses search-root normalization and registry serialization, not shell UI.
set(vove_window_settings_test_sources
    window_settings.cpp
    external_application_registry.cpp
    ui_text.cpp)
add_executable(vove-window-settings-tests
    "${PROJECT_SOURCE_DIR}/tests/ui/window_settings_test.cpp"
    ${vove_window_settings_test_sources})
target_link_libraries(vove-window-settings-tests PRIVATE vove::search Qt6::Core)
vove_register_focused_controller_test(vove-window-settings-tests)

# Passive shell/widget closure, derived from window_shell.cpp and the worker's checker.
# No models, MainWindow, preview processes or file-command workflows are linked here.
set(vove_window_shell_test_sources
    window_shell.cpp
    add_favorite_button.cpp
    catalog_tool_button.cpp
    create_folder_button.cpp
    drag_views.cpp
    format_style.cpp
    interface_font.cpp
    main_menu_dialog.cpp
    navigation_button.cpp
    page_button.cpp
    preview_modified_label.cpp
    preview_name_label.cpp
    scrollbar_style.cpp
    smooth_scroll.cpp
    theme_switch.cpp
    thumbnail_scale.cpp
    toolbar_fields.cpp
    translation_manager.cpp
    ui_text.cpp
    window_controls.cpp
    window_frame.cpp
    window_placement.cpp
    zoom_preview.cpp)
add_executable(vove-window-shell-tests
    "${PROJECT_SOURCE_DIR}/tests/ui/window_shell_test.cpp"
    ${vove_window_shell_test_sources})
target_link_libraries(vove-window-shell-tests PRIVATE Qt6::Widgets)
if(WIN32)
    target_link_libraries(vove-window-shell-tests PRIVATE dwmapi ole32 shell32 uuid)
endif()
qt_add_resources(vove-window-shell-tests "vove-focused-shell-artwork"
    PREFIX "/artwork"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/artwork"
    FILES ${vove_author_artwork})
add_dependencies(vove-window-shell-tests vove-translations)
vove_register_focused_controller_test(vove-window-shell-tests
    "${CMAKE_CURRENT_BINARY_DIR}/translations")

# Selection/places tests share the common objects;
# compile only DropRouter twice so synthetic source authorization cannot leak to production.
set(vove_selection_places_test_sources
    selection_controller.cpp
    places_controller.cpp
    selection_command.cpp
    drag_views.cpp
    lazy_directory_tree_model.cpp
    directory_list_model.cpp
    external_application_registry.cpp
    external_open_coordinator.cpp
    default_application_name.cpp)
add_library(vove-selection-places-test-support STATIC ${vove_selection_places_test_sources})
vove_configure_focused_controller_target(vove-selection-places-test-support)
target_link_libraries(vove-selection-places-test-support PUBLIC
    vove::platform Qt6::Widgets Qt6::Network)
if(WIN32)
    target_link_libraries(vove-selection-places-test-support PUBLIC shlwapi ole32 shell32 uuid)
endif()

add_executable(vove-selection-places-drop-tests
    "${PROJECT_SOURCE_DIR}/tests/ui/selection_places_drop_controller_test.cpp"
    drop_router.cpp)
target_compile_definitions(vove-selection-places-drop-tests PRIVATE
    VOVE_TEST_ALLOW_SYNTHETIC_INTERNAL_DRAG=1)
target_link_libraries(vove-selection-places-drop-tests PRIVATE vove-selection-places-test-support)
vove_register_focused_controller_test(vove-selection-places-drop-tests)

add_executable(vove-selection-places-drop-production-tests
    "${PROJECT_SOURCE_DIR}/tests/ui/selection_places_drop_controller_test.cpp"
    drop_router.cpp)
target_link_libraries(vove-selection-places-drop-production-tests PRIVATE
    vove-selection-places-test-support)
vove_register_focused_controller_test(vove-selection-places-drop-production-tests
    --production-sources)

# Preview tests compile their reused UI objects from source,
# rather than depending on object paths or building the entire application executable.
set(vove_preview_controller_test_sources
    preview_controller.cpp
    diagnostic_sink.cpp
    directory_list_model.cpp
    folder_mosaic_controller.cpp
    gif_animation_client.cpp
    preview_client.cpp
    external_component_detection.cpp
    format_style.cpp
    preview_modified_label.cpp
    preview_name_label.cpp
    zoom_preview.cpp
    page_button.cpp)
add_executable(vove-preview-controller-tests
    "${PROJECT_SOURCE_DIR}/tests/ui/preview_controller_test.cpp"
    ${vove_preview_controller_test_sources})
target_link_libraries(vove-preview-controller-tests PRIVATE
    vove::catalog vove::core vove::color vove::platform vove::preview
    vove::preview_protocol vove::worker_protocol vove::qoi_codec
    Qt6::Widgets Qt6::Network)
target_compile_definitions(vove-preview-controller-tests PRIVATE
    VOVE_UI_TEST_HOOKS=1
    "VOVE_TEST_HANGING_PREVIEW_HELPER_PATH=\"$<TARGET_FILE:vove-hanging-preview-helper>\"")
add_dependencies(vove-preview-controller-tests vove-hanging-preview-helper)
vove_register_focused_controller_test(vove-preview-controller-tests)
set_tests_properties(vove-preview-controller-tests PROPERTIES TIMEOUT 90)

# Status tests require no catalog/backend implementation.
set(vove_status_presenter_test_sources status_presenter.cpp operation_notice.cpp)
add_executable(vove-status-presenter-tests
    "${PROJECT_SOURCE_DIR}/tests/ui/status_presenter_test.cpp"
    ${vove_status_presenter_test_sources})
target_link_libraries(vove-status-presenter-tests PRIVATE Qt6::Widgets)
target_compile_definitions(vove-status-presenter-tests PRIVATE VOVE_UI_TEST_HOOKS=1)
vove_register_focused_controller_test(vove-status-presenter-tests)

# FileCommands checker closure: workflow units, their dialogs/queue and drop/places inputs.
# The test receives platform-specific helper filenames via targets, including on Linux.
set(vove_file_command_controller_test_sources
    file_command_controller.cpp
    file_command_input_workflow.cpp
    file_command_mutation_workflow.cpp
    file_command_recovery_workflow.cpp
    file_command_transfer_workflow.cpp
    file_command_trash_workflow.cpp
    batch_rename_dialog.cpp
    rename_dialog.cpp
    format_style.cpp
    object_transfer_queue.cpp
    diagnostic_sink.cpp
    selection_command.cpp
    drop_router.cpp
    places_controller.cpp
    drag_views.cpp
    directory_list_model.cpp
    lazy_directory_tree_model.cpp)
add_executable(vove-file-command-controller-tests
    "${PROJECT_SOURCE_DIR}/tests/ui/file_command_controller_test.cpp"
    ${vove_file_command_controller_test_sources})
target_link_libraries(vove-file-command-controller-tests PRIVATE
    vove::fileops vove::platform vove::search Qt6::Widgets)
target_compile_definitions(vove-file-command-controller-tests PRIVATE VOVE_UI_TEST_HOOKS=1)
if(WIN32)
    target_link_libraries(vove-file-command-controller-tests PRIVATE ole32 shell32 uuid)
endif()
add_dependencies(vove-file-command-controller-tests vove-fileop-helper vove-catalog-helper)
vove_register_focused_controller_test(vove-file-command-controller-tests
    "$<TARGET_FILE:vove-fileop-helper>" "$<TARGET_FILE:vove-catalog-helper>")
set_tests_properties(vove-file-command-controller-tests PROPERTIES TIMEOUT 90)

set(vove_focused_controller_test_targets
    vove-window-settings-tests
    vove-window-shell-tests
    vove-selection-places-drop-tests
    vove-selection-places-drop-production-tests
    vove-preview-controller-tests
    vove-status-presenter-tests
    vove-file-command-controller-tests)
add_custom_target(vove-controller-focused-tests DEPENDS ${vove_focused_controller_test_targets})
