# The GUI classes are a library so the automated GUI tests can create and
# drive the real windows; NCSender.exe is just main.cpp on top.
add_library(ncui STATIC
    MainWindow.h            MainWindow.cpp
    MachinePanel.h          MachinePanel.cpp
    CodeView.h              CodeView.cpp
    Theme.h                 Theme.cpp
    ProfileEditorDialog.h   ProfileEditorDialog.cpp
    LogView.h               LogView.cpp
)
target_include_directories(ncui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Images used by the style sheet, built into the exe.
qt_add_resources(ncui "ncui_images"
    PREFIX "/"
    FILES
        images/chevron.png
        images/chevron@2x.png
        images/chevron-disabled.png
        images/chevron-disabled@2x.png
)
target_link_libraries(ncui PUBLIC nccore Qt6::Widgets)

# qt_add_executable (rather than add_executable) so that with a static Qt the
# Windows platform plugin and the native style are linked into the exe.
qt_add_executable(NCSender WIN32 main.cpp)
target_link_libraries(NCSender PRIVATE ncui)
target_compile_definitions(NCSender PRIVATE NCSENDER_VERSION="${PROJECT_VERSION}")

# Version information shown in File Explorer > Properties > Details.
if(WIN32)
    configure_file(NCSender.rc.in "${CMAKE_CURRENT_BINARY_DIR}/NCSender.rc" @ONLY)
    target_sources(NCSender PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/NCSender.rc")
endif()

if(NCSENDER_STANDALONE)
    # Native Windows 11 / Windows look; without it Qt falls back to Fusion.
    if(TARGET Qt6::QModernWindowsStylePlugin)
        qt_import_plugins(NCSender INCLUDE Qt6::QModernWindowsStylePlugin)
    elseif(TARGET Qt6::QWindowsVistaStylePlugin)
        qt_import_plugins(NCSender INCLUDE Qt6::QWindowsVistaStylePlugin)
    endif()
endif()

# Copy the Qt DLLs and plugins next to the exe so the bin folder can be
# copied to a shop PC as-is.
if(WIN32 AND NCSENDER_DEPLOY_QT)
    if(TARGET Qt6::windeployqt)
        set(_ncs_windeployqt "$<TARGET_FILE:Qt6::windeployqt>")
    else()
        get_target_property(_ncs_qmake Qt6::qmake IMPORTED_LOCATION)
        if(_ncs_qmake)
            get_filename_component(_ncs_qtbin "${_ncs_qmake}" DIRECTORY)
        endif()
        find_program(_ncs_windeployqt_path windeployqt HINTS "${_ncs_qtbin}" "${Qt6_DIR}/../../../bin")
        set(_ncs_windeployqt "${_ncs_windeployqt_path}")
    endif()

    if(_ncs_windeployqt)
        add_custom_command(TARGET NCSender POST_BUILD
            # Skip plugin types that would pull in Qt6Network (TUIO touch over
            # UDP, TLS backends, network-status monitor). The app never
            # uses the network and should not ship networking code.
            COMMAND "${_ncs_windeployqt}"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-system-dxc-compiler
                    --no-opengl-sw
                    --skip-plugin-types generic,networkinformation,tls
                    $<IF:$<CONFIG:Debug>,--debug,--release>
                    "$<TARGET_FILE:NCSender>"
            COMMENT "Copying Qt runtime next to NCSender.exe (windeployqt)"
            VERBATIM)
    else()
        message(WARNING "windeployqt not found - copy the Qt DLLs next to the exe yourself.")
    endif()
endif()
