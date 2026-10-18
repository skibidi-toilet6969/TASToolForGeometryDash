#include "ui.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"
#include <windows.h>
#include <d3d9.h>
#include <thread>
#include <atomic>
#include <string>
#include <mutex>

// ---------------- Shared state ----------------
static std::atomic<bool>     g_active(true);
static std::atomic<bool>     g_isPaused(false);
static std::atomic<bool>     g_stepOnce(false);
static std::atomic<bool>     g_isCounting(true);
static std::atomic<bool>     g_isRecording(false);
static std::atomic<int>      g_frameCount(0);
static std::atomic<int>      g_inputMode(0);          // 0 = Mouse, 1 = Spacebar
static std::atomic<unsigned> g_resetGeneration(0);
static std::atomic<bool>     g_isDead(false);
static std::atomic<int>      g_deathFrame(-1);
static std::atomic<bool>     g_running(true);

// Thread-safe storage for the last logged input display text
static std::string g_lastLoggedText = "None";
static std::mutex  g_textMutex;

// Extension point (see ui.h)
static void (*g_extraUI)() = nullptr;
static int g_clientW = 300;
static int g_clientH = 400;

void SetExtraUI(void (*drawFn)(), int clientW, int clientH) {
    g_extraUI = drawFn;
    g_clientW = clientW;
    g_clientH = clientH;
}

static HWND g_uiHwnd = NULL;
static IDirect3D9* g_pD3D = NULL;
static IDirect3DDevice9* g_pd3dDevice = NULL;

bool IsActive()                { return g_active.load(); }
void SetActive(bool active)    { g_active.store(active); }

bool IsPaused()                { return g_isPaused.load(); }
void SetPaused(bool paused)    { g_isPaused.store(paused); }
void RequestStep()             { g_stepOnce.store(true); }
bool ConsumeStepRequest()      { return g_stepOnce.exchange(false); }

bool IsCounting()              { return g_isCounting.load(); }
bool IsRecording()             { return g_isRecording.load(); }
int  GetInputMode()            { return g_inputMode.load(); }
int  GetFrameCount()           { return g_frameCount.load(); }

// FIX: the Start/Stop buttons now actually matter - frames are only counted
// while the counter is running.
// Also frozen while the player is dead, so "Frames Ran" shows the frame you died on
// instead of running on through the death animation until the respawn.
void CountFrame() {
    if (g_isCounting.load() && !g_isDead.load())
        g_frameCount.fetch_add(1);
}

void ResetFrameCount() {
    g_frameCount.store(0);
    g_resetGeneration.fetch_add(1); // lets the recorder know a new attempt began
}

unsigned GetResetGeneration()  { return g_resetGeneration.load(); }

bool IsDead()                  { return g_isDead.load(); }
int  GetDeathFrame()           { return g_deathFrame.load(); }
void ClearDead()               { g_isDead.store(false); }
void MarkDead() {
    g_deathFrame.store(g_frameCount.load());
    g_isDead.store(true);
}

void SetLastLoggedInput(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_textMutex);
    g_lastLoggedText = text;
}

static std::string GetLastLoggedInput() {
    std::lock_guard<std::mutex> lock(g_textMutex);
    return g_lastLoggedText;
}

bool IsUIWindowInteracted() {
    if (!g_uiHwnd) return false;
    if (GetForegroundWindow() == g_uiHwnd) return true;

    // Mouse-down reaches GetAsyncKeyState slightly before the window becomes
    // foreground, so also check what is under the cursor.
    POINT pt;
    if (GetCursorPos(&pt)) {
        HWND under = WindowFromPoint(pt);
        if (under && GetAncestor(under, GA_ROOT) == g_uiHwnd) return true;
    }
    return false;
}

LRESULT WINAPI UIVecProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void RenderLoop() {
    const int kClientW = g_clientW;
    const int kClientH = g_clientH;
    // Fixed-size window (resizing would need a full D3D reset path)
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, UIVecProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "GA_TASTool", NULL };
    RegisterClassExA(&wc);

    // FIX: size the *client area* exactly, so the panel is never cut off
    RECT rc = { 0, 0, kClientW, kClientH };
    AdjustWindowRect(&rc, style, FALSE);

    g_uiHwnd = CreateWindowExA(0, "GA_TASTool", "Geometry Dash TAS - Control Panel", style,
                               100, 100, rc.right - rc.left, rc.bottom - rc.top,
                               NULL, NULL, wc.hInstance, NULL);

    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D) return;

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_uiHwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &d3dpp, &g_pd3dDevice) < 0)
        return;

    ShowWindow(g_uiHwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_uiHwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(g_uiHwnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (g_running.load() && msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // Don't spin while minimized
        if (IsIconic(g_uiHwnd)) {
            Sleep(20);
            continue;
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        // FIX: no ImGui close button - it used to kill the whole panel with no way to reopen it
        ImGui::Begin("TAS Controller", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        const bool paused    = g_isPaused.load();
        const bool counting  = g_isCounting.load();
        const bool recording = g_isRecording.load();

        ImGui::Text("Status: %s", paused ? "FROZEN" : "RUNNING");
        ImGui::Text("Frames Ran: %d", g_frameCount.load());
        ImGui::Text("Counter: %s", counting ? "ON" : "OFF");
        if (g_isDead.load())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "DEAD - died on frame %d", g_deathFrame.load());
        else
            ImGui::Text("Player: alive");
        ImGui::Separator();

        // Counter control buttons
        if (ImGui::Button("Start", ImVec2(80, 30))) g_isCounting.store(true);
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(80, 30)))  g_isCounting.store(false);
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(80, 30))) ResetFrameCount();

        ImGui::Separator();
        // FIX: the panel could freeze the game (Step) but had no way to resume it
        if (ImGui::Button(paused ? "Continue (F1)" : "Freeze (F1)", ImVec2(-1, 25))) {
            g_isPaused.store(!paused);
        }
        if (ImGui::Button("Step 1 Frame (F2)", ImVec2(-1, 25))) {
            g_isPaused.store(true);
            g_stepOnce.store(true);
        }

        ImGui::Separator();
        // Input mode switch (locked while recording so the log stays consistent)
        const int currentMode = g_inputMode.load();
        const char* modeLabel = (currentMode == 0) ? "Input Mode: Mouse Click" : "Input Mode: Spacebar";
        if (ImGui::Button(modeLabel, ImVec2(-1, 30))) {
            if (!recording) g_inputMode.store(1 - currentMode);
        }
        if (recording) ImGui::TextDisabled("(mode locked while recording)");

        ImGui::Separator();
        if (ImGui::Button(recording ? "Stop Recording Input" : "Start Recording Input", ImVec2(-1, 30))) {
            g_isRecording.store(!recording);
        }
        if (recording && !counting) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Counter is OFF - inputs not logged");
        }

        ImGui::Text("Last Logged: %s", GetLastLoggedInput().c_str());

        if (g_extraUI) g_extraUI();

        ImGui::End();

        ImGui::Render();
        g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_pd3dDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_RGBA(30, 30, 30, 255), 1.0f, 0);

        if (g_pd3dDevice->BeginScene() >= 0) {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_pd3dDevice->EndScene();
        }

        HRESULT result = g_pd3dDevice->Present(NULL, NULL, NULL, NULL);
        if (result == D3DERR_DEVICELOST && g_pd3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) {
            // FIX: ImGui's device objects must be released before Reset() and rebuilt after
            ImGui_ImplDX9_InvalidateDeviceObjects();
            if (g_pd3dDevice->Reset(&d3dpp) == D3D_OK)
                ImGui_ImplDX9_CreateDeviceObjects();
        }
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_pd3dDevice) g_pd3dDevice->Release();
    if (g_pD3D) g_pD3D->Release();
    DestroyWindow(g_uiHwnd);
    UnregisterClassA("GA_TASTool", GetModuleHandle(NULL));
}

void InitUIThread() {
    std::thread(RenderLoop).detach();
}

void ShutdownUI() {
    g_running = false;
}
