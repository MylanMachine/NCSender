find_package(Qt6 REQUIRED COMPONENTS Test)

# Pure logic: formatting, profiles, profile file, log, file loading, channel basics.
add_executable(nccore_tests tst_core.cpp)
target_link_libraries(nccore_tests PRIVATE nccore Qt6::Test)
add_test(NAME nccore_tests COMMAND nccore_tests)

# Real transfers over the com0com pairs (skips itself if they are missing).
add_executable(nctransfer_tests tst_transfer.cpp)
target_link_libraries(nctransfer_tests PRIVATE nccore Qt6::Test)
add_test(NAME nctransfer_tests COMMAND nctransfer_tests)

# The real GUI (MainWindow, panels, profile editor) driven by QTest.
add_executable(ncui_tests tst_ui.cpp)
target_link_libraries(ncui_tests PRIVATE ncui Qt6::Test)
add_test(NAME ncui_tests COMMAND ncui_tests)

# windeployqt only deploys what NCSender.exe needs; the tests also need QtTest.
# The GUI tests draw offscreen, which needs the offscreen platform plugin.
if(WIN32)
    add_custom_command(TARGET nccore_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Test>" "$<TARGET_FILE_DIR:nccore_tests>"
        VERBATIM)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        add_custom_command(TARGET ncui_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ncui_tests>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
                    "$<TARGET_FILE_DIR:ncui_tests>/platforms"
            VERBATIM)
    endif()
endif()
