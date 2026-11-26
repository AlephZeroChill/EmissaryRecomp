add_library(aurora_core STATIC
        lib/aurora.cpp
        lib/input.cpp
        lib/window.cpp
        lib/logging.cpp
        lib/system_info.cpp
        lib/system_info.hpp
)
add_library(aurora::core ALIAS aurora_core)
set_target_properties(aurora_core PROPERTIES FOLDER "aurora")

target_compile_definitions(aurora_core PUBLIC AURORA TARGET_PC)
target_include_directories(aurora_core PUBLIC include)
target_link_libraries(aurora_core PUBLIC fmt::fmt ${AURORA_SDL3_TARGET} xxhash)
target_link_libraries(aurora_core PRIVATE absl::btree absl::flat_hash_map $<$<BOOL:${TRACY_ENABLE}>:TracyClient>)

if (CMAKE_SYSTEM_NAME STREQUAL Windows)
    # stuff for fetching system info.
    target_link_libraries(aurora_core PRIVATE ntdll dxgi advapi32 user32)
elseif (APPLE)
    target_sources(aurora_core PRIVATE lib/system_info_mac.mm)
    target_link_libraries(aurora_core PRIVATE "-framework Foundation")
endif ()

if (AURORA_ENABLE_GX)
    target_sources(aurora_core PRIVATE lib/imgui.cpp)
    target_link_libraries(aurora_core PUBLIC imgui)
endif ()

if (AURORA_ENABLE_GX)
    target_compile_definitions(aurora_core PUBLIC AURORA_ENABLE_GX)
    target_sources(aurora_core PRIVATE lib/gpu/gpu.cpp lib/gpu/shader.cpp)
    target_link_libraries(aurora_core PRIVATE glslang glslang-default-resource-limits)
    if (CMAKE_SYSTEM_NAME STREQUAL Windows)
        # Direct3D 12: SPIR-V -> HLSL (SPIRV-Cross) -> DXBC (d3dcompiler_47.dll, loaded when first needed).
        target_sources(aurora_core PRIVATE lib/gpu/shader_d3d12.cpp)
        target_link_libraries(aurora_core PRIVATE spirv-cross-hlsl spirv-cross-glsl spirv-cross-core)
        target_compile_definitions(aurora_core PRIVATE AURORA_GPU_D3D12_SHADERS)
    endif ()
endif ()
