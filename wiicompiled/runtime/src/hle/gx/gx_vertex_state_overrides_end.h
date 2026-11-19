// No #pragma once: the end of a file that included gx_vertex_state_overrides.h. Restores hle_stubs.h's definitions
// (copied verbatim) for the files that follow it in the unity translation unit.
#undef PPC_NATIVE_OVERRIDE
#undef PPC_NATIVE_OVERRIDE_VOID

#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list) \
    extern "C" ret_type func_##addr_hex arg_list { return name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list) \
    extern "C" void func_##addr_hex arg_list { name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)
