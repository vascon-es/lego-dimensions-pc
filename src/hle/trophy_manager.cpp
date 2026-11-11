#include "trophy_manager.h"
#include "trophy_sounds.h"
#include "trophy_trp.h"
#include "hle/overrides/hle_filesystem.h"
#include "runtime/lang.h"
#include "hle_log.h"

#include <windows.h>
#include <mmsystem.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <thread>
#include <atomic>

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
        // 3.0: os icones vem do trophy00.trp do jogo do jogador (atualizacao primeiro: traz os das DLCs)
        const auto icons = dir / "NPWR07423_00" / "Icons";
        if (trophy_trp::EnsureIcons(icons, {hle::fs::App0Patch(), hle::fs::App0()})) {
            log::Tagged("TROPHY", "trophy icons ready in %s", icons.string().c_str());
        } else {
            log::Tagged("TROPHY", "trophy icons not found (no sce_sys\\trophy\\trophy00.trp in the game folder)");
        }
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

namespace {

// 3.0: sino curto feito aqui mesmo (nao e o som de console nenhum), para os estilos sem som embutido (Wii U, Switch).
// notas: (frequencia, inicio em s); cada uma decai sozinha.
std::vector<unsigned char> Chime(std::initializer_list<std::pair<float, float>> notes, float seconds, float decay) {
    constexpr int kRate = 44100;
    const int frames = static_cast<int>(seconds * kRate);
    std::vector<float> mix(frames, 0.0f);
    for (const auto& [freq, start] : notes) {
        for (int i = static_cast<int>(start * kRate); i < frames; ++i) {
            const float t = static_cast<float>(i) / kRate - start;
            const float env = std::exp(-t * decay) * std::min(1.0f, t * 400.0f);
            mix[i] += env * (std::sin(6.2831853f * freq * t) + 0.25f * std::sin(6.2831853f * 2.0f * freq * t));
        }
    }
    std::vector<unsigned char> wav(44 + frames * 2);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4), put32(4, static_cast<uint32_t>(36 + frames * 2)), std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16), put16(20, 1), put16(22, 1), put32(24, kRate), put32(28, kRate * 2), put16(32, 2), put16(34, 16);
    std::memcpy(&wav[36], "data", 4), put32(40, static_cast<uint32_t>(frames * 2));
    for (int i = 0; i < frames; ++i) {
        const float v = std::clamp(mix[i] * 0.28f, -1.0f, 1.0f);
        put16(44 + i * 2, static_cast<uint16_t>(static_cast<int16_t>(v * 32767.0f)));
    }
    return wav;
}

// mp3 (ou wav) por MCI numa thread propria: abre, toca ate o fim e fecha
void PlayMci(const std::filesystem::path& path) {
    static std::atomic<int> s_counter{0};
    const std::wstring alias = L"ld_trophy_" + std::to_wstring(++s_counter);
    std::thread([path, alias] {
        const std::wstring open = L"open \"" + path.wstring() + L"\" type mpegvideo alias " + alias;
        if (mciSendStringW(open.c_str(), nullptr, 0, nullptr) == 0) {
            mciSendStringW((L"play " + alias + L" wait").c_str(), nullptr, 0, nullptr);
            mciSendStringW((L"close " + alias).c_str(), nullptr, 0, nullptr);
        }
    }).detach();
}

} // namespace

void TrophyManager::PlayPreviewSound() {
    PlayTrophySound();
}

void TrophyManager::PlayTrophySound() {
    std::string choice, style;
    {
        std::lock_guard lock(m_mutex);
        choice = m_sound_type, style = m_popup_style;
    }
    if (choice == "off" || choice == "desligado" || choice == "none") {
        return;
    }
    // 3.0: "auto" = o som do console do pop-up; PlayStation / Xbox / Steam forcam a familia
    std::string key = style;
    if (choice == "playstation" && style.rfind("ps", 0) != 0) key = "ps5";
    if (choice == "xbox" && style.rfind("xbox", 0) != 0) key = "xbox";
    if (choice == "steam") key = "steam";
    const std::string file = key == "xbox" ? "xboxone" : key;

    // 1) o som que o jogador colocou: assets\trophy\sounds\<estilo>.wav / .mp3
    std::error_code ec;
    const auto sounds = m_root_path / "assets" / "trophy" / "sounds";
    if (const auto wav = sounds / (file + ".wav"); std::filesystem::exists(wav, ec)) {
        PlaySoundW(wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        return;
    }
    if (const auto mp3 = sounds / (file + ".mp3"); std::filesystem::exists(mp3, ec)) {
        PlayMci(mp3);
        return;
    }
    // 2) o som personalizado antigo (um para todos)
    if (const auto custom = m_root_path / "assets" / "custom_trophy" / "trophy.wav"; std::filesystem::exists(custom, ec)) {
        PlaySoundW(custom.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        return;
    }
    // 3) os sons que ja vem no port
    const void* sound = kTrophySoundPlaystation;
    if (key.rfind("xbox", 0) == 0) {
        sound = kTrophySoundXbox;
    } else if (key == "steam") {
        sound = kTrophySoundSteam;
    } else if (key == "wiiu") {
        static const auto s_wiiu = Chime({{1046.5f, 0.0f}, {1318.5f, 0.09f}, {1568.0f, 0.18f}, {2093.0f, 0.27f}}, 1.1f, 5.0f);
        sound = s_wiiu.data();
    } else if (key == "switch") {
        static const auto s_switch = Chime({{1318.5f, 0.0f}, {1975.5f, 0.075f}}, 0.55f, 9.0f);
        sound = s_switch.data();
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(sound), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
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
    if (m_sound_type == "auto") m_sound_type = "playstation";
    else if (m_sound_type == "playstation") m_sound_type = "xbox";
    else if (m_sound_type == "xbox") m_sound_type = "steam";
    else if (m_sound_type == "steam") m_sound_type = "off";
    else m_sound_type = "auto";
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
    static const char* const kOrder[] = {"ps5", "ps4", "ps3", "xbox", "xbox360", "wiiu", "steam", "switch"}; // trophy_popup
    size_t i = 0;
    while (i < 8 && m_popup_style != kOrder[i]) ++i;
    m_popup_style = kOrder[(i + 1) % 8];
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
