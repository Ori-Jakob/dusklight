// Separate TU: windows.h macros clash with decomp names.
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#endif

#include "autotest/State.hpp"

namespace twili::autotest::detail {

// A test crash must end the process, not wait on a dialog.
void suppressErrorDialogs() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

}  // namespace twili::autotest::detail
