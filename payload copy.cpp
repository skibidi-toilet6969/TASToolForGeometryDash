#include <windows.h>
#include <iostream>
#include <fstream>
#include "ui.h"

void InitUIThread();
void ShutdownUI();
bool& GetIsPaused();
bool& GetStepOnce();
bool& GetIsCounting(); // New getter for independent counter state
int& GetframeCount();
void ResetFrameCount();

#define THISCALL __attribute__((thiscall))

// --- Scheduler Hook (Physics & Frames) ---
typedef void(THISCALL *CCScheduler_Update_t)(void* self, float dt);
CCScheduler_Update_t oCCScheduler_Update = nullptr;
BYTE g_trampolineUpdate[16];

// --- PlayLayer::resetLevel Hook (Death / Level Restart) ---
typedef void(THISCALL *PlayLayer_ResetLevel_t)(void* self);
PlayLayer_ResetLevel_t oPlayLayer_ResetLevel = nullptr;
BYTE g_trampolineResetLevel[16];

// Hook for Physics Update
void THISCALL hkCCScheduler_Update(void* self, float dt) {
    if (GetIsPaused()) {
        if (GetStepOnce()) {
            GetStepOnce() = false;
            oCCScheduler_Update(self, 1.0f / 240.0f);
            if (GetIsCounting()) {
                GetframeCount()++;
            }
        } else {
            oCCScheduler_Update(self, 0.0f);
        }
    } else {
        oCCScheduler_Update(self, dt);
        if (GetIsCounting()) {
            GetframeCount()++;
        }
    }
}

// Hook for Level Reset / Death Retry
void THISCALL hkPlayLayer_ResetLevel(void* self) {
    // Reset counter value to 0, but keep counting if it was active
    ResetFrameCount();
    oPlayLayer_ResetLevel(self);
}

bool InstallHook(void* target, void* detour, BYTE* trampoline, uintptr_t& originalFuncPtr) {
    DWORD oldProtect;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;

    uintptr_t targetAddr = (uintptr_t)target;
    uintptr_t detourAddr = (uintptr_t)detour;
    DWORD relativeOffset = (DWORD)(detourAddr - targetAddr - 5);

    memcpy(trampoline, target, 5);
    trampoline[5] = 0xE9; 
    
    uintptr_t trampTarget = targetAddr + 5;
    uintptr_t trampCurrent = (uintptr_t)(trampoline + 6);
    *(DWORD*)(trampoline + 6) = (DWORD)(trampTarget - trampCurrent - 4);

    DWORD trampProtect;
    VirtualProtect(trampoline, 16, PAGE_EXECUTE_READWRITE, &trampProtect);
    originalFuncPtr = (uintptr_t)trampoline;

    *(BYTE*)targetAddr = 0xE9;
    *(DWORD*)(targetAddr + 1) = relativeOffset;

    VirtualProtect(target, 5, oldProtect, &oldProtect);
    return true;
}

DWORD WINAPI MainThread(LPVOID lpParam) {
    HMODULE hCocos = NULL;
    while (!(hCocos = GetModuleHandleA("libcocos2d.dll"))) {
        Sleep(50);
    }

    HMODULE hGD = GetModuleHandleA(NULL); // GeometryDash.exe base

    uintptr_t schedulerAddr = (uintptr_t)hCocos + 0x103220;
    uintptr_t resetLevelAddr = (uintptr_t)hGD + 0x33A000;

    bool schedulerHooked = InstallHook((void*)schedulerAddr, (void*)&hkCCScheduler_Update, g_trampolineUpdate, (uintptr_t&)oCCScheduler_Update);
    bool resetHooked = InstallHook((void*)resetLevelAddr, (void*)&hkPlayLayer_ResetLevel, g_trampolineResetLevel, (uintptr_t&)oPlayLayer_ResetLevel);

    if (schedulerHooked && resetHooked) {
        InitUIThread();
        MessageBoxA(NULL, "TAS Active! F1=Toggle Freeze, F2=Step.", "GD TAS", MB_OK);
    } else {
        MessageBoxA(NULL, "Hooking failed! Check offsets.", "GD TAS", MB_OK);
    }

    while (true) {
        if (GetAsyncKeyState(VK_END) & 1) break;
        
        // F1: Toggle Freeze / Continue for the game engine
        if (GetAsyncKeyState(VK_F1) & 1) {
            GetIsPaused() = !GetIsPaused();
        }
        
        // F2: Step 1 Frame
        if (GetAsyncKeyState(VK_F2) & 1) {
            GetIsPaused() = true;
            GetStepOnce() = true;
        }

        Sleep(10);
    }

    ShutdownUI();
    MessageBoxA(NULL, "TAS Unloaded.", "GD TAS", MB_OK);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)MainThread, NULL, 0, NULL);
    }
    return TRUE;
}