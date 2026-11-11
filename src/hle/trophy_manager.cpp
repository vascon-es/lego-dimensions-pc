#include "trophy_manager.h"
#include "trophy_sounds.h"
#include "runtime/lang.h"
#include "hle_log.h"

#include <windows.h>
#include <mmsystem.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace hle::trophy {

TrophyManager& TrophyManager::Instance() {
    static TrophyManager s_instance;
    return s_instance;
}

TrophyManager::TrophyManager() = default;

void TrophyManager::Init(const std::filesystem::path& root_path, const std::filesystem::path& saves_dir) {
    {
        std::lock_guard lock(m_mutex);
        m_root_path = root_path;
        m_saves_dir = saves_dir;

        std::error_code ec;
        // Prefer assets/trophy/NPWR07423_00.xml
        auto dir = m_root_path / "assets" / "trophy";
        std::filesystem::create_directories(dir, ec);
        m_xml_path = dir / "NPWR07423_00.xml";

        m_initialized = true;
        LoadTrophyXml();
    }
    CheckRetroactiveSaves();
}

const TrophyDef* TrophyManager::FindDef(int trophy_id) const {
    size_t count = 0;
    const TrophyDef* defs = GetTrophyDefs(&count);
    for (size_t i = 0; i < count; ++i) {
        if (defs[i].id == trophy_id) {
            return &defs[i];
        }
    }
    return nullptr;
}

bool TrophyManager::IsUnlocked(int trophy_id) const {
    if (trophy_id < 0 || trophy_id >= 128) return false;
    const int word = trophy_id / 32;
    const int bit = trophy_id % 32;
    return (m_flags[word] & (1u << bit)) != 0;
}

uint64_t TrophyManager::GetTimestamp(int trophy_id) const {
    std::lock_guard lock(m_mutex);
    if (trophy_id < 0 || trophy_id >= 128) return 0;
    return m_timestamps[trophy_id];
}

void TrophyManager::GetProgress(int* unlocked, int* total, int* plat, int* gold, int* silver, int* bronze) const {
    std::lock_guard lock(m_mutex);
    size_t count = 0;
    const TrophyDef* defs = GetTrophyDefs(&count);
    int u = 0, p = 0, g = 0, s = 0, b = 0;
    for (size_t i = 0; i < count; ++i) {
        if (IsUnlocked(defs[i].id)) {
            ++u;
            if (defs[i].type == 'P') ++p;
            else if (defs[i].type == 'G') ++g;
            else if (defs[i].type == 'S') ++s;
            else if (defs[i].type == 'B') ++b;
        }
    }
    if (unlocked) *unlocked = u;
    if (total) *total = static_cast<int>(count);
    if (plat) *plat = p;
    if (gold) *gold = g;
    if (silver) *silver = s;
    if (bronze) *bronze = b;
}

void TrophyManager::GetUnlockState(uint32_t* flags, uint32_t* count) {
    std::lock_guard lock(m_mutex);
    if (!flags || !count) return;
    std::memcpy(flags, m_flags, sizeof(m_flags));

    uint32_t total = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t w = m_flags[i];
        while (w) {
            total += (w & 1);
            w >>= 1;
        }
    }
    *count = total;
}

bool TrophyManager::UnlockTrophy(int trophy_id, int* platinum_id, bool notify) {
    std::lock_guard lock(m_mutex);
    if (trophy_id < 0 || trophy_id >= 128) return false;

    if (platinum_id) {
        *platinum_id = -1;
    }

    const int word = trophy_id / 32;
    const int bit = trophy_id % 32;
    if (m_flags[word] & (1u << bit)) {
        // Already unlocked
        return false;
    }

    m_flags[word] |= (1u << bit);
    const auto now = std::chrono::system_clock::now();
    m_timestamps[trophy_id] = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    const TrophyDef* def = FindDef(trophy_id);
    log::Tagged("TROPHY", "Unlocked [%c] ID=%d '%s'", def ? def->type : '?', trophy_id, def ? def->name_en : "");

    // Check platinum (if base game trophies 1..38 are all unlocked, unlock platinum ID 0)
    bool all_base = true;
    for (int i = 1; i <= 38; ++i) {
        const int w = i / 32;
        const int b = i % 32;
        if (!(m_flags[w] & (1u << b))) {
            all_base = false;
            break;
        }
    }

    if (notify) {
        QueueToast(trophy_id);
        PlayTrophySound();
    }

    if (all_base && !(m_flags[0] & 1u)) {
        m_flags[0] |= 1u;
        m_timestamps[0] = m_timestamps[trophy_id];
        if (platinum_id) {
            *platinum_id = 0;
        }
        if (notify) {
            QueueToast(0);
        }
        log::Tagged("TROPHY", "PLATINUM UNLOCKED!");
    }

    if (notify) {
        SaveTrophyXml();
    }
    return true;
}

void TrophyManager::QueueToast(int trophy_id) {
    m_toast_queue.push_back(trophy_id);
}

void TrophyManager::PlayTrophySound() {
    if (m_sound_type == "off" || m_sound_type == "desligado" || m_sound_type == "none") {
        return;
    }

    // Check custom sound file first
    auto custom_wav = m_root_path / "assets" / "custom_trophy" / "trophy.wav";
    std::error_code ec;
    if (std::filesystem::exists(custom_wav, ec)) {
        PlaySoundW(custom_wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        return;
    }

    if (m_sound_type == "xbox") {
        PlaySoundW(reinterpret_cast<LPCWSTR>(kTrophySoundXbox), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    } else if (m_sound_type == "steam") {
        PlaySoundW(reinterpret_cast<LPCWSTR>(kTrophySoundSteam), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    } else {
        // Default: PlayStation
        PlaySoundW(reinterpret_cast<LPCWSTR>(kTrophySoundPlaystation), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
}

void TrophyManager::SetSoundType(const std::string& sound_name) {
    std::lock_guard lock(m_mutex);
    m_sound_type = sound_name;
}

std::string TrophyManager::GetSoundType() const {
    std::lock_guard lock(m_mutex);
    return m_sound_type;
}

void TrophyManager::CycleSoundType() {
    std::lock_guard lock(m_mutex);
    if (m_sound_type == "playstation") m_sound_type = "xbox";
    else if (m_sound_type == "xbox") m_sound_type = "steam";
    else if (m_sound_type == "steam") m_sound_type = "off";
    else m_sound_type = "playstation";
}

void TrophyManager::SetPopupStyle(const std::string& style) {
    std::lock_guard lock(m_mutex);
    m_popup_style = style;
}

std::string TrophyManager::GetPopupStyle() const {
    std::lock_guard lock(m_mutex);
    return m_popup_style;
}

void TrophyManager::CyclePopupStyle() {
    std::lock_guard lock(m_mutex);
    if (m_popup_style == "ps5") m_popup_style = "ps4";
    else if (m_popup_style == "ps4") m_popup_style = "xbox";
    else if (m_popup_style == "xbox") m_popup_style = "xbox360";
    else if (m_popup_style == "xbox360") m_popup_style = "steam";
    else m_popup_style = "ps5";
}

void TrophyManager::Update(float delta_sec) {
    std::lock_guard lock(m_mutex);

    if (m_active_toast.active) {
        m_active_toast.time_remaining -= delta_sec;
        if (m_active_toast.time_remaining <= 0.0f) {
            m_active_toast.active = false;
            m_toast_cooldown = 0.25f; // Small pause between consecutive toasts
        }
    } else if (m_toast_cooldown > 0.0f) {
        m_toast_cooldown -= delta_sec;
    } else if (!m_toast_queue.empty()) {
        const int next_id = m_toast_queue.front();
        m_toast_queue.pop_front();

        const TrophyDef* def = FindDef(next_id);
        if (def) {
            m_active_toast.trophy_id = next_id;
            m_active_toast.type = def->type;
            m_active_toast.title = lang::Pt() ? def->name_pt : def->name_en;
            m_active_toast.desc = lang::Pt() ? def->desc_pt : def->desc_en;
            m_active_toast.popup_style = m_popup_style;
            m_active_toast.total_duration = 5.5f;
            m_active_toast.time_remaining = 5.5f;
            m_active_toast.active = true;
        }
    }
}

bool TrophyManager::HasActiveToast() const {
    std::lock_guard lock(m_mutex);
    return m_active_toast.active;
}

ToastState TrophyManager::GetActiveToast() const {
    std::lock_guard lock(m_mutex);
    return m_active_toast;
}

void TrophyManager::ResetTrophies() {
    std::lock_guard lock(m_mutex);
    std::memset(m_flags, 0, sizeof(m_flags));
    std::memset(m_timestamps, 0, sizeof(m_timestamps));
    m_toast_queue.clear();
    m_active_toast.active = false;

    // Ensure marker exists so retroactive scan will not re-grant reset trophies
    std::error_code ec;
    auto marker = m_root_path / "assets" / "trophy" / ".retro_done";
    std::ofstream m(marker, std::ios::trunc);
    if (m) m << "reset\n";

    SaveTrophyXml();
    log::Tagged("TROPHY", "All trophies reset to locked state.");
}

void TrophyManager::LoadTrophyXml() {
    std::error_code ec;
    if (std::filesystem::exists(m_xml_path, ec)) {
        std::ifstream file(m_xml_path);
        if (file) {
            std::string line;
            while (std::getline(file, line)) {
                auto pos = line.find("<trophy id=\"");
                if (pos == std::string::npos) continue;

                pos += 12;
                int id = std::atoi(line.c_str() + pos);
                if (id < 0 || id >= 128) continue;

                if (line.find("unlockstate=\"true\"") != std::string::npos) {
                    m_flags[id / 32] |= (1u << (id % 32));
                    auto ts_pos = line.find("timestamp=\"");
                    if (ts_pos != std::string::npos) {
                        m_timestamps[id] = std::strtoull(line.c_str() + ts_pos + 11, nullptr, 10);
                    }
                }
            }
            log::Tagged("TROPHY", "Loaded trophy XML: %s", m_xml_path.string().c_str());
        }
    }

    bool has_any_flag = false;
    for (int i = 0; i < 4; ++i) {
        if (m_flags[i]) has_any_flag = true;
    }
    if (!has_any_flag) {
        const std::filesystem::path dat_paths[] = {
            m_root_path / "assets" / "trophy" / "trophies.dat",
            m_saves_dir / "trophies.dat",
            m_root_path / "assets" / "savedata0" / "trophies.dat"
        };
        for (const auto& p : dat_paths) {
            std::ifstream dat(p, std::ios::binary);
            if (!dat) continue;
            char magic[4] = {};
            dat.read(magic, 4);
            if (std::memcmp(magic, "LDT1", 4) == 0) {
                uint32_t c = 0;
                dat.read(reinterpret_cast<char*>(&c), 4);
                dat.read(reinterpret_cast<char*>(m_flags), sizeof(m_flags));
                dat.read(reinterpret_cast<char*>(m_timestamps), sizeof(m_timestamps));
                log::Tagged("TROPHY", "Loaded trophies.dat from %s (%u unlocked)", p.string().c_str(), c);
                break;
            }
        }
    }
}

void TrophyManager::SaveTrophyXml() {
    std::ofstream out(m_xml_path, std::ios::trunc);
    if (!out) return;

    out << "<?xml version=\"1.0\"?>\n";
    out << "<trophyconf version=\"1.1\" platform=\"ps4\" policy=\"large\">\n";
    out << "\t<npcommid>NPWR07423_00</npcommid>\n";
    out << "\t<trophyset-version>01.09</trophyset-version>\n";
    out << "\t<parental-level license-area=\"default\">0</parental-level>\n";
    for (int g = 1; g <= 13; ++g) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%03d", g);
        out << "\t<group id=\"" << buf << "\" />\n";
    }

    size_t count = 0;
    const TrophyDef* defs = GetTrophyDefs(&count);
    for (size_t i = 0; i < count; ++i) {
        const auto& d = defs[i];
        char id_str[16];
        std::snprintf(id_str, sizeof(id_str), "%03d", d.id);
        const bool unlocked = IsUnlocked(d.id);
        const char* pid = d.id == 0 ? "-1" : "000";

        out << "\t<trophy id=\"" << id_str << "\" hidden=\"no\" ttype=\"" << d.type << "\" pid=\"" << pid << "\"";
        if (unlocked) {
            out << " unlockstate=\"true\" timestamp=\"" << m_timestamps[d.id] << "\"";
        }
        out << " />\n";
    }
    out << "</trophyconf>\n";

    // Write binary trophies.dat
    uint32_t total_unlocked = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t w = m_flags[i];
        while (w) {
            total_unlocked += (w & 1);
            w >>= 1;
        }
    }

    const std::filesystem::path dat_paths[] = {
        m_root_path / "assets" / "trophy" / "trophies.dat",
        m_saves_dir / "trophies.dat",
        m_root_path / "assets" / "savedata0" / "trophies.dat"
    };

    for (const auto& p : dat_paths) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream dat(p, std::ios::binary | std::ios::trunc);
        if (dat) {
            dat.write("LDT1", 4);
            dat.write(reinterpret_cast<const char*>(&total_unlocked), 4);
            dat.write(reinterpret_cast<const char*>(m_flags), sizeof(m_flags));
            dat.write(reinterpret_cast<const char*>(m_timestamps), sizeof(m_timestamps));
        }
    }
}

void TrophyManager::CheckRetroactiveSaves() {
    std::error_code ec;
    auto marker = m_root_path / "assets" / "trophy" / ".retro_done";
    if (std::filesystem::exists(marker, ec)) {
        std::ifstream mf(marker);
        std::string content;
        if (mf && std::getline(mf, content)) {
            if (content == "reset" || content == "v2") {
                return; // Already checked v2 or user explicitly reset
            }
        }
    }

    log::Tagged("TROPHY", "Initial setup: checking save files for retroactive milestone progress...");

    std::vector<std::filesystem::path> check_dirs = {
        m_saves_dir / "Slot00",
        m_root_path / "assets" / "savedata0" / "Slot00",
        m_saves_dir / "Slot01",
        m_root_path / "assets" / "savedata0" / "Slot01"
    };

    bool has_save = false;
    for (const auto& slot_dir : check_dirs) {
        auto v2game = slot_dir / "V2GAME01";
        auto game = slot_dir / "GAME01";

        if (std::filesystem::exists(v2game, ec) || std::filesystem::exists(game, ec)) {
            auto target = std::filesystem::exists(v2game, ec) ? v2game : game;
            if (std::filesystem::file_size(target, ec) > 1024) {
                has_save = true;
                break;
            }
        }
    }

    if (has_save) {
        // Player has an existing save with prologue completed:
        // 1. Follow the LEGO Brick Road (prologue complete) -> Trophy 1
        // 2. Rebuild the LEGO Gateway (happens in Vorton immediately after prologue) -> Trophy 15
        // 3. Visit any Hub World for the First Time (Vorton is a hub world) -> Trophy 21
        int plat = -1;
        UnlockTrophy(1, &plat, false);
        UnlockTrophy(15, &plat, false);
        UnlockTrophy(21, &plat, false);
        log::Tagged("TROPHY", "Retroactive save check: unlocked Trophies 1, 15, and 21.");
    }

    SaveTrophyXml();

    std::ofstream m(marker, std::ios::trunc);
    if (m) m << "v2\n";
}

} // namespace hle::trophy
