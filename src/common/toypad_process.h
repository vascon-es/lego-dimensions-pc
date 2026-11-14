#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace toypad_proc {

inline bool IsToypadExeName(const wchar_t* name) {
    if (!name) return false;
    if (_wcsnicmp(name, L"LegoToypad", 10) == 0) return true;
    if (wcsstr(name, L"Toypad") || wcsstr(name, L"toypad") || wcsstr(name, L"ToyPad") || wcsstr(name, L"Harrysof")) return true;
    return false;
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

// Fecha todas as instancias do LegoToypad (e filhos diretos, ex.: WebView) enviando WM_CLOSE e encerrando se ficar na bandeja.
inline int CloseAll() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    std::vector<DWORD> roots;
    std::vector<std::pair<DWORD, DWORD>> procs;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snap, &entry)) {
        do {
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
            if (pid && std::find(targets.begin(), targets.end(), ppid) != targets.end() &&
                std::find(targets.begin(), targets.end(), pid) == targets.end()) {
                targets.push_back(pid);
                added = true;
            }
        }
    }

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            wchar_t title[256]{};
            if (GetWindowTextW(hwnd, title, 256) > 0) {
                if (wcsstr(title, L"Toypad") || wcsstr(title, L"toypad") ||
                    wcsstr(title, L"Toy Pad") || wcsstr(title, L"Harrysof") ||
                    wcsstr(title, L"LegoToypad")) {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(hwnd, &pid);
                    if (pid) {
                        auto* pids = reinterpret_cast<std::vector<DWORD>*>(lp);
                        if (std::find(pids->begin(), pids->end(), pid) == pids->end()) {
                            pids->push_back(pid);
                        }
                    }
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&targets));

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
    for (DWORD pid : roots) {
        HANDLE hp = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!hp) continue;
        if (WaitForSingleObject(hp, 150) == WAIT_TIMEOUT) {
            if (TerminateProcess(hp, 0)) {
                ++closed;
            }
        } else {
            ++closed;
        }
        CloseHandle(hp);
    }
    for (DWORD pid : targets) {
        if (std::find(roots.begin(), roots.end(), pid) != roots.end()) continue;
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
