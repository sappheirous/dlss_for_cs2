#include "runtime.hpp"

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Initialization runs after loader-lock release; never wait for this worker in DllMain.
        if (const auto worker = CreateThread(nullptr, 0, &bootstrap, nullptr, 0, nullptr))
            CloseHandle(worker);
        else
            return FALSE;
    }

    return TRUE;
}
