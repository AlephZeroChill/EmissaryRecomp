# libusb for SDL3's HIDAPI joystick drivers on Windows and macOS.
#
# The official GameCube adapter (WUP-028) is a vendor-specific USB device, not HID, so SDL3
# can only reach it through libusb - which the official SDL3 packages leave out. The vendored
# SDL3 build compiles libusb from the pinned upstream release and links it in statically.
# (On macOS no SDL joystick saw the adapter without it: IOKit's HID manager lists HID devices only.)

include(FetchContent)
FetchContent_Declare(libusb
  URL "https://github.com/libusb/libusb/releases/download/v${AURORA_LIBUSB_VERSION}/libusb-${AURORA_LIBUSB_VERSION}.tar.bz2"
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
# Upstream ships no CMakeLists.txt, so this only populates the source tree.
FetchContent_MakeAvailable(libusb)

set(_libusb_root "${libusb_SOURCE_DIR}/libusb")
set(_libusb_sources
  "${_libusb_root}/core.c"
  "${_libusb_root}/descriptor.c"
  "${_libusb_root}/hotplug.c"
  "${_libusb_root}/io.c"
  "${_libusb_root}/strerror.c"
  "${_libusb_root}/sync.c"
)
if (APPLE)
  # The release's own macOS configuration (Xcode/config.h) and its Darwin backend over IOKit.
  list(APPEND _libusb_sources
    "${_libusb_root}/os/darwin_usb.c"
    "${_libusb_root}/os/events_posix.c"
    "${_libusb_root}/os/threads_posix.c"
  )
  set(_libusb_config "${libusb_SOURCE_DIR}/Xcode")
else ()
  list(APPEND _libusb_sources
    "${_libusb_root}/os/events_windows.c"
    "${_libusb_root}/os/threads_windows.c"
    "${_libusb_root}/os/windows_common.c"
    "${_libusb_root}/os/windows_usbdk.c"
    "${_libusb_root}/os/windows_winusb.c"
  )
  set(_libusb_config "${CMAKE_CURRENT_LIST_DIR}/libusb")
endif ()
add_library(usb-1.0 STATIC ${_libusb_sources})
target_include_directories(usb-1.0
  PUBLIC "${_libusb_root}"
  PRIVATE "${_libusb_config}" "${_libusb_root}/os"
)
if (APPLE)
  target_link_libraries(usb-1.0 PUBLIC "-framework IOKit" "-framework CoreFoundation" "-framework Security")
endif ()
set_target_properties(usb-1.0 PROPERTIES UNITY_BUILD OFF)
if (APPLE)
  # An imported target over the library: SDL exports its static target, and an export may name an imported target,
  # where an alias of this one failed the generate step ("requires target usb-1.0 that is not in any export set").
  add_library(LibUSB::LibUSB INTERFACE IMPORTED GLOBAL)
  set_target_properties(LibUSB::LibUSB PROPERTIES INTERFACE_LINK_LIBRARIES usb-1.0
    INTERFACE_INCLUDE_DIRECTORIES "${_libusb_root}")
else ()
  add_library(LibUSB::LibUSB ALIAS usb-1.0)
endif ()

# SDL's FindLibUSB expects an installed copy. Satisfy its presence checks with this target
# instead: the alias pre-empts the imported target it would otherwise create, and the link
# probe is answered up front because the archive does not exist until build time.
set(LibUSB_INCLUDE_PATH "${_libusb_root}" CACHE PATH "" FORCE)
set(LibUSB_LIBRARY "usb-1.0" CACHE STRING "" FORCE)
set(HAVE_LIBUSB_H 1 CACHE INTERNAL "" FORCE)
set(SDL_HIDAPI_LIBUSB ON CACHE BOOL "" FORCE)
set(SDL_HIDAPI_LIBUSB_SHARED OFF CACHE BOOL "" FORCE)
