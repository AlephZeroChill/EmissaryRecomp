# Toolchain for Horizon (Nintendo Switch homebrew): Clang compiles against devkitPro's devkitA64
# (newlib, libstdc++) and libnx; devkitA64's g++ links with libnx's switch.specs.
#   cmake -DGAME=... -DNATIVE_ARGS=-DCMAKE_TOOLCHAIN_FILE=/path/to/Horizon.cmake -P build.cmake
set(DEVKITPRO "$ENV{DEVKITPRO}")
if(NOT DEVKITPRO)
    set(DEVKITPRO /opt/devkitpro)
endif()
set(ENV{DEVKITPRO} "${DEVKITPRO}")  # switch.specs reads it at link time
set(_hz_a64 "${DEVKITPRO}/devkitA64")
file(GLOB _hz_gcc_version LIST_DIRECTORIES true RELATIVE "${_hz_a64}/aarch64-none-elf/include/c++" "${_hz_a64}/aarch64-none-elf/include/c++/*")
set(_hz_cxx "${_hz_a64}/aarch64-none-elf/include/c++/${_hz_gcc_version}")

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(NINTENDO_SWITCH TRUE)
set(CMAKE_C_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_CXX_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_SYSROOT "${_hz_a64}/aarch64-none-elf")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "${DEVKITPRO}/portlibs/switch" "${DEVKITPRO}/libnx" "${_hz_a64}/aarch64-none-elf")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Cortex-A57. Native thread_local: Clang reads the thread pointer from tpidr_el0, which every thread
# sets to libnx's (runtime/src/platform/horizon.c, HorizonSetThreadPointer).
# _DEFAULT_SOURCE: newlib hides its POSIX and BSD declarations under Clang's strict -std= modes.
set(_hz_flags "-march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIE -ffunction-sections -fdata-sections -D__SWITCH__ -D_DEFAULT_SOURCE -isystem ${DEVKITPRO}/libnx/include -isystem ${DEVKITPRO}/portlibs/switch/include")
set(CMAKE_C_FLAGS_INIT "${_hz_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_hz_flags} -nostdinc++ -isystem ${_hz_cxx} -isystem ${_hz_cxx}/aarch64-none-elf -Wno-invalid-constexpr")
set(CMAKE_ASM_FLAGS_INIT "${_hz_flags}")
set(_hz_link "${_hz_a64}/bin/aarch64-none-elf-g++ -specs=${DEVKITPRO}/libnx/switch.specs -march=armv8-a+crc+crypto -fPIE")
# No <FLAGS>: those are Clang's compile flags, and g++ only links here.
set(CMAKE_C_LINK_EXECUTABLE "${_hz_link} <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> -L${DEVKITPRO}/portlibs/switch/lib -L${DEVKITPRO}/libnx/lib -lnx")
set(CMAKE_CXX_LINK_EXECUTABLE "${_hz_link} <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> -L${DEVKITPRO}/portlibs/switch/lib -L${DEVKITPRO}/libnx/lib -lnx")
set(HORIZON_ELF2NRO "${DEVKITPRO}/tools/bin/elf2nro")
set(HORIZON_NACPTOOL "${DEVKITPRO}/tools/bin/nacptool")
