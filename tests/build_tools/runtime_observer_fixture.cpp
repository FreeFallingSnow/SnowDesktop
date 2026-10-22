#include <windows.h>
#ifndef SNOWDESKTOP_RUNTIME_FIXTURE_VALUE
#define SNOWDESKTOP_RUNTIME_FIXTURE_VALUE 7
#endif
extern "C" __declspec(dllexport) int runtime_observer_value(){return SNOWDESKTOP_RUNTIME_FIXTURE_VALUE;}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
