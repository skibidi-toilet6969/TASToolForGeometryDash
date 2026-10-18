#pragma once
#include <string>

// ---------------- UI lifecycle ----------------
void InitUIThread();
void ShutdownUI();

// ---------------- Shared state (all thread-safe) ----------------
// When inactive, the hooks become pure pass-through (used on END / "unload").
bool IsActive();
void SetActive(bool active);

// Freeze / step
bool IsPaused();
void SetPaused(bool paused);
void RequestStep();           // ask the scheduler hook to run exactly one frame
bool ConsumeStepRequest();    // true once per RequestStep()

// Frame counter
bool IsCounting();            // Start / Stop buttons
int  GetFrameCount();
void CountFrame();            // +1 frame, ONLY if the counter is running
void ResetFrameCount();       // sets to 0 and bumps the reset generation
unsigned GetResetGeneration();// changes every time the frame counter is reset

// Death state: set by the destroyPlayer hook, cleared by the resetLevel hook.
// While dead the frame counter is frozen and inputs are not logged.
bool IsDead();
void MarkDead();              // remembers the frame the player died on
void ClearDead();
int  GetDeathFrame();

// Input recording
bool IsRecording();
int  GetInputMode();          // 0 = Mouse, 1 = Spacebar
void SetLastLoggedInput(const std::string& text);

// True if the control-panel window is focused or the cursor is over it
// (so clicks on the panel are not mistaken for in-game clicks).
bool IsUIWindowInteracted();

// Optional extension point: lets other code (e.g. payload_debug.cpp) draw extra ImGui
// widgets inside the control panel and choose the panel's client size.
// Call BEFORE InitUIThread().
void SetExtraUI(void (*drawFn)(), int clientW, int clientH);
