#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>

// NOTE: build this as the SAME architecture as Geometry Dash (32-bit / x86),
// otherwise the LoadLibraryA address resolved below is wrong for the target.

DWORD GetProcessIdByName(const wchar_t* processName) {
    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W processEntry;
        processEntry.dwSize = sizeof(processEntry);
        if (Process32FirstW(snapshot, &processEntry)) {
            do {
                if (_wcsicmp(processEntry.szExeFile, processName) == 0) {
                    pid = processEntry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &processEntry));
        }
        CloseHandle(snapshot);
    }
    return pid;
}

int main() {
    const wchar_t* targetProcess = L"GeometryDash.exe";

    // FIX: resolve payload.dll next to the injector .exe, not relative to the
    // current working directory (which breaks when launched from another folder).
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char dllPath[MAX_PATH];
    snprintf(dllPath, MAX_PATH, "%spayload.dll", exePath);

    std::cout << "[*] Target DLL Path: " << dllPath << "\n";

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        std::cout << "[-] payload.dll not found next to the injector!\n";
        system("pause");
        return 1;
    }

    std::cout << "[*] Searching for GeometryDash.exe...\n";
    
    DWORD pid = GetProcessIdByName(targetProcess);
    if (!pid) {
        std::cout << "[-] Process not found! Make sure Geometry Dash is running.\n";
        system("pause");
        return 1;
    }
    std::cout << "[+] Found GeometryDash.exe (PID: " << pid << ")\n";

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) {
        std::cout << "[-] OpenProcess failed! Error Code: " << GetLastError() << "\n";
        system("pause");
        return 1;
    }

    LPVOID pRemoteMem = VirtualAllocEx(hProcess, NULL, strlen(dllPath) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemoteMem) {
        std::cout << "[-] VirtualAllocEx failed! Error Code: " << GetLastError() << "\n";
        CloseHandle(hProcess);
        system("pause");
        return 1;
    }

    if (!WriteProcessMemory(hProcess, pRemoteMem, dllPath, strlen(dllPath) + 1, NULL)) {
        std::cout << "[-] WriteProcessMemory failed! Error Code: " << GetLastError() << "\n";
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        system("pause");
        return 1;
    }

    LPVOID pLoadLibrary = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLibrary, pRemoteMem, 0, NULL);
    if (!hThread) {
        std::cout << "[-] CreateRemoteThread failed! Error Code: " << GetLastError() << "\n";
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        system("pause");
        return 1;
    }

    std::cout << "[+] Remote thread created! Waiting for LoadLibrary to finish...\n";
    WaitForSingleObject(hThread, INFINITE);

    // Check what HMODULE LoadLibrary returned inside the target process
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    std::cout << "[*] LoadLibraryA returned module handle (HMODULE): " << (void*)(uintptr_t)exitCode << "\n";

    if (exitCode == 0) {
        std::cout << "[-] CRITICAL: LoadLibraryA returned NULL! The game rejected the DLL (usually an architecture mismatch or missing dependency).\n";
    } else {
        std::cout << "[+] SUCCESS: DLL loaded into game memory!\n";
    }

    // Clean up
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    
    system("pause");
    return 0;
}
