# AURORA_GPU_SDLGPU: implement the WebGPU calls aurora makes on SDL_GPU (lib/sdlgpu/webgpu_sdl.cpp) instead of
# linking Dawn. Dawn's headers stay the API definition; WGSL is translated by naga (lib/sdlgpu/wgsl_xlate, Rust).
# Sets AURORA_WEBGPU_TARGET to the library every WebGPU user links.
option(AURORA_GPU_SDLGPU "Implement aurora's WebGPU calls on SDL_GPU instead of linking Dawn" OFF)

if (AURORA_GPU_SDLGPU)
  find_program(AURORA_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
  set(_xlate_dir "${CMAKE_CURRENT_LIST_DIR}/../lib/sdlgpu/wgsl_xlate")
  set(_xlate_target "${CMAKE_BINARY_DIR}/wgsl_xlate")
  set(_xlate_lib "${_xlate_target}/release/${CMAKE_STATIC_LIBRARY_PREFIX}wgsl_xlate${CMAKE_STATIC_LIBRARY_SUFFIX}")
  add_custom_command(
      OUTPUT "${_xlate_lib}"
      COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${_xlate_target}"
              "${AURORA_CARGO}" build --release --manifest-path "${_xlate_dir}/Cargo.toml"
      DEPENDS "${_xlate_dir}/Cargo.toml" "${_xlate_dir}/src/lib.rs"
      COMMENT "Building the naga WGSL translator"
      VERBATIM)
  add_custom_target(aurora_wgsl_xlate_build DEPENDS "${_xlate_lib}")

  add_library(aurora_webgpu_sdl STATIC "${CMAKE_CURRENT_LIST_DIR}/../lib/sdlgpu/webgpu_sdl.cpp")
  add_dependencies(aurora_webgpu_sdl aurora_wgsl_xlate_build)
  target_compile_features(aurora_webgpu_sdl PRIVATE cxx_std_20)
  target_include_directories(aurora_webgpu_sdl PUBLIC
      $<TARGET_PROPERTY:dawn::webgpu_dawn,INTERFACE_INCLUDE_DIRECTORIES>)
  target_link_libraries(aurora_webgpu_sdl PUBLIC ${AURORA_SDL3_TARGET} "${_xlate_lib}")
  if (APPLE)
    # Dawn's imported target used to bring these in; aurora's macOS sources (system_info_mac.mm) need them too.
    target_link_libraries(aurora_webgpu_sdl PUBLIC "-framework Foundation" "-framework Metal" "-framework QuartzCore")
  endif ()
  set(AURORA_WEBGPU_TARGET aurora_webgpu_sdl CACHE INTERNAL "")
else ()
  set(AURORA_WEBGPU_TARGET dawn::webgpu_dawn CACHE INTERNAL "")
endif ()
