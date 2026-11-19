// No #pragma once: included at the top of each GX file whose entry points read or write the HLE vertex state
// (gx_vertex.cpp, gx_dl.cpp), after that file's other includes, and paired with gx_vertex_state_overrides_end.h at the
// file's end. The runtime's unity build compiles every GX file in one translation unit (unity_gx_bridge), so without
// the end header the hook below would stay on for the files that follow (fog, Z mode, TEV, textures...), and each of
// their calls would take the vertex state back early.
//
// While the draw worker carries the vertex state (gx_internal.h GX_HLE_MaterializeIfPending; g3d_native.cpp), the
// game thread's copy is stale: these files' override thunks take it back first, and the thunk is what gets registered,
// so a call through the dispatch table takes it back too. Direct calls bound by name (runtime/include/
// native_cpu_calls.inc: InvokeDirectCpu reaches the HLE function, not the thunk) skip it: the vertex-state entry
// points listed there (GX__Begin_HLE, GX__SetVtxDesc_HLE, GX__ClearVtxDesc_HLE, GX__SetVtxAttrFmt_HLE) handle it in
// their bodies too. A function marked GX_THUNK_SKIPS_TAKEBACK (gx_internal.h) before its override handles the carry
// itself (the setters' rewrite tracking, GXBegin's check) or touches no vertex state: its thunk does not take the state
// back. Same macro names and arguments as hle_stubs.h (the translator parses them); only the bodies differ.
#include "gx_internal.h"

#undef PPC_NATIVE_OVERRIDE
#undef PPC_NATIVE_OVERRIDE_VOID

#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list)                                    \
    extern "C" ret_type func_##addr_hex arg_list {                                                           \
        if constexpr (!GxThunkSkipsTakeBack<&name>::value) GX_HLE_MaterializeIfPending();                    \
        return name call_list;                                                                                \
    }                                                                                                         \
    REGISTER_NATIVE_FUNCTION_AS(0x##addr_hex, func_##addr_hex, #name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list)                                         \
    extern "C" void func_##addr_hex arg_list {                                                               \
        if constexpr (!GxThunkSkipsTakeBack<&name>::value) GX_HLE_MaterializeIfPending();                    \
        name call_list;                                                                                       \
    }                                                                                                         \
    REGISTER_NATIVE_FUNCTION_AS(0x##addr_hex, func_##addr_hex, #name)
