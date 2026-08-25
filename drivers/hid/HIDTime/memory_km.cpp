
#include <wdm.h>

// These should be unreachable - they exist to satisfy a linker requirement that comes up when using nontrivial destructors,
// but we don't have any implementations of non-placement operator new and don't call operator delete so they should be optimized
// out in release builds.  Because we have LCTG turned on, that optimization doesn't happen until link time, though.
void __cdecl
operator delete(void *, size_t)
{
    __fastfail(FAST_FAIL_INVALID_ARG);
}
void __cdecl
operator delete(void *)
{
    __fastfail(FAST_FAIL_INVALID_ARG);
}