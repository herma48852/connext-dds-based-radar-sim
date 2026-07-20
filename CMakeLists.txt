cmake_minimum_required(VERSION 3.20)
project(AesaRadarSim VERSION 1.0.0 LANGUAGES CXX)

if(APPLE)
    # Native Metal UI renderer (Objective-C++): replaces Apple's deprecated
    # OpenGL->Metal shim, the prime suspect in the windowed crashes.
    enable_language(OBJCXX)
endif()

# ---------------------------------------------------------------------------
# AESA SPY-6 class radar simulation on RTI Connext DDS 7.7.0 LTS
# macOS (Apple Silicon) first, Windows 11 / Visual Studio 2022 ready.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# --- Locate RTI Connext ----------------------------------------------------
if(NOT DEFINED CONNEXTDDS_DIR AND DEFINED ENV{CONNEXTDDS_DIR})
    set(CONNEXTDDS_DIR "$ENV{CONNEXTDDS_DIR}")
endif()
if(NOT DEFINED CONNEXTDDS_DIR AND DEFINED ENV{NDDSHOME})
    set(CONNEXTDDS_DIR "$ENV{NDDSHOME}")
endif()
if(NOT CONNEXTDDS_DIR)
    message(FATAL_ERROR
        "RTI Connext DDS not found.\n"
        "Set CONNEXTDDS_DIR (or NDDSHOME) to your Connext 7.7.0 installation, e.g.:\n"
        "  cmake -B build -DCONNEXTDDS_DIR=/Applications/rti_connext_dds-7.7.0")
endif()
message(STATUS "Using RTI Connext DDS at: ${CONNEXTDDS_DIR}")

list(APPEND CMAKE_MODULE_PATH "${CONNEXTDDS_DIR}/resource/cmake")
find_package(RTIConnextDDS "7.7.0" REQUIRED COMPONENTS core)

# --- IDL -> C++ (modern C++11 mapping, used from C++20 code) ---------------
# Explicit rtiddsgen invocation: transparent and identical on macOS/Windows.
find_program(RTIDDSGEN_EXECUTABLE
    NAMES rtiddsgen rtiddsgen.bat
    HINTS "${CONNEXTDDS_DIR}/bin"
    REQUIRED)

set(RADAR_IDL "${CMAKE_CURRENT_SOURCE_DIR}/idl/radar_types.idl")
set(GEN_DIR   "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
set(GEN_SOURCES "${GEN_DIR}/radar_types.cxx" "${GEN_DIR}/radar_typesPlugin.cxx")
set(GEN_HEADERS "${GEN_DIR}/radar_types.hpp" "${GEN_DIR}/radar_typesPlugin.hpp")

add_custom_command(
    OUTPUT  ${GEN_SOURCES} ${GEN_HEADERS}
    COMMAND "${RTIDDSGEN_EXECUTABLE}" -language C++11 -replace
            -d "${GEN_DIR}" "${RADAR_IDL}"
    DEPENDS "${RADAR_IDL}"
    COMMENT "rtiddsgen: generating C++11 types from idl/radar_types.idl")

add_library(radar_types STATIC ${GEN_SOURCES})
target_include_directories(radar_types PUBLIC "${GEN_DIR}")
target_link_libraries(radar_types PUBLIC RTIConnextDDS::cpp2_api)

# --- Graphics dependencies (Dear ImGui + GLFW + Metal[mac]/GL[other] + ImPlot)
if(NOT APPLE)
    find_package(OpenGL REQUIRED)
endif()
include(FetchContent)

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4)

# Dear ImGui has no upstream CMakeLists; we build it ourselves below.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.5)
FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.16)

FetchContent_MakeAvailable(glfw imgui implot)

set(IMGUI_BACKEND_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
if(APPLE)
    list(APPEND IMGUI_BACKEND_SOURCES
        ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm)
else()
    list(APPEND IMGUI_BACKEND_SOURCES
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${IMGUI_BACKEND_SOURCES})
if(APPLE)
    set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
# SYSTEM includes: warnings from third-party headers (e.g. clang's
# -Wnontrivial-memcall on imgui.h's memset constructors) must not drown
# out warnings from our own code.
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)
if(NOT APPLE)
    target_link_libraries(imgui PUBLIC OpenGL::GL)
endif()
# imgui's own .cpps include imgui.h with quoted includes, which resolve
# next to the source file (not via the SYSTEM path above), so silence the
# clang-only warning for the library's own translation units as well.
target_compile_options(imgui PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-nontrivial-memcall>)

add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp)
target_include_directories(implot SYSTEM PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui)

# --- Common DDS infrastructure (header-only) --------------------------------
add_library(radar_common INTERFACE)
target_include_directories(radar_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/common")
target_link_libraries(radar_common INTERFACE radar_types RTIConnextDDS::cpp2_api)

# --- Radar simulation application -------------------------------------------
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
endif()
target_include_directories(radar_app PRIVATE src/radar_app)
target_link_libraries(radar_app PRIVATE radar_common imgui implot glfw)
if(APPLE)
    target_link_libraries(radar_app PRIVATE
        "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
else()
    target_link_libraries(radar_app PRIVATE OpenGL::GL)
endif()

# --- Target generator application -------------------------------------------
add_executable(target_gen
    src/target_gen/main.cpp
    src/target_gen/TargetFleet.cpp
    src/target_gen/DiagnosticsInjector.cpp)
target_include_directories(target_gen PRIVATE src/target_gen)
target_link_libraries(target_gen PRIVATE radar_common)

# Offline tracker replay harness: replicates the beam raster, implant gates
# and CFAR peak-pick, and drives the production TrackerCore. No Connext
# required — build and run anywhere for fast tracker verification.
add_executable(tracker_replay
    tests/tracker_replay.cpp
    src/radar_app/components/TrackerCore.cpp)
target_include_directories(tracker_replay PRIVATE src/radar_app/components)

if(WIN32)
    target_compile_definitions(radar_app PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_definitions(target_gen PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

if(APPLE)
    # Connext on macOS uses @rpath; the runtime environment is set up by
    # sourcing rtisetenv_<arch>.bash (see README) or by DYLD_LIBRARY_PATH.
    set_target_properties(radar_app target_gen PROPERTIES MACOSX_RPATH ON)
    # macOS deprecates ALL of OpenGL (since 10.14); every GL app gets one
    # warning per call. Silence them in our sources (ImGui's GL backend
    # already defines this for itself).
    target_compile_definitions(radar_app PRIVATE GL_SILENCE_DEPRECATION)
endif()

# Make the QoS file discoverable next to the binaries.
add_custom_target(copy_qos ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/qos" "$<TARGET_FILE_DIR:radar_app>/qos"
    COMMENT "Copying QoS profiles")

install(TARGETS radar_app target_gen RUNTIME DESTINATION bin)
install(DIRECTORY qos/ DESTINATION bin/qos)
