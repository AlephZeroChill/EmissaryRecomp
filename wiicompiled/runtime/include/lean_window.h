// The lean guest-memory window's address space (memory_access.h, MKW_LEAN_MEMORY; Memory::Init in memory.cpp): one
// reservation, backed in parts, where some parts are views of one shared object so that several guest addresses reach
// the same bytes (MEM1's mirrors). The OS calls only: memory.cpp decides what goes where.
#pragma once

#include <cstdint>
#include <string>

namespace LeanWindow {

// `size` bytes of address space, nothing backed; null on failure. Windows: one placeholder, which later calls split
// and replace (Commit, MapShared). Horizon: the window platform/horizon.c set aside at start-up.
uint8_t* Reserve(uint64_t size);

// Private memory reading zero, read-write, over [at, at + size) of the reservation (64 KiB aligned, not backed yet):
// 0, or the OS's error code.
uint32_t Commit(uint8_t* at, uint64_t size);

// One new shared object of `size` bytes (64 KiB aligned), mapped read-write at each of `targets[0..count)` (64 KiB
// aligned, inside the reservation, not backed yet): every target shows the same bytes, which read zero. False, with
// `error` set, on failure or where the OS gives no way to map one object twice (Horizon: svcMapMemory takes its source
// pages away).
bool MapShared(uint64_t size, uint8_t* const* targets, int count, std::string* error);

}  // namespace LeanWindow
