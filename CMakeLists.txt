cmake_minimum_required(VERSION 3.21)
project(AesaRadarSim VERSION 1.0.0 LANGUAGES CXX)

include(CTest)

option(RADAR_BUILD_APPLICATIONS
    "Build the Connext DDS radar_app and target_gen executables" ON)
option(RADAR_BUILD_UI_TESTS
    "Build the display-free ImGui operator-control regression" ON)

if(APPLE AND RADAR_BUILD_APPLICATIONS)
    # Native Metal UI renderer (Objective-C++).
    enable_language(OBJCXX)
endif()
if(WIN32 AND RADAR_BUILD_APPLICATIONS)
    enable_language(RC)
endif()

# ---------------------------------------------------------------------------
# AESA SPY-6 class radar simulation on RTI Connext DDS 7.7.0 LTS
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_library(radar_project_options INTERFACE)
target_compile_definitions(radar_project_options INTERFACE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>)

# --- Portable target-scenario and tracker cores ----------------------------
add_library(target_scenario_core STATIC
    src/target_gen/TargetScenario.cpp)
target_include_directories(target_scenario_core PUBLIC src/target_gen)
target_link_libraries(target_scenario_core PRIVATE radar_project_options)

# Offline tracker replay harness: production TrackerCore with no Connext.
add_executable(tracker_replay
    tests/tracker_replay.cpp
    src/radar_app/components/TrackerCore.cpp)
target_include_directories(tracker_replay PRIVATE src/radar_app/components)
target_link_libraries(tracker_replay PRIVATE radar_project_options)

# --- Optional graphics dependencies ----------------------------------------
set(RADAR_NEEDS_IMGUI OFF)
if(RADAR_BUILD_APPLICATIONS OR (BUILD_TESTING AND RADAR_BUILD_UI_TESTS))
    set(RADAR_NEEDS_IMGUI ON)
endif()

if(RADAR_NEEDS_IMGUI)
    include(FetchContent)

    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.90.5)
    FetchContent_Declare(implot
        GIT_REPOSITORY https://github.com/epezent/implot.git
        GIT_TAG        v0.16)
    FetchContent_MakeAvailable(imgui implot)

    add_library(imgui_core STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
    target_include_directories(imgui_core SYSTEM PUBLIC ${imgui_SOURCE_DIR})
    target_compile_options(imgui_core PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-nontrivial-memcall>)
    target_link_libraries(imgui_core PRIVATE radar_project_options)

    add_library(implot STATIC
        ${implot_SOURCE_DIR}/implot.cpp
        ${implot_SOURCE_DIR}/implot_items.cpp)
    target_include_directories(implot SYSTEM PUBLIC ${implot_SOURCE_DIR})
    target_link_libraries(implot PUBLIC imgui_core)
endif()

if(RADAR_BUILD_APPLICATIONS)
    # --- Locate RTI Connext -------------------------------------------------
    if(NOT DEFINED CONNEXTDDS_DIR AND DEFINED ENV{CONNEXTDDS_DIR})
        set(CONNEXTDDS_DIR "$ENV{CONNEXTDDS_DIR}")
    endif()
    if(NOT DEFINED CONNEXTDDS_DIR AND DEFINED ENV{NDDSHOME})
        set(CONNEXTDDS_DIR "$ENV{NDDSHOME}")
    endif()
    if(NOT CONNEXTDDS_DIR)
        message(FATAL_ERROR
            "RTI Connext DDS not found.\n"
            "Set CONNEXTDDS_DIR (or NDDSHOME) to the Connext 7.7.0 installation.")
    endif()
    file(TO_CMAKE_PATH "${CONNEXTDDS_DIR}" CONNEXTDDS_DIR)
    message(STATUS "Using RTI Connext DDS at: ${CONNEXTDDS_DIR}")

    list(APPEND CMAKE_MODULE_PATH "${CONNEXTDDS_DIR}/resource/cmake")
    # The Windows target package is consumed through its DLL import libraries.
    # Limit BUILD_SHARED_LIBS to FindRTIConnextDDS so GLFW remains static.
    if(WIN32)
        if(DEFINED BUILD_SHARED_LIBS)
            set(RADAR_SAVED_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
            set(RADAR_BUILD_SHARED_LIBS_WAS_DEFINED ON)
        else()
            set(RADAR_BUILD_SHARED_LIBS_WAS_DEFINED OFF)
        endif()
        set(BUILD_SHARED_LIBS ON)
    endif()
    find_package(RTIConnextDDS "7.7.0" REQUIRED COMPONENTS core)
    if(WIN32)
        if(RADAR_BUILD_SHARED_LIBS_WAS_DEFINED)
            set(BUILD_SHARED_LIBS "${RADAR_SAVED_BUILD_SHARED_LIBS}")
        else()
            unset(BUILD_SHARED_LIBS)
        endif()
    endif()

    # --- IDL -> Modern C++ types -------------------------------------------
    find_program(RTIDDSGEN_EXECUTABLE
        NAMES rtiddsgen rtiddsgen.bat
        HINTS "${CONNEXTDDS_DIR}/bin"
        REQUIRED)

    set(RADAR_IDL "${CMAKE_CURRENT_SOURCE_DIR}/idl/radar_types.idl")
    set(GEN_DIR   "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${GEN_DIR}")
    set(GEN_SOURCES
        "${GEN_DIR}/radar_types.cxx"
        "${GEN_DIR}/radar_typesPlugin.cxx")
    set(GEN_HEADERS
        "${GEN_DIR}/radar_types.hpp"
        "${GEN_DIR}/radar_typesPlugin.hpp")

    add_custom_command(
        OUTPUT ${GEN_SOURCES} ${GEN_HEADERS}
        COMMAND "${RTIDDSGEN_EXECUTABLE}" -language C++11 -replace
                -d "${GEN_DIR}" "${RADAR_IDL}"
        DEPENDS "${RADAR_IDL}"
        COMMENT "rtiddsgen: generating C++11 types from idl/radar_types.idl")

    add_library(radar_types STATIC ${GEN_SOURCES})
    target_include_directories(radar_types PUBLIC "${GEN_DIR}")
    target_link_libraries(radar_types PUBLIC
        RTIConnextDDS::cpp2_api radar_project_options)

    # --- GLFW and renderer backends ----------------------------------------
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4)
    FetchContent_MakeAvailable(glfw)

    set(IMGUI_BACKEND_SOURCES
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
    if(APPLE)
        list(APPEND IMGUI_BACKEND_SOURCES
            ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm)
    else()
        find_package(OpenGL REQUIRED)
        list(APPEND IMGUI_BACKEND_SOURCES
            ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    endif()

    add_library(imgui_backends STATIC ${IMGUI_BACKEND_SOURCES})
    target_include_directories(imgui_backends SYSTEM PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui_backends PUBLIC imgui_core glfw)
    if(APPLE)
        set_source_files_properties(
            ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    else()
        target_link_libraries(imgui_backends PUBLIC OpenGL::GL)
    endif()

    # --- Common DDS infrastructure ----------------------------------------
    add_library(radar_common INTERFACE)
    target_include_directories(radar_common INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/common")
    target_link_libraries(radar_common INTERFACE
        radar_types RTIConnextDDS::cpp2_api radar_project_options)

    # --- Radar simulation application -------------------------------------
    add_executable(radar_app
        src/radar_app/main.cpp
        src/radar_app/DataBus.hpp
        src/radar_app/components/ComponentBase.hpp
        src/radar_app/components/BeamScheduler.cpp
        src/radar_app/components/DetectionProcessor.cpp
        src/radar_app/components/TrackManager.cpp
        src/radar_app/components/TrackerCore.cpp
        src/radar_app/components/CalibrationMonitor.cpp
        src/radar_app/components/CommandHandler.cpp
        src/radar_app/components/HmiUi.cpp
        src/radar_app/ShipSimulator.cpp
        src/radar_app/ui/UiApp.cpp
        src/radar_app/ui/PpiView.cpp
        src/radar_app/ui/AScopeView.cpp
        src/radar_app/ui/BScopeView.cpp
        src/radar_app/ui/Panels.cpp)

    if(APPLE)
        target_sources(radar_app PRIVATE src/radar_app/ui/MetalContext.mm)
        set_source_files_properties(src/radar_app/ui/MetalContext.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set_target_properties(radar_app PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.aesaradarsim.radarapp"
            MACOSX_BUNDLE_BUNDLE_NAME "AesaRadarSim")
    elseif(WIN32)
        target_sources(radar_app PRIVATE
            src/radar_app/ui/windows/WindowsResources.rc)
        # WindowsResources.rc embeds the application manifest itself.
        target_link_options(radar_app PRIVATE /MANIFEST:NO)
        target_link_libraries(radar_app PRIVATE dbghelp)
    endif()

    target_include_directories(radar_app PRIVATE src/radar_app)
    target_link_libraries(radar_app PRIVATE
        radar_common imgui_core imgui_backends implot)
    if(APPLE)
        target_link_libraries(radar_app PRIVATE
            "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
    else()
        target_link_libraries(radar_app PRIVATE OpenGL::GL)
    endif()

    # --- Target generator application -------------------------------------
    add_executable(target_gen
        src/target_gen/main.cpp
        src/target_gen/TargetFleet.cpp
        src/target_gen/DiagnosticsInjector.cpp)
    target_include_directories(target_gen PRIVATE src/target_gen)
    target_link_libraries(target_gen PRIVATE radar_common target_scenario_core)

    if(APPLE)
        set_target_properties(radar_app target_gen PROPERTIES MACOSX_RPATH ON)
        target_compile_definitions(radar_app PRIVATE GL_SILENCE_DEPRECATION)
    endif()

    # Copy QoS beside each executable. TARGET_FILE_DIR handles VS config dirs.
    add_custom_command(TARGET radar_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/qos"
                "$<TARGET_FILE_DIR:radar_app>/qos"
        COMMENT "Copying QoS profiles beside radar_app")
    add_custom_command(TARGET target_gen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/qos"
                "$<TARGET_FILE_DIR:target_gen>/qos"
        COMMENT "Copying QoS profiles beside target_gen")

    install(TARGETS radar_app target_gen
        RUNTIME DESTINATION bin
        BUNDLE DESTINATION .)
    install(DIRECTORY qos/ DESTINATION bin/qos)

    if(WIN32)
        install(PROGRAMS
            scripts/windows/ensure-build.ps1
            scripts/windows/run-demo.cmd
            scripts/windows/run-demo.ps1
            scripts/windows/smoke-test.cmd
            scripts/windows/smoke-test.ps1
            DESTINATION scripts/windows)
        install(FILES
            docs/RUN_WINDOWS.md
            DESTINATION docs)
        install(FILES ConnextStudioDemo.md DESTINATION .)
        set(CPACK_GENERATOR "ZIP")
        set(CPACK_PACKAGE_FILE_NAME
            "AesaRadarSim-${PROJECT_VERSION}-windows-x64")
        set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
        set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
        include(CPack)
    endif()
endif()

# --- Fast, display-free regression coverage --------------------------------
if(BUILD_TESTING)
    if(RADAR_BUILD_UI_TESTS)
        add_executable(ui_controls_smoke
            tests/ui_controls_smoke.cpp
            src/radar_app/ui/AScopeView.cpp
            src/radar_app/ui/Panels.cpp)
        target_include_directories(ui_controls_smoke PRIVATE
            src/radar_app src/common)
        target_link_libraries(ui_controls_smoke PRIVATE
            imgui_core implot radar_project_options)
        add_test(NAME ui_controls_smoke COMMAND ui_controls_smoke)
        set_tests_properties(ui_controls_smoke PROPERTIES
            TIMEOUT 30 LABELS "smoke;regression;portable")
    endif()

    add_executable(target_scenario_regression
        tests/target_scenario_regression.cpp)
    target_link_libraries(target_scenario_regression PRIVATE
        target_scenario_core radar_project_options)

    add_test(NAME target_scenario_regression COMMAND target_scenario_regression)
    add_test(NAME tracker_replay_regression
        COMMAND tracker_replay 300 --self-test --quiet)
    set_tests_properties(
        target_scenario_regression tracker_replay_regression
        PROPERTIES TIMEOUT 30 LABELS "smoke;regression;portable")
endif()
