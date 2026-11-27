# Source-level fixes applied to the vendored SDL3 tree before it is built.
#
# Each patch is an exact string replacement, checked and idempotent: a tree that
# already carries the fix is left alone, a tree where the anchor text is missing
# (a different SDL version) stops the configure with a clear message instead of
# silently building without the fix.
#
# Used two ways:
#   - included by AuroraSDL3Provider.cmake, which calls aurora_sdl3_apply_patches()
#     on a pre-provided source tree (FETCHCONTENT_SOURCE_DIR_SDL);
#   - run as `cmake -DSDL_SOURCE_DIR=<dir> -P AuroraSDL3Patches.cmake` from the
#     FetchContent PATCH_COMMAND for a freshly extracted tarball.

function(_aurora_sdl3_replace file description old new)
  file(READ "${file}" _content)
  string(FIND "${_content}" "${new}" _already)
  if (NOT _already EQUAL -1)
    return()
  endif ()
  string(FIND "${_content}" "${old}" _anchor)
  if (_anchor EQUAL -1)
    message(FATAL_ERROR "aurora: SDL3 patch '${description}' does not apply to ${file}; "
      "the vendored SDL version changed, review AuroraSDL3Patches.cmake")
  endif ()
  string(REPLACE "${old}" "${new}" _content "${_content}")
  file(WRITE "${file}" "${_content}")
  message(STATUS "aurora: SDL3 patch applied: ${description}")
endfunction()

function(aurora_sdl3_apply_patches sdl_source_dir)
  set(_wii "${sdl_source_dir}/src/joystick/hidapi/SDL_hidapi_wii.c")
  if (NOT EXISTS "${_wii}")
    message(FATAL_ERROR "aurora: SDL3 source tree at ${sdl_source_dir} has no SDL_hidapi_wii.c")
  endif ()

  # Wii Remote: rebuild the joystick in place after an extension change.
  #
  # When a Nunchuk or Classic Controller is plugged in or pulled out, the driver
  # flags the joystick as disconnected so it can come back with the new name and
  # capabilities. But the HID device stays open, and HIDAPI only removes a
  # joystick-less device once its handle is closed, so nothing ever re-creates
  # the joystick: the remote is gone for good until the application toggles the
  # driver hint (which closes and re-opens the Bluetooth HID handle, something
  # some Windows Bluetooth stacks answer by dropping the link). Instead, when the
  # device has no joystick, probe the extension again (two bounded attempts, at
  # most once a second) and connect a new joystick on the still-open handle.
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (context field)"
[==[    Uint64 m_ulNextMotionPlusCheck;
    bool m_bDisconnected;
]==]
[==[    Uint64 m_ulNextMotionPlusCheck;
    bool m_bDisconnected;
    Uint64 m_ulNextReconnect; /* WiiCompiled: see HIDAPI_DriverWii_UpdateDevice */
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (bounded extension probe)"
[==[static EWiiExtensionControllerType ReadExtensionControllerType(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    EWiiExtensionControllerType eExtensionControllerType = k_eWiiExtensionControllerType_Unknown;
    const int MAX_ATTEMPTS = 20;
    int attempts = 0;
]==]
[==[static EWiiExtensionControllerType ReadExtensionControllerTypeAttempts(SDL_HIDAPI_Device *device, int MAX_ATTEMPTS)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    EWiiExtensionControllerType eExtensionControllerType = k_eWiiExtensionControllerType_Unknown;
    int attempts = 0;
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (probe wrapper)"
[==[static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
]==]
[==[static EWiiExtensionControllerType ReadExtensionControllerType(SDL_HIDAPI_Device *device)
{
    return ReadExtensionControllerTypeAttempts(device, 20);
}

static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (UpdateDevice)"
[==[    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        return false;
    }

    now = SDL_GetTicks();
]==]
[==[    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        /* WiiCompiled: the joystick was dropped (extension plugged or unplugged,
         * or a failed read) but the HID device is still open, and the device
         * list only removes a joystick-less device once its handle is closed.
         * Rebuild the joystick in place, so an extension change behaves like it
         * does on the console: the remote never goes away, it just changes
         * type. If the remote does not answer, keep the device and retry. */
        now = SDL_GetTicks();
        if (ctx->m_ulNextReconnect != 0 && now < ctx->m_ulNextReconnect) {
            return false;
        }
        ctx->m_ulNextReconnect = now + 1000;
        {
            EWiiExtensionControllerType type = ReadExtensionControllerTypeAttempts(device, 2);
            if (type == k_eWiiExtensionControllerType_Unknown) {
                return false;
            }
            ctx->m_bDisconnected = false;
            ctx->m_ulLastInput = now;
            ctx->m_ulNextReconnect = 0;
            ctx->m_eExtensionControllerType = type;
            UpdateDeviceIdentity(device);
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Reconnected joystick as %s", device->name);
            return HIDAPI_JoystickConnected(device, NULL);
        }
    }

    now = SDL_GetTicks();
]==])

  # Horizon (libnx homebrew): newlib's pthreads and POSIX clocks. Every other subsystem falls
  # back to SDL's dummy driver; the runtime polls libnx's pads into a virtual gamepad itself.
  _aurora_sdl3_replace("${sdl_source_dir}/CMakeLists.txt" "Horizon platform"
[==[elseif(N3DS)
  sdl_glob_sources("${SDL3_SOURCE_DIR}/src/main/n3ds/*.c")
]==]
[==[elseif(NINTENDO_SWITCH)
  set(SDL_THREAD_PTHREAD 1)
  set(SDL_THREAD_PTHREAD_RECURSIVE_MUTEX 1)
  sdl_glob_sources("${SDL3_SOURCE_DIR}/src/thread/pthread/*.c")
  set(HAVE_SDL_THREADS TRUE)
  set(SDL_TIMER_UNIX 1)
  sdl_glob_sources("${SDL3_SOURCE_DIR}/src/timer/unix/*.c")
  set(HAVE_SDL_TIMERS TRUE)
  set(SDL_TIME_UNIX 1)
  sdl_glob_sources("${SDL3_SOURCE_DIR}/src/time/unix/*.c")
  set(HAVE_SDL_TIME TRUE)
  set(SDL_FSOPS_POSIX 1)
  sdl_sources("${SDL3_SOURCE_DIR}/src/filesystem/posix/SDL_sysfsops.c")
  set(HAVE_SDL_FSOPS TRUE)
  # Vulkan through the dummy video driver (src/video/dummy/SDL_horizonvulkan.c), which also builds SDL_GPU's Vulkan backend.
  set(SDL_VIDEO_VULKAN 1)
  set(HAVE_VULKAN TRUE)
  sdl_sources("${SDL3_SOURCE_DIR}/src/video/dummy/SDL_horizonvulkan.c")
elseif(N3DS)
  sdl_glob_sources("${SDL3_SOURCE_DIR}/src/main/n3ds/*.c")
]==])
  _aurora_sdl3_replace("${sdl_source_dir}/src/dynapi/SDL_dynapi.h" "Horizon: no dynamic API"
[==[#elif defined(SDL_PLATFORM_3DS)
#define SDL_DYNAMIC_API 0 // devkitARM doesn't support dynamic linking
]==]
[==[#elif defined(SDL_PLATFORM_3DS)
#define SDL_DYNAMIC_API 0 // devkitARM doesn't support dynamic linking
#elif defined(__SWITCH__)
#define SDL_DYNAMIC_API 0 // devkitA64 doesn't support dynamic linking
]==])
  # newlib's struct tm has no tm_gmtoff; Horizon's libc runs in UTC.
  _aurora_sdl3_replace("${sdl_source_dir}/src/time/unix/SDL_systime.c" "Horizon: no tm_gmtoff"
[==[#if (_POSIX_VERSION >= 202405L) || (!defined(sun) && !defined(__sun))
        dt->utc_offset = (int)tm->tm_gmtoff;]==]
[==[#if defined(__SWITCH__)
        dt->utc_offset = 0;
#elif (_POSIX_VERSION >= 202405L) || (!defined(sun) && !defined(__sun))
        dt->utc_offset = (int)tm->tm_gmtoff;]==])
  # Horizon: Vulkan for the dummy video driver, from cmake/sdl-horizon.
  file(COPY_FILE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sdl-horizon/SDL_horizonvulkan.c"
       "${sdl_source_dir}/src/video/dummy/SDL_horizonvulkan.c" ONLY_IF_DIFFERENT)
  _aurora_sdl3_replace("${sdl_source_dir}/src/video/dummy/SDL_nullvideo.c" "Horizon: Vulkan on the dummy driver"
[==[    device->free = DUMMY_DeleteDevice;
]==]
[==[    device->free = DUMMY_DeleteDevice;
#if defined(__SWITCH__) && defined(SDL_VIDEO_VULKAN)
    {
        extern void HORIZON_SetupVulkan(SDL_VideoDevice *device);  // SDL_horizonvulkan.c
        HORIZON_SetupVulkan(device);
    }
#endif
]==])
  # Horizon: the dummy display is the VI layer, not a fake 1024x768 desktop. With the fake mode the window ends up
  # 1024x768 and the compositor stretches that 4:3 swapchain onto the 16:9 screen.
  _aurora_sdl3_replace("${sdl_source_dir}/src/video/dummy/SDL_nullvideo.c" "Horizon: display mode is the VI layer"
[==[    mode.w = 1024;
    mode.h = 768;
]==]
[==[    mode.w = 1024;
    mode.h = 768;
#if defined(__SWITCH__)
    {
        extern void HORIZON_DisplaySize(int *w, int *h);  // SDL_horizonvulkan.c
        HORIZON_DisplaySize(&mode.w, &mode.h);
    }
#endif
]==])
  # SDL_GPU skips Vulkan drivers that report no conformance version; on Horizon the one driver, Mesa NVK,
  # reports none, so that would leave no device at all.
  _aurora_sdl3_replace("${sdl_source_dir}/src/gpu/vulkan/SDL_gpu_vulkan.c" "Horizon: accept NVK"
[==[    if (!isConformant) {
        return 0;
    }
]==]
[==[#if !defined(__SWITCH__)
    if (!isConformant) {
        return 0;
    }
#else
    (void)isConformant;
#endif
]==])
  # The dummy filesystem's fallback would collide with the POSIX file operations' getcwd one.
  _aurora_sdl3_replace("${sdl_source_dir}/src/filesystem/dummy/SDL_sysfilesystem.c" "Horizon: POSIX current directory"
[==[char *SDL_SYS_GetCurrentDirectory(void)
{
    const char *base = SDL_GetBasePath();
    if (!base) {
        return NULL;
    }

    return SDL_strdup(base);
}
]==]
[==[#ifndef SDL_FSOPS_POSIX
char *SDL_SYS_GetCurrentDirectory(void)
{
    const char *base = SDL_GetBasePath();
    if (!base) {
        return NULL;
    }

    return SDL_strdup(base);
}
#endif
]==])
  # libnx's pthreads have no scheduling policies (newlib declares the functions; nothing defines them).
  _aurora_sdl3_replace("${sdl_source_dir}/src/thread/pthread/SDL_systhread.c" "Horizon: no pthread scheduling"
[==[bool SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority)
{
#ifdef SDL_PLATFORM_RISCOS]==]
[==[bool SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority)
{
#if defined(SDL_PLATFORM_RISCOS) || defined(__SWITCH__)]==])

  # Vulkan: no memory defragmentation while another thread records a command buffer.
  #
  # VULKAN_Submit defragments: it moves buffers and textures to new memory and re-points their containers, uniform
  # buffers included (uniformBufferForDefrag->buffer). It only skips allocations with pending transfers; render-pass
  # use pins nothing (VULKAN_INTERNAL_TrackBuffer counts the buffer, not its allocation). Aurora's presenter submits
  # (and so defragments) while its frame worker records the next frame on another command buffer, so the frame
  # worker can push uniforms or bind through a pointer that is being replaced (ThreadSanitizer: DefragmentMemory on
  # the presenter against PushUniformData, BindIndexBuffer, BindGraphicsDescriptorSets, FetchFramebuffer on the frame
  # worker). x86 sees the new buffer's fields before its pointer; ARM need not, which fits crashes seen only on the
  # Switch and on Apple-silicon Ryujinx (NULL in VULKAN_INTERNAL_AcquireUniformBufferFromPool). Count the command
  # buffers being recorded, gate new ones, and defragment only while the submitting one is the only one, from any
  # submit rather than only swapchain submits (those overlap recording most of the time, and allocations marked for
  # defrag stay out of service until it runs).
  set(_vk "${sdl_source_dir}/src/gpu/vulkan/SDL_gpu_vulkan.c")
  _aurora_sdl3_replace("${_vk}" "Vulkan defrag gate (renderer fields)"
[==[    // We don't want transfer commands to block each other,
    // but we want all transfers to block during defrag.
    SDL_RWLock *defragLock;
]==]
[==[    // We don't want transfer commands to block each other,
    // but we want all transfers to block during defrag.
    SDL_RWLock *defragLock;

    // WiiCompiled: command buffers acquired and not yet submitted or cancelled, and the gate VULKAN_Submit holds
    // while it defragments so that none starts meanwhile (see its defragmentation).
    SDL_Mutex *defragGate;
    SDL_AtomicInt recordingCommandBuffers;
]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan defrag gate (create)"
[==[    renderer->acquireCommandBufferLock = SDL_CreateMutex();
]==]
[==[    renderer->acquireCommandBufferLock = SDL_CreateMutex();
    renderer->defragGate = SDL_CreateMutex();
    SDL_SetAtomicInt(&renderer->recordingCommandBuffers, 0);
]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan defrag gate (destroy)"
[==[    SDL_DestroyMutex(renderer->acquireCommandBufferLock);
]==]
[==[    SDL_DestroyMutex(renderer->acquireCommandBufferLock);
    SDL_DestroyMutex(renderer->defragGate);
]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan defrag gate (acquire)"
[==[    SDL_UnlockMutex(renderer->acquireCommandBufferLock);

    commandBuffer->descriptorSetCache = descriptorSetCache;
]==]
[==[    SDL_UnlockMutex(renderer->acquireCommandBufferLock);

    // WiiCompiled: recording from here until submitted or cancelled; never starts while a submit defragments.
    SDL_LockMutex(renderer->defragGate);
    (void)SDL_AtomicIncRef(&renderer->recordingCommandBuffers);
    SDL_UnlockMutex(renderer->defragGate);

    commandBuffer->descriptorSetCache = descriptorSetCache;
]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan defrag gate (submitted)"
[==[static void VULKAN_INTERNAL_ReleaseCommandBuffer(VulkanCommandBuffer *vulkanCommandBuffer)
{
    VulkanRenderer *renderer = vulkanCommandBuffer->renderer;
]==]
[==[static void VULKAN_INTERNAL_ReleaseCommandBuffer(VulkanCommandBuffer *vulkanCommandBuffer)
{
    VulkanRenderer *renderer = vulkanCommandBuffer->renderer;

    (void)SDL_AtomicDecRef(&renderer->recordingCommandBuffers);  // WiiCompiled: submitted, no longer recording
]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan defrag gate (cancelled)"
[==[    vulkanCommandBuffer->autoReleaseFence = false;
    SDL_LockMutex(renderer->submitLock);
    VULKAN_INTERNAL_CleanCommandBuffer(renderer, vulkanCommandBuffer, true);
    SDL_UnlockMutex(renderer->submitLock);
]==]
[==[    vulkanCommandBuffer->autoReleaseFence = false;
    SDL_LockMutex(renderer->submitLock);
    VULKAN_INTERNAL_CleanCommandBuffer(renderer, vulkanCommandBuffer, true);
    SDL_UnlockMutex(renderer->submitLock);
    (void)SDL_AtomicDecRef(&renderer->recordingCommandBuffers);  // WiiCompiled: cancelled, no longer recording
]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan defrag gate (defragment alone)"
[==[    if (performCleanups &&
        renderer->allocationsToDefragCount > 0 &&
        !renderer->defragInProgress) {
        if (!VULKAN_INTERNAL_DefragmentMemory(renderer, vulkanCommandBuffer))
        {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "%s", "Failed to defragment memory, likely OOM!");
        }
    }
]==]
[==[    // WiiCompiled: only while this is the one command buffer being recorded, with the gate held so none starts
    // meanwhile (defragmentation re-points buffers another thread's recording may be using), and from any submit.
    if (renderer->allocationsToDefragCount > 0 &&
        !renderer->defragInProgress) {
        SDL_LockMutex(renderer->defragGate);
        if (SDL_GetAtomicInt(&renderer->recordingCommandBuffers) <= 1) {
            if (!VULKAN_INTERNAL_DefragmentMemory(renderer, vulkanCommandBuffer))
            {
                SDL_LogError(SDL_LOG_CATEGORY_GPU, "%s", "Failed to defragment memory, likely OOM!");
            }
        }
        SDL_UnlockMutex(renderer->defragGate);
    }
]==])

  # macOS: open libusb devices without detaching the kernel's driver first. The GameCube adapter (WUP-028) is a HID
  # interface that macOS's HID driver holds; libusb can detach it only with root or a device-access entitlement
  # (LIBUSB_ERROR_ACCESS), and hidapi gave the device up on that failure, so no controller on the adapter ever showed.
  # Claiming the interface with the driver still attached works on macOS (the adapter's reports read normally).
  _aurora_sdl3_replace("${sdl_source_dir}/src/hidapi/libusb/hid.c" "libusb: no kernel driver detach on macOS"
[==[#ifndef __FreeBSD__
#define DETACH_KERNEL_DRIVER
#endif]==]
[==[#if !defined(__FreeBSD__) && !defined(__APPLE__) /* WiiCompiled: see AuroraSDL3Patches.cmake */
#define DETACH_KERNEL_DRIVER
#endif]==])

  # GameCube adapter (WUP-028, and adapters in its Wii U mode): every stick and trigger byte as the controller sent it.
  # SDL stretched each axis from the range it had seen (at first 128 +-88) to the whole Sint16 range, so a byte of
  # 128+80 read as 128+116, and aurora's dead zone then took out everything under about 21 on each axis: a box
  # controller's modifier coordinates (and a stick's small values) reached the game unlike on Dolphin, which passes
  # the bytes through (Brawl Online board: "modifiers don't seem to work on emissary ... the coordinates are not the
  # same"). Now an axis is (byte - 128) * 256, the same for the triggers, and the joystick says so in its properties
  # ("WiiCompiled.gamecube.raw_axes"), where aurora's PADRead looks before it takes the bytes back (pad.cpp).
  set(_gc "${sdl_source_dir}/src/joystick/hidapi/SDL_hidapi_gamecube.c")
  _aurora_sdl3_replace("${_gc}" "GameCube adapter: raw axes"
[==[    axis_value = (Sint16)HIDAPI_RemapVal(curSlot[off], ctx->min_axis[i * SDL_GAMEPAD_AXIS_COUNT + axis], ctx->max_axis[i * SDL_GAMEPAD_AXIS_COUNT + axis], SDL_MIN_SINT16, SDL_MAX_SINT16); \]==]
[==[    axis_value = (Sint16)(((int)curSlot[off] - 128) * 256); /* WiiCompiled: the byte as is, AuroraSDL3Patches.cmake */ \]==])
  _aurora_sdl3_replace("${_gc}" "GameCube adapter: raw axes (property)"
[==[            joystick->nbuttons = 12;
            joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;
            if (ctx->wireless[i]) {]==]
[==[            joystick->nbuttons = 12;
            joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;
            if (!ctx->pc_mode) { /* WiiCompiled: axes are (byte - 128) * 256, AuroraSDL3Patches.cmake */
                SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), "WiiCompiled.gamecube.raw_axes", true);
            }
            if (ctx->wireless[i]) {]==])

  # Vulkan, a lost device (Brawl Online bug report #365 and the other device-lost reports, all NVIDIA on Vulkan):
  # - robustBufferAccess on wherever the device has it: a shader's read past the end of a bound buffer (aurora pulls
  #   vertices out of storage buffers, with indices the game's display lists give) reads zeros instead of faulting the
  #   GPU, which a driver answers by taking the device away. Direct3D 11 and 12 always check buffer views this way.
  #   The hint "WiiCompiled.vulkan.robust_buffer_access" = 0 turns it off (aurora's AURORA_GFX_FLAGS=norobust).
  # - VK_EXT_device_fault where the device has it (optional, as VK_KHR_driver_properties): after a loss, the driver
  #   can say what faulted and where (aurora gpu.cpp log_device_lost_details).
  # - The instance and device handles, and whether each of those is on, in the device's properties
  #   ("WiiCompiled.vulkan.*"), for aurora's own calls.
  _aurora_sdl3_replace("${_vk}" "Vulkan device fault (extension field)"
[==[    // Only required for decoding HDR ASTC textures
    Uint8 EXT_texture_compression_astc_hdr;
} VulkanExtensions;]==]
[==[    // Only required for decoding HDR ASTC textures
    Uint8 EXT_texture_compression_astc_hdr;
    // WiiCompiled: what faulted when the device is lost (AuroraSDL3Patches.cmake)
    Uint8 EXT_device_fault;
} VulkanExtensions;]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan device fault (extension check)"
[==[else CHECK(MSFT_layered_driver) else CHECK(EXT_texture_compression_astc_hdr)
]==]
[==[else CHECK(MSFT_layered_driver) else CHECK(EXT_texture_compression_astc_hdr) else CHECK(EXT_device_fault)
]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan device fault (extension count)"
[==[        supports->EXT_texture_compression_astc_hdr);]==]
[==[        supports->EXT_texture_compression_astc_hdr +
        supports->EXT_device_fault);]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan device fault (extension list)"
[==[    CHECK(EXT_texture_compression_astc_hdr)
#undef CHECK]==]
[==[    CHECK(EXT_texture_compression_astc_hdr)
    CHECK(EXT_device_fault)
#undef CHECK]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan robust buffer access"
[==[    if (haveDeviceFeatures.multiDrawIndirect) {
        features->desiredVulkan10DeviceFeatures.multiDrawIndirect = VK_TRUE;
        renderer->supportsMultiDrawIndirect = true;
    }
]==]
[==[    if (haveDeviceFeatures.multiDrawIndirect) {
        features->desiredVulkan10DeviceFeatures.multiDrawIndirect = VK_TRUE;
        renderer->supportsMultiDrawIndirect = true;
    }

    /* WiiCompiled: out-of-bounds buffer reads give zeros instead of a GPU fault (AuroraSDL3Patches.cmake) */
    if (haveDeviceFeatures.robustBufferAccess &&
        SDL_GetHintBoolean("WiiCompiled.vulkan.robust_buffer_access", true)) {
        features->desiredVulkan10DeviceFeatures.robustBufferAccess = VK_TRUE;
    }
]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan device fault (feature)"
[==[    vulkanResult = renderer->vkCreateDevice(
        renderer->physicalDevice,
        &deviceCreateInfo,
        NULL,
        &renderer->logicalDevice);]==]
[==[    VkPhysicalDeviceFaultFeaturesEXT wiiDeviceFault; /* WiiCompiled: AuroraSDL3Patches.cmake */
    if (renderer->supports.EXT_device_fault) {
        SDL_zero(wiiDeviceFault);
        wiiDeviceFault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
        wiiDeviceFault.pNext = (void *)deviceCreateInfo.pNext;
        wiiDeviceFault.deviceFault = VK_TRUE;
        deviceCreateInfo.pNext = &wiiDeviceFault;
    }

    vulkanResult = renderer->vkCreateDevice(
        renderer->physicalDevice,
        &deviceCreateInfo,
        NULL,
        &renderer->logicalDevice);]==])
  _aurora_sdl3_replace("${_vk}" "Vulkan handles in the device's properties"
[==[    if (!VULKAN_INTERNAL_CreateLogicalDevice(renderer, &features)) {
        SET_STRING_ERROR("Failed to create logical device!");
        SDL_free(renderer);
        SDL_Vulkan_UnloadLibrary();
        return NULL;
    }
]==]
[==[    if (!VULKAN_INTERNAL_CreateLogicalDevice(renderer, &features)) {
        SET_STRING_ERROR("Failed to create logical device!");
        SDL_free(renderer);
        SDL_Vulkan_UnloadLibrary();
        return NULL;
    }
    /* WiiCompiled: AuroraSDL3Patches.cmake */
    SDL_SetPointerProperty(renderer->props, "WiiCompiled.vulkan.instance", (void *)renderer->instance);
    SDL_SetPointerProperty(renderer->props, "WiiCompiled.vulkan.device", (void *)renderer->logicalDevice);
    SDL_SetBooleanProperty(renderer->props, "WiiCompiled.vulkan.device_fault", renderer->supports.EXT_device_fault != 0);
    SDL_SetBooleanProperty(renderer->props, "WiiCompiled.vulkan.robust_buffer_access",
                           features.desiredVulkan10DeviceFeatures.robustBufferAccess == VK_TRUE);
]==])

  # Direct3D 12, a removed device (Brawl Online bug report #369: an RTX 3060 Ti that Vulkan's recovery had moved to
  # D3D12 crashed three times in SDL at the driver's next reset, with exit code 1, and no recovery happened):
  # - A uniform buffer is mapped each time a command buffer takes one, and Map fails on a removed device, so
  #   D3D12_INTERNAL_AcquireUniformBufferFromPool gives NULL, which PushUniformData (read 0x8, writeOffset) and
  #   BindGraphicsResources (read 0x0, ->buffer) then read through on the frame worker. Without a uniform buffer a
  #   push does nothing and a bind leaves the slot alone: the device is gone and its commands are thrown away anyway.
  # - A failed Present set no error, so aurora logged "Failed to submit a command buffer: " and never knew the device
  #   was gone. It sets one now, and every error whose HRESULT is a device removal (DEVICE_REMOVED/HUNG/RESET,
  #   DRIVER_INTERNAL_ERROR) ends in "[DXGI_ERROR_DEVICE_REMOVED]": SDL replaces DEVICE_REMOVED by the removal
  #   reason, and its system text does not say that the device is gone. Aurora's names_lost_device looks for it.
  # - A command list that fails to close returned from D3D12_Submit still holding submitLock: every later submit
  #   waited for good.
  # - Test hook: WIICOMPILED_TEST_D3D12_REMOVE_DEVICE_AFTER_SUBMITS=N removes the device for real at the Nth submit
  #   (ID3D12Device5::RemoveDevice, which takes it away from this process only, as a driver reset does to all).
  set(_d3d12 "${sdl_source_dir}/src/gpu/d3d12/SDL_gpu_d3d12.c")
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (error names it)"
[==[    if (res == DXGI_ERROR_DEVICE_REMOVED) {
        if (renderer->device) {]==]
[==[    /* WiiCompiled: AuroraSDL3Patches.cmake */
    const char *wiiRemoved = "";
    if (res == (HRESULT)0x887A0005L || res == (HRESULT)0x887A0006L || res == (HRESULT)0x887A0007L ||
        res == (HRESULT)0x887A0020L) {
        wiiRemoved = " [DXGI_ERROR_DEVICE_REMOVED]";
    }

    if (res == DXGI_ERROR_DEVICE_REMOVED) {
        if (renderer->device) {]==])
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (error names it, code only)"
[==[        SDL_SetError("%s! Error Code: " HRESULT_FMT, msg, res);]==]
[==[        SDL_SetError("%s! Error Code: " HRESULT_FMT "%s", msg, res, wiiRemoved);]==])
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (error names it, with text)"
[==[    SDL_SetError("%s! Error Code: %s " HRESULT_FMT, msg, wszMsgBuff, res);]==]
[==[    SDL_SetError("%s! Error Code: %s " HRESULT_FMT "%s", msg, wszMsgBuff, res, wiiRemoved);]==])
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (present error)"
[==[        res = IDXGISwapChain_Present(
            windowData->swapchain,
            syncInterval,
            presentFlags);
        if (FAILED(res)) {
            result = false;
        }]==]
[==[        res = IDXGISwapChain_Present(
            windowData->swapchain,
            syncInterval,
            presentFlags);
        if (FAILED(res)) {
            D3D12_INTERNAL_SetError(renderer, "Failed to present", res); /* WiiCompiled: AuroraSDL3Patches.cmake */
            result = false;
        }]==])
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (close unlocks)"
[==[    res = ID3D12GraphicsCommandList_Close(d3d12CommandBuffer->graphicsCommandList);
    CHECK_D3D12_ERROR_AND_RETURN("Failed to close command list!", false);

    res = ID3D12GraphicsCommandList_QueryInterface(]==]
[==[    res = ID3D12GraphicsCommandList_Close(d3d12CommandBuffer->graphicsCommandList);
    if (FAILED(res)) { /* WiiCompiled: AuroraSDL3Patches.cmake */
        SDL_UnlockMutex(renderer->submitLock);
        CHECK_D3D12_ERROR_AND_RETURN("Failed to close command list!", false);
    }

    res = ID3D12GraphicsCommandList_QueryInterface(]==])
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (push without a uniform buffer)"
[==[        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Unrecognized shader stage!");
        return;
    }

    blockSize =]==]
[==[        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Unrecognized shader stage!");
        return;
    }

    if (uniformBuffer == NULL) { /* WiiCompiled: Map failed (a removed device), AuroraSDL3Patches.cmake */
        return;
    }

    blockSize =]==])
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (no next uniform buffer)"
[==[        uniformBuffer = D3D12_INTERNAL_AcquireUniformBufferFromPool(commandBuffer);

        uniformBuffer->drawOffset = 0;]==]
[==[        uniformBuffer = D3D12_INTERNAL_AcquireUniformBufferFromPool(commandBuffer);
        if (uniformBuffer == NULL) { /* WiiCompiled: AuroraSDL3Patches.cmake */
            if (shaderStage == SDL_GPU_SHADERSTAGE_VERTEX) {
                commandBuffer->vertexUniformBuffers[slotIndex] = NULL;
            } else if (shaderStage == SDL_GPU_SHADERSTAGE_FRAGMENT) {
                commandBuffer->fragmentUniformBuffers[slotIndex] = NULL;
            } else if (shaderStage == SDL_GPU_SHADERSTAGE_COMPUTE) {
                commandBuffer->computeUniformBuffers[slotIndex] = NULL;
            }
            return;
        }

        uniformBuffer->drawOffset = 0;]==])
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (vertex uniform bind)"
[==[            if (graphicsPipeline->header.num_vertex_uniform_buffers > i) {]==]
[==[            if (graphicsPipeline->header.num_vertex_uniform_buffers > i &&
                commandBuffer->vertexUniformBuffers[i] != NULL) { /* WiiCompiled */]==])
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (fragment uniform bind)"
[==[            if (graphicsPipeline->header.num_fragment_uniform_buffers > i) {]==]
[==[            if (graphicsPipeline->header.num_fragment_uniform_buffers > i &&
                commandBuffer->fragmentUniformBuffers[i] != NULL) { /* WiiCompiled */]==])
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (compute uniform bind)"
[==[            if (computePipeline->header.numUniformBuffers > i) {]==]
[==[            if (computePipeline->header.numUniformBuffers > i &&
                commandBuffer->computeUniformBuffers[i] != NULL) { /* WiiCompiled */]==])
  _aurora_sdl3_replace("${_d3d12}" "D3D12 removed device (test hook)"
[==[static bool D3D12_Submit(
    SDL_GPUCommandBuffer *commandBuffer)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12Renderer *renderer = d3d12CommandBuffer->renderer;
    ID3D12CommandList *commandLists[1];
    HRESULT res;

    SDL_LockMutex(renderer->submitLock);
]==]
[==[/* WiiCompiled test hook (AuroraSDL3Patches.cmake): WIICOMPILED_TEST_D3D12_REMOVE_DEVICE_AFTER_SUBMITS=N removes
   the device at the process's Nth submit, once. Called with submitLock held. */
static void D3D12_INTERNAL_WiiTestRemoveDevice(D3D12Renderer *renderer)
{
    static const IID wiiIIDDevice5 = { 0x8b4f173b, 0x2fea, 0x4b80, { 0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d } };
    static Sint64 after = -1;
    static Sint64 submits = 0;
    if (after < 0) {
        const char *value = SDL_getenv("WIICOMPILED_TEST_D3D12_REMOVE_DEVICE_AFTER_SUBMITS");
        after = value != NULL ? SDL_strtoll(value, NULL, 10) : 0;
    }
    if (after <= 0 || ++submits != after) {
        return;
    }
    ID3D12Device5 *device5 = NULL;
    if (SUCCEEDED(ID3D12Device_QueryInterface(renderer->device, &wiiIIDDevice5, (void **)&device5))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "WIICOMPILED_TEST_D3D12_REMOVE_DEVICE_AFTER_SUBMITS: removing the device");
        ID3D12Device5_RemoveDevice(device5);
        ID3D12Device5_Release(device5);
    }
}

static bool D3D12_Submit(
    SDL_GPUCommandBuffer *commandBuffer)
{
    D3D12CommandBuffer *d3d12CommandBuffer = (D3D12CommandBuffer *)commandBuffer;
    D3D12Renderer *renderer = d3d12CommandBuffer->renderer;
    ID3D12CommandList *commandLists[1];
    HRESULT res;

    SDL_LockMutex(renderer->submitLock);
    D3D12_INTERNAL_WiiTestRemoveDevice(renderer);
]==])
endfunction()

# Script mode (FetchContent PATCH_COMMAND).
if (CMAKE_SCRIPT_MODE_FILE AND DEFINED SDL_SOURCE_DIR)
  aurora_sdl3_apply_patches("${SDL_SOURCE_DIR}")
endif ()
