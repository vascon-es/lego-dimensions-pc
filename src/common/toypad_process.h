#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")

#include <algorithm>
#include <utility>
#include <vector>

namespace toypad_proc {

inline bool IsToypadExeName(const wchar_t* name) {
    if (!name) return false;
    // Nao confunde com o launcher ou o executavel principal do jogo
    if (_wcsicmp(name, L"LegoDimensionsLauncher.exe") == 0) return false;
    if (_wcsicmp(name, L"lego_dimensions.exe") == 0) return false;
    return (_wcsnicmp(name, L"LegoToypad", 10) == 0);
}

template <typename Pred>
inline bool AnyProcessMatches(Pred&& pred) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snap, &entry)) {
        do {
            if (pred(entry.szExeFile)) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

inline bool IsExeRunning(const wchar_t* exact_name) {
    return exact_name && AnyProcessMatches([exact_name](const wchar_t* name) { return _wcsicmp(name, exact_name) == 0; });
}

inline bool IsRunning() {
    return AnyProcessMatches([](const wchar_t* name) { return IsToypadExeName(name); });
}

// Fecha todas as instancias do LegoToypad (e processos filhos diretos, ex.: WebView/Electron)
// NUNCA encerra o Launcher nem o processo atual.
inline int CloseAll() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    const DWORD current_pid = GetCurrentProcessId();
    std::vector<DWORD> roots;
    std::vector<std::pair<DWORD, DWORD>> procs;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == current_pid || entry.th32ProcessID <= 4) continue;
            if (_wcsicmp(entry.szExeFile, L"LegoDimensionsLauncher.exe") == 0) continue;
            if (_wcsicmp(entry.szExeFile, L"lego_dimensions.exe") == 0) continue;

            if (IsToypadExeName(entry.szExeFile)) {
                roots.push_back(entry.th32ProcessID);
            }
            procs.emplace_back(entry.th32ProcessID, entry.th32ParentProcessID);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    if (roots.empty()) return 0;

    std::vector<DWORD> targets = roots;
    bool added = true;
    while (added) {
        added = false;
        for (const auto& [pid, ppid] : procs) {
            if (pid && pid != current_pid &&
                std::find(targets.begin(), targets.end(), ppid) != targets.end() &&
                std::find(targets.begin(), targets.end(), pid) == targets.end()) {
                targets.push_back(pid);
                added = true;
            }
        }
    }

    // Envia WM_CLOSE para janelas que pertensam aos processos alvos do Toypad
    struct EnumCtx {
        const std::vector<DWORD>* pids;
    } ctx{&targets};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            const auto* c = reinterpret_cast<const EnumCtx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid && std::find(c->pids->begin(), c->pids->end(), pid) != c->pids->end()) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    int closed = 0;
    // Aguarda encerramento suave e finaliza com seguranca se permanecer na bandeja
    for (DWORD pid : roots) {
        if (pid == current_pid) continue;
        HANDLE hp = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!hp) continue;
        if (WaitForSingleObject(hp, 200) == WAIT_TIMEOUT) {
            if (TerminateProcess(hp, 0)) {
                ++closed;
            }
        } else {
            ++closed;
        }
        CloseHandle(hp);
    }
    for (DWORD pid : targets) {
        if (pid == current_pid || std::find(roots.begin(), roots.end(), pid) != roots.end()) continue;
        HANDLE hp = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!hp) continue;
        if (TerminateProcess(hp, 0)) {
            ++closed;
        }
        CloseHandle(hp);
    }
    return closed;
}

} // namespace toypad_proc
