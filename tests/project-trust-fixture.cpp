#include <windows.h>

#ifndef PROJECT_TRUST_FIXTURE_VALUE
#define PROJECT_TRUST_FIXTURE_VALUE 42
#endif

extern "C" __declspec(dllexport) int projectTrustFixture()
{
    return PROJECT_TRUST_FIXTURE_VALUE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}

