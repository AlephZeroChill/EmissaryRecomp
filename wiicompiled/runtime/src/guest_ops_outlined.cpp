// Out-of-line guest operations (MKW_OUTLINE_GUEST_OPS, isa/ppc_isa_config.h): each function here is the inline Body
// its header entry point would otherwise expand at every translated call site, compiled once with the translated
// code's floating-point options (CMakeLists.txt: MKW_PPC_SEMANTIC_RUNTIME_SOURCES) and Clang's preserve_all
// convention (MKW_GUEST_OP_CC, from the header declarations). Same code, same results; the call sites shrink.
#include "memory_access.h"
#include "isa/ppc_isa_cr.h"
#include "isa/ppc_isa_float.h"

#if MKW_OUTLINE_GUEST_OPS

#if MKW_LEAN_MEMORY
namespace MemoryInline {
void LeanStore8(uint32_t address, uint8_t value) { LeanStoreBody<uint8_t>(address, value); }
void LeanStore16(uint32_t address, uint16_t value) { LeanStoreBody<uint16_t>(address, value); }
void LeanStore32(uint32_t address, uint32_t value) { LeanStoreBody<uint32_t>(address, value); }
void LeanStore64(uint32_t address, uint64_t value) { LeanStoreBody<uint64_t>(address, value); }
void LeanStoreRam8(uint32_t address, uint8_t value) { LeanStoreRamBody<uint8_t>(address, value); }
void LeanStoreRam16(uint32_t address, uint16_t value) { LeanStoreRamBody<uint16_t>(address, value); }
void LeanStoreRam32(uint32_t address, uint32_t value) { LeanStoreRamBody<uint32_t>(address, value); }
void LeanStoreRam64(uint32_t address, uint64_t value) { LeanStoreRamBody<uint64_t>(address, value); }
// FlatWriteFloat32's and FlatWriteRamFloat32's lean bodies: the Gekko narrowing, then the store.
void LeanStoreFloat32(uint32_t address, double value) {
    LeanStoreBody<uint32_t>(address, ConvertPpcDoubleToSingleBits(value));
}
void LeanStoreRamFloat32(uint32_t address, double value) {
    LeanStoreRamBody<uint32_t>(address, ConvertPpcDoubleToSingleBits(value));
}
double LeanLoadFloat32(uint32_t address) { return FlatReadFloat32Body(address); }
}  // namespace MemoryInline
#endif

float PpcForceSingleValueOutlined(double value) { return PpcForceSingleValueBody(value); }
double PpcFaddOutlined(double a, double b) { return PpcFaddBody(a, b); }
double PpcFsubOutlined(double a, double b) { return PpcFsubBody(a, b); }
double PpcFmulOutlined(double a, double b) { return PpcFmulBody(a, b); }
double PpcFdivOutlined(double a, double b) { return PpcFdivBody(a, b); }
double PpcFmulsOutlined(double a, double c) { return PpcFmulsBody(a, c); }
double PpcFmaddsOutlined(double a, double c, double b) { return PpcFmaddsBody(a, c, b); }
double PpcFmsubsOutlined(double a, double c, double b) { return PpcFmsubsBody(a, c, b); }
double PpcFnmaddsOutlined(double a, double c, double b) { return PpcFnmaddsBody(a, c, b); }
double PpcFnmsubsOutlined(double a, double c, double b) { return PpcFnmsubsBody(a, c, b); }

#endif

#if MKW_OUTLINE_CR_COMPARES
uint32_t PpcCrCompareSignedOutlined(uint32_t cr, uint32_t xer, uint32_t field, int32_t a, int32_t b) noexcept {
    return PpcCrCompareBody<int32_t>(cr, xer, field, a, b);
}
uint32_t PpcCrCompareUnsignedOutlined(uint32_t cr, uint32_t xer, uint32_t field, uint32_t a, uint32_t b) noexcept {
    return PpcCrCompareBody<uint32_t>(cr, xer, field, a, b);
}
uint32_t PpcCrCompareFloatOutlined(uint32_t cr, uint32_t field, double a, double b) noexcept {
    return PpcCrCompareFloatBody(cr, field, a, b);
}
uint32_t PpcCr0CompareSignedOutlined(uint32_t cr, uint32_t xer, int32_t a, int32_t b) noexcept {
    return PpcCrCompareBody<int32_t>(cr, xer, 0, a, b);
}
uint32_t PpcCr0CompareSignedZeroOutlined(uint32_t cr, uint32_t xer, int32_t a) noexcept {
    return PpcCrCompareBody<int32_t>(cr, xer, 0, a, 0);
}
uint32_t PpcCr0CompareUnsignedOutlined(uint32_t cr, uint32_t xer, uint32_t a, uint32_t b) noexcept {
    return PpcCrCompareBody<uint32_t>(cr, xer, 0, a, b);
}
uint32_t PpcCr0CompareUnsignedZeroOutlined(uint32_t cr, uint32_t xer, uint32_t a) noexcept {
    return PpcCrCompareBody<uint32_t>(cr, xer, 0, a, 0);
}
#endif
