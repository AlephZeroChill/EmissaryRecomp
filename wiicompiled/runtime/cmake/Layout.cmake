# Code layout from a clang frontend profile: hot code packed together, cold code out of the way. Changes placement and
# optimization decisions only, never what the code computes (exact-float flags are untouched; -ffp-contract=off etc.
# still apply to every translated TU).
#
#   MKW_PGO_GENERATE=ON       instrumented build (-fprofile-instr-generate). Linux/macOS/Windows hosts only: Horizon has
#                             no clang profile runtime. The pad script's EXIT writes the profile (hle/input/pad.cpp calls
#                             __llvm_profile_write_file before _Exit); set LLVM_PROFILE_FILE=<dir>/x-%p.profraw.
#   MKW_PGO_PROFILE=<file>    use a merged profile (llvm-profdata merge). The frontend hash is computed from each
#                             function's own AST, independent of the target, so one profile collected on x86 Linux serves
#                             the Switch (aarch64) build. Functions whose source changed since the profile (translator
#                             re-emit) silently fall back to non-PGO code; nothing is ever miscompiled by a stale profile.
#   MKW_SPLIT_FUNCTIONS=OFF   opt-in, with a profile, on ELF targets (Linux, Switch, Android): -fsplit-machine-functions
#                             moves the rarely/never-executed blocks of executed functions (checked-store slow paths, NaN
#                             resolution, interpreter fallbacks) into .text.split.<fn>: hot group 12.6 -> 5.3 MB. OFF by
#                             default: no measurable effect on the x86 test build (vanilla and P+ pairs within noise, in
#                             opposite directions), and without it the Switch build has no cross-section branches or x16
#                             spills. Worth re-measuring on Switch hardware (48 KB L1i, 2 MB L2), where density may count.
#   MKW_SECTION_ORDERING_FILE=<file>  GNU ld >= 2.43 --section-ordering-file (devkitA64 has 2.45.1; Ubuntu 24.04's host ld
#                             2.42 does not): .text.hot.<fn> statements, hottest first (tools: lay-order.py from the same
#                             profile). For the Switch the file must lead with KEEP (*(.crt0)): its statements go ahead
#                             of switch.ld's, and crt0 (entry branch, NRO0/MOD0 header) has to stay at offset 0. No gain
#                             on x86 (hotness order separates callers from callees); untested on hardware.
#
# Placement: with a profile, clang names sections .text.hot.<fn> (hot), .text.unlikely.<fn> (never executed / cold) and
# .text.split.<fn> (cold halves). GNU ld's default script and libnx's switch.ld both emit .text.unlikely*, then
# .text.hot*, then everything else, so all hot code ends up in one contiguous run without a custom linker script.
option(MKW_PGO_GENERATE "Instrumented build that writes a clang profile (-fprofile-instr-generate)" OFF)
set(MKW_SECTION_ORDERING_FILE "" CACHE FILEPATH "GNU ld --section-ordering-file for the hot functions; empty = off")
set(MKW_PGO_PROFILE "" CACHE FILEPATH "Merged clang profile (.profdata) for -fprofile-instr-use; empty = no PGO")
option(MKW_SPLIT_FUNCTIONS "With MKW_PGO_PROFILE: split cold blocks out of hot functions (ELF targets)" OFF)

if(MKW_PGO_GENERATE AND MKW_PGO_PROFILE)
    message(FATAL_ERROR "MKW_PGO_GENERATE and MKW_PGO_PROFILE are exclusive")
endif()
if(MKW_PGO_GENERATE)
    if(MKW_PLATFORM_SWITCH)
        message(FATAL_ERROR "MKW_PGO_GENERATE: collect the profile on a host build; Horizon has no profile runtime")
    endif()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-instr-generate>)
    add_compile_definitions($<$<COMPILE_LANGUAGE:C,CXX>:MKW_PGO_GENERATE=1>)
    add_link_options(-fprofile-instr-generate)
elseif(MKW_PGO_PROFILE)
    if(NOT EXISTS "${MKW_PGO_PROFILE}")
        message(FATAL_ERROR "MKW_PGO_PROFILE=${MKW_PGO_PROFILE} does not exist")
    endif()
    # Rebuild everything when the profile changes: CMake does not track files named only inside a flag, so its hash
    # goes into the command line (an unused definition; the AST and every profile hash stay the same).
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MKW_PGO_PROFILE}")
    file(MD5 "${MKW_PGO_PROFILE}" _mkw_pgo_md5)
    string(SUBSTRING "${_mkw_pgo_md5}" 0 12 _mkw_pgo_md5)
    # C/C++ only: the data-init blob (.S) goes through the same driver.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-instr-use=${MKW_PGO_PROFILE};-DMKW_PGO_PROFILE_ID=0x${_mkw_pgo_md5};-Wno-profile-instr-unprofiled;-Wno-profile-instr-out-of-date;-Wno-profile-instr-missing>")
    if(MKW_PLATFORM_LINUX OR MKW_PLATFORM_SWITCH OR MKW_PLATFORM_ANDROID)
        # One section per function, so the hot/unlikely prefixes apply per function (Horizon.cmake already has it).
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-ffunction-sections>)
        if(MKW_SPLIT_FUNCTIONS)
            add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-fsplit-machine-functions>)
        endif()
    endif()
    # Hot = any function with a block run in the top 99.99% of counts (>= ~1x per frame in a Training profile), not
    # LLVM's default 99%: frontend PGO counts every inline memory helper (MemoryInline::LeanHost, ByteSwap32, ...) on
    # every guest access, and those counts alone set the 99% threshold, leaving per-frame game code "warm" and scattered
    # among the split-off cold halves.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-mllvm -profile-summary-cutoff-hot=999900>")
endif()

if(MKW_SECTION_ORDERING_FILE)
    if(NOT EXISTS "${MKW_SECTION_ORDERING_FILE}")
        message(FATAL_ERROR "MKW_SECTION_ORDERING_FILE=${MKW_SECTION_ORDERING_FILE} does not exist")
    endif()
    add_link_options("-Wl,--section-ordering-file=${MKW_SECTION_ORDERING_FILE}")
    # Relink when the file changes (a file named only in a flag is not a dependency).
    function(_mkw_ordering_link_depends)
        foreach(_t WiiCompiled WiiCompiledLoader)
            if(TARGET ${_t})
                set_property(TARGET ${_t} APPEND PROPERTY LINK_DEPENDS "${MKW_SECTION_ORDERING_FILE}")
            endif()
        endforeach()
    endfunction()
    cmake_language(DEFER CALL _mkw_ordering_link_depends)
endif()
