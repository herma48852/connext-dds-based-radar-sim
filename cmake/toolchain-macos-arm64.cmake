# Toolchain file: macOS on Apple Silicon (M1/M2/M3/M4)
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-macos-arm64.cmake \
#         -DCONNEXTDDS_DIR=/Applications/rti_connext_dds-7.7.0
#
# The Connext target architecture (e.g. arm64Darwin23clang16.0) is
# auto-detected by FindRTIConnextDDS.cmake from your installation.

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
