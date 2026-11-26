#pragma once

#include <cstdint>
#include <string>

namespace aurora {
  void log_system_information();

  namespace system_info {
#if __APPLE__
    std::string getSystemVersionString();
#endif
#if _WIN32
    // The memory of its own of the DXGI adapter whose description is `name` (SDL_GPU's Direct3D 12 device name).
    bool dxgi_adapter_memory(const std::string& name, uint64_t& dedicatedBytes);
#endif
  }
}