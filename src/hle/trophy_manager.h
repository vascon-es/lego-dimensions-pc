#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <filesystem>
#include "trophy_defs.h"

namespace hle::trophy {

struct ToastState {
    int trophy_id = -1;
    char type = 'B';
    std::string title;
    std::string desc;
    std::string popup_style = "ps5"; // ps5, ps4, xbox, xbox360, steam
    float time_remaining = 0.0f;
    float total_duration = 5.5f;
    bool active = false;
};

class TrophyManager {
public:
    static TrophyManager& Instance();

    void Init(const std::filesystem::path& root_path, const std::filesystem::path& saves_dir);

    // Called from sceNpTrophyUnlockTrophy
    bool UnlockTrophy(int trophy_id, int* platinum_id, bool notify = true);

    // Called from sceNpTrophyGetTrophyUnlockState
    void GetUnlockState(uint32_t* flags, uint32_t* count);

    bool IsUnlocked(int trophy_id) const;
    uint64_t GetTimestamp(int trophy_id) const;
    void GetProgress(int* unlocked, int* total, int* plat, int* gold, int* silver, int* bronze) const;

    void SetSoundType(const std::string& sound_name);
    std::string GetSoundType() const;
    void CycleSoundType();

    void SetPopupStyle(const std::string& style);
    std::string GetPopupStyle() const;
    void CyclePopupStyle();

    void ResetTrophies();

    void CheckRetroactiveSaves();

    // Toast notifications
    void QueueToast(int trophy_id);
    void Update(float delta_sec);
    bool HasActiveToast() const;
    ToastState GetActiveToast() const;

    const TrophyDef* FindDef(int trophy_id) const;

private:
    TrophyManager();
    ~TrophyManager() = default;

    void LoadTrophyXml();
    void SaveTrophyXml();
    void PlayTrophySound();

    mutable std::recursive_mutex m_mutex;
    std::filesystem::path m_root_path;
    std::filesystem::path m_saves_dir;
    std::filesystem::path m_xml_path;

    uint32_t m_flags[4] = {}; // 128-bit bitmask
    uint64_t m_timestamps[128] = {};
    std::string m_sound_type = "playstation"; // playstation, xbox, steam, off, custom
    std::string m_popup_style = "ps5";        // ps5, ps4, xbox, xbox360, steam

    std::deque<int> m_toast_queue;
    ToastState m_active_toast;
    float m_toast_cooldown = 0.0f;
    bool m_initialized = false;
    bool m_retroactive_done = false;
};

} // namespace hle::trophy
