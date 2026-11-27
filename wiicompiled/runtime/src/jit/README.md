# PoyoJIT

Like Kirby, it inhales whatever a mod brings and absorbs its moves natively. Poyo.

PoyoJIT is the run-time compiler for guest code that can't be statically recompiled. This includes the code a mod puts
in memory (Gecko code bodies, plugins, replaced modules) and every translated function a mod patches.

It compiles the cached interpreter's decoded blocks (`../ppc_interpreter.cpp`) to x86-64 at run time. Results should be
identical to the interpreter's, which stays as the reference, the fallback for instructions without native code, and the
engine wherever PoyoJIT is not built or is switched off.
