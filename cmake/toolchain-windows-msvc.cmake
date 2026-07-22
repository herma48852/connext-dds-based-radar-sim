# Toolchain file: Windows 11, Visual Studio 2022 (MSVC v143, x64)
# Usage (Developer PowerShell / x64 Native Tools prompt):
#   cmake -B build -G "Visual Studio 17 2022" -A x64 ^
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-msvc.cmake ^
#         -DCONNEXTDDS_DIR="C:\Program Files\rti_connext_dds-7.7.0"
#
# Expected Connext target architecture (Connext 7.7.0 LTS):
#   x64Win64VS2017 (binary-compatible with VS2022)
#
# Notes for the Windows port:
#  - Dependencies (GLFW) are fetched with FetchContent, no vcpkg needed.
#  - If you prefer vcpkg for GLFW, pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#    instead and remove glfw from FetchContent in the root CMakeLists.txt.
#  - Connext DLLs must be on PATH when running (or copied next to the exe).

set(CMAKE_SYSTEM_NAME Windows)

# Connext Windows libraries are built with the dynamic CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
