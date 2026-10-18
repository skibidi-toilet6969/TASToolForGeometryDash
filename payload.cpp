#include <windows.h>
#include <fstream>
#include <string>
#include <cstring>
#include "ui.h"

#define THISCALL __attribute__((thiscall))

// --- Scheduler Hook (Physics & Frames) ---
typedef void(THISCALL *CCScheduler_Update_t)(void* self, float dt);
CCScheduler_Update_t oCCScheduler_Update = nullptr;
BYTE g_trampolineUpdate[16];

// --- PlayLayer::resetLevel Hook (Death / Level Restart) ---
typedef void(THISCALL *PlayLayer_ResetLevel_t)(void* self);
PlayLayer_ResetLevel_t oPlayLayer_ResetLevel = nullptr;
BYTE g_trampolineResetLevel[16];

// --- PlayLayer::destroyPlayer Hook (the actual moment of death) ---
// Assumes the GD 2.1 signature: void PlayLayer::destroyPlayer(PlayerObject*)
typedef void(THISCALL *PlayLayer_DestroyPlayer_t)(void* self, void* player);
PlayLayer_DestroyPlayer_t oPlayLayer_DestroyPlayer = nullptr;
BYTE g_trampolineDestroyPlayer[16];

// bool PlayLayer.isDead = PlayLayer + 0x39C
static const uintptr_t kPlayLayerIsDeadOffset = 0x39C;

HMODULE g_hModuleSelf = NULL;

// One "frame" = one 240 Hz physics tick. Same value for stepping and for normal running.
static const float kStepDt = 1.0f / 240.0f;
// Safety cap so a long hitch (loading, window drag) doesn't try to simulate seconds at once.
static const int kMaxStepsPerUpdate = 60;

static std::string g_tasPath;
static float g_accumulator = 0.0f; // game thread only

std::string GetTasFilePath(HMODULE hModule) {
    char path[MAX_PATH];
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string::size_type pos = std::string(path).find_last_of("\\/");
    return std::string(path).substr(0, pos) + "\\tas.txt";
}

// ------------------------------------------------------------------
// Focus helpers - hotkeys / clicks should only count when the game
// (or our control panel) is the active window, not when you're typing
// in a browser.
// ------------------------------------------------------------------
static bool IsOurProcessForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Reliable edge detection (the "& 1" bit of GetAsyncKeyState is not reliable).
static bool EdgePressed(int vk, bool focused, bool& prev) {
    const bool down = focused && KeyDown(vk);
    const bool edge = down && !prev;
    prev = down;
    return edge;
}

// ------------------------------------------------------------------
// Input recording. Runs on the GAME thread, right before each scheduler
// update, so the logged frame number is exactly the frame the input
// will be applied on (the old 5 ms polling thread could be off by
// several frames).
//
// Log format (tas.txt):
//   <frame>, click            mouse button pressed
//   <frame>, click_release    mouse button released
//   <frame>, space            space pressed
//   <frame>, space_release    space released
// ------------------------------------------------------------------
static bool g_prevDown = false;
static bool g_loggedDown = false;
static bool g_wasRecording = false;
static unsigned g_lastGeneration = 0;
static std::string g_recName = "click";

static void AppendLog(int frame, const std::string& label) {
    std::ofstream file(g_tasPath, std::ios::app);
    if (file.is_open())
        file << frame << ", " << label << "\n";
}

static void TruncateLog() {
    std::ofstream file(g_tasPath, std::ios::trunc);
}

static void PollInput() {
    const int mode = GetInputMode();
    const int vk = (mode == 0) ? VK_LBUTTON : VK_SPACE;
    const char* name = (mode == 0) ? "click" : "space";

    // Only count input that is really meant for the game
    const bool focused = IsOurProcessForeground() && !IsUIWindowInteracted();
    const bool down = focused && KeyDown(vk);

    const bool recording = IsRecording() && IsCounting();
    const int frame = GetFrameCount();

    // Dead: the frame counter is frozen and nothing that happens now belongs to any attempt.
    // Keep tracking the key so a release during the death animation isn't lost.
    if (IsDead()) {
        g_prevDown = down;
        return;
    }

    // Frame counter was reset (death / level restart / Reset button): new attempt
    const unsigned gen = GetResetGeneration();
    const bool newAttempt = (gen != g_lastGeneration);
    g_lastGeneration = gen;

    if (!recording) {
        // Recording just stopped while the key was held: close the hold
        // (skipped if the frame counter was just reset - that release belongs to the old attempt)
        if (g_wasRecording && g_loggedDown && !newAttempt)
            AppendLog(frame, g_recName + "_release");
        g_wasRecording = false;
        g_loggedDown = false;
        g_prevDown = down; // keep tracking so a held key isn't logged as a new press later
        return;
    }

    if (!g_wasRecording || newAttempt) {
        // Fresh log: starting a recording, or frame numbers restarted from 0.
        // (Previously tas.txt grew forever and mixed frames from every attempt.)
        TruncateLog();
        g_loggedDown = false;
        SetLastLoggedInput("None");
    }
    g_wasRecording = true;
    g_recName = name;

    if (down && !g_prevDown) {
        AppendLog(frame, name);
        g_loggedDown = true;
        SetLastLoggedInput(std::to_string(frame) + ", " + name);
    } else if (!down && g_prevDown && g_loggedDown) {
        const std::string label = std::string(name) + "_release";
        AppendLog(frame, label);
        g_loggedDown = false;
        SetLastLoggedInput(std::to_string(frame) + ", " + label);
    }
    g_prevDown = down;
}

// ------------------------------------------------------------------
// Hooks (logic only - hook installation itself is unchanged)
// ------------------------------------------------------------------
void THISCALL hkCCScheduler_Update(void* self, float dt) {
    if (!IsActive()) {
        oCCScheduler_Update(self, dt);
        return;
    }

    PollInput();

    if (IsPaused()) {
        if (ConsumeStepRequest()) {
            oCCScheduler_Update(self, kStepDt);
            CountFrame();
        } else {
            oCCScheduler_Update(self, 0.0f);
        }
        return;
    }

    // FIX: run on the same fixed 240 Hz tick the Step button uses, so frame numbers
    // are identical whether you step manually or let it run, at any display FPS.
    // (Before, running counted 1 per rendered frame while stepping counted 1 per tick.)
    g_accumulator += dt;
    int steps = 0;
    while (g_accumulator >= kStepDt && steps < kMaxStepsPerUpdate) {
        oCCScheduler_Update(self, kStepDt);
        CountFrame();
        g_accumulator -= kStepDt;
        ++steps;
    }
    if (steps == kMaxStepsPerUpdate)
        g_accumulator = 0.0f; // drop the backlog after a big hitch
}

void THISCALL hkPlayLayer_DestroyPlayer(void* self, void* player) {
    if (!IsActive()) {
        oPlayLayer_DestroyPlayer(self, player);
        return;
    }

    // destroyPlayer can be entered several times for one death (multiple hazards, dual mode),
    // so only react when the game itself flips PlayLayer::isDead from false to true.
    const bool* isDeadPtr = (const bool*)((uintptr_t)self + kPlayLayerIsDeadOffset);
    const bool wasDead = *isDeadPtr;
    oPlayLayer_DestroyPlayer(self, player);
    if (!wasDead && *isDeadPtr && !IsDead())
        MarkDead();
}

void THISCALL hkPlayLayer_ResetLevel(void* self) {
    if (IsActive()) {
        ResetFrameCount();
        ClearDead();
        g_accumulator = 0.0f;
    }
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
    // Must be set before the hooks go live (PollInput uses it on the game thread)
    g_tasPath = GetTasFilePath(g_hModuleSelf);

    HMODULE hCocos = NULL;
    while (!(hCocos = GetModuleHandleA("libcocos2d.dll"))) {
        Sleep(50);
    }

    HMODULE hGD = GetModuleHandleA(NULL); // GeometryDash.exe base

    uintptr_t schedulerAddr = (uintptr_t)hCocos + 0x103220;
    uintptr_t resetLevelAddr = (uintptr_t)hGD + 0x20BF00;      // PlayLayer::resetLevel
    uintptr_t destroyPlayerAddr = (uintptr_t)hGD + 0x20A1A0;   // PlayLayer::destroyPlayer

    bool schedulerHooked = InstallHook((void*)schedulerAddr, (void*)&hkCCScheduler_Update, g_trampolineUpdate, (uintptr_t&)oCCScheduler_Update);
    bool resetHooked = InstallHook((void*)resetLevelAddr, (void*)&hkPlayLayer_ResetLevel, g_trampolineResetLevel, (uintptr_t&)oPlayLayer_ResetLevel);

    bool destroyHooked = InstallHook((void*)destroyPlayerAddr, (void*)&hkPlayLayer_DestroyPlayer, g_trampolineDestroyPlayer, (uintptr_t&)oPlayLayer_DestroyPlayer);

    if (schedulerHooked && resetHooked && destroyHooked) {
        InitUIThread();
        MessageBoxA(NULL, "TAS Active! Input logger ready.", "GD TAS", MB_OK);
    } else {
        MessageBoxA(NULL, "Hooking failed! Check offsets.", "GD TAS", MB_OK);
    }

    // Hotkeys (only react while the game or the control panel is the active window):
    //   F1 = freeze / continue, F2 = step 1 frame, END = disable
    bool prevEnd = false, prevF1 = false, prevF2 = false;
    while (true) {
        const bool focused = IsOurProcessForeground();

        if (EdgePressed(VK_END, focused, prevEnd)) break;

        if (EdgePressed(VK_F1, focused, prevF1)) {
            SetPaused(!IsPaused());
        }

        if (EdgePressed(VK_F2, focused, prevF2)) {
            SetPaused(true);
            RequestStep();
        }

        Sleep(5);
    }

    // FIX: END used to leave the game FROZEN forever if it was paused (the hotkey thread
    // and UI were gone but the hooks were still active). Make the hooks pass-through and
    // unfreeze. The hooks themselves stay installed (hook handling intentionally untouched).
    SetActive(false);
    SetPaused(false);
    ShutdownUI();
    MessageBoxA(NULL, "TAS disabled (game runs normally again).", "GD TAS", MB_OK);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_hModuleSelf = hModule;
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)MainThread, NULL, 0, NULL);
    }
    return TRUE;
}
