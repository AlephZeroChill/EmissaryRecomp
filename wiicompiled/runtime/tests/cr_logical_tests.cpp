// Standalone: c++ -std=c++17 -Iruntime/include runtime/tests/cr_logical_tests.cpp -o /tmp/cr-logical-test
#include "isa/ppc_isa_cr.h"
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <random>
#include <iostream>

void require(bool condition) { if (!condition) std::abort(); }

template<class T> void CheckComparison(uint32_t cr, uint32_t xer, uint32_t field, T a, T b) {
    const uint32_t shift = 28 - 4 * field;
    const uint32_t nibble = (uint32_t(a < b) << 3) | (uint32_t(a > b) << 2) |
                            (uint32_t(a == b) << 1) | (MKW_LEAN_FLAGS ? 0u : (xer >> 31));
    const uint32_t expected = (cr & ~(15u << shift)) | (nibble << shift);
    require(PpcCrCompareBody(cr, xer, field, a, b) == expected);
}
static int32_t SignedBits(uint32_t value) {
    int32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
static void TestIntegerComparisonFields() {
    constexpr uint32_t values[] = {0, 1, 2, 0x7ffe, 0x7fff, 0x8000, 0xffff,
        0x10000, 0x7ffffffe, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff};
    for (const auto a : values) for (const auto b : values)
        for (uint32_t field = 0; field < 8; ++field)
            for (uint32_t cr : {0u, 0xffffffffu, 0x5a5aa5a5u})
                for (uint32_t xer : {0u, 0xffffffffu, 0x7fffffffu, 0x80000000u}) {
                    CheckComparison(cr, xer, field, a, b);
                    CheckComparison(cr, xer, field, SignedBits(a), SignedBits(b));
                }
    std::mt19937 random(0x3c12a45);
    for (unsigned n = 0; n < 1000000; ++n) {
        const uint32_t a = random(), b = n % 4 ? random() : a;
        const uint32_t cr = random(), xer = random(), field = random() % 8;
        CheckComparison(cr, xer, field, a, b);
        CheckComparison(cr, xer, field, SignedBits(a), SignedBits(b));
    }
    std::cout << "signed/unsigned CR compare fields: ok\n";
}

int main() {
    TestIntegerComparisonFields();
    // Truth tables ordered (A,B) = 00,01,10,11. PowerPC CR logical operations:
    // NOR, AND-complement, XOR, NAND, AND, equivalence, OR-complement, OR.
    constexpr uint8_t truth[8] = {0b0001,0b0100,0b0110,0b0111,0b1000,0b1001,0b1101,0b1110};
    for (unsigned op=0; op<8; ++op)
        for (unsigned a=0; a<2; ++a)
            for (unsigned b=0; b<2; ++b)
                for (unsigned dst=0; dst<32; ++dst) {
                    uint32_t before=0x5a5aa5a5;
                    before=PpcCrSetBitResident(before,4,a);
                    before=PpcCrSetBitResident(before,1,b);
                    const uint32_t after=PpcCrLogicalResident(before,op,dst,4,1);
                    const uint32_t bit=1u<<(31-dst);
                    require(((after & bit)!=0) == ((truth[op]>>(2*a+b))&1));
                    require((before & ~bit)==(after & ~bit));
                }
    // PPOnline lockChar: resume=false, lastCss=-1 must branch to selectedChar.
    uint32_t cr=0;
    SetCRResident(cr,0,0,int32_t(0),int32_t(0));
    SetCRResident(cr,0,1,int32_t(-1),int32_t(0));
    cr=PpcCrLogicalResident(cr,6,20,4,1);
    require(GetCRBitResident(cr,5,0));
}
