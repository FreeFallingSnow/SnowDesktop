// Test: overlay HWND on desktop for ImGui
// Key: WS_EX_LAYERED + UpdateLayeredWindow for per-pixel alpha

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// 1. D3D11 device + swap chain for the overlay
// 2. Render ImGui to swap chain
// 3. Present to the layered window
// 4. Use UpdateLayeredWindow or DXGI alpha mode

int main() { return 0; }
