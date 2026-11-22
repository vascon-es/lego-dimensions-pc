#include "common/cutscene_patch.h"
#include "common/game_region.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <stdexcept>

void check(bool value) { if (!value) throw std::runtime_error("region/patch check failed"); }
void sfo(const std::filesystem::path& root, const std::string& tid) {
    std::filesystem::create_directories(root / "sce_sys");
    std::array<unsigned char, 55> bytes{};
    const auto put = [&](size_t at, uint32_t value) { std::memcpy(bytes.data()+at, &value, 4); };
    std::memcpy(bytes.data(), "\0PSF", 4); put(4,0x101); put(8,36); put(12,45); put(16,1);
    bytes[22]=4; bytes[23]=2; put(24,10); put(28,10);
    std::memcpy(bytes.data()+36,"TITLE_ID",9); std::memcpy(bytes.data()+45,tid.c_str(),10);
    std::ofstream(root / "sce_sys" / "param.sfo", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("ld-region-unit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        // 4.5: pacotes de DLC instalados (multiplayer: o mundo do dono da sala sem o save local liberar)
        {
            const auto game = root / "dlc-check" / "game";
            std::filesystem::create_directories(game / "sce_sys");
            const auto write_sfo = [](const std::filesystem::path& file, const std::vector<std::pair<std::string, std::string>>& kv) {
                std::filesystem::create_directories(file.parent_path());
                const size_t keys = 20 + 16 * kv.size();
                std::vector<unsigned char> bytes(keys, 0), values;
                for (size_t i = 0; i < kv.size(); ++i) {
                    const size_t row = 20 + 16 * i;
                    const uint16_t key_off = static_cast<uint16_t>(bytes.size() - keys);
                    const uint16_t format = kv[i].first == "CATEGORY" ? 0x0404 : 0x0204;
                    const uint32_t length = static_cast<uint32_t>(kv[i].second.size());
                    const uint32_t offset = static_cast<uint32_t>(values.size());
                    const uint32_t capacity = length + 1;
                    bytes.insert(bytes.end(), kv[i].first.begin(), kv[i].first.end());
                    bytes.push_back(0);
                    bytes.insert(bytes.end(), (4 - ((bytes.size() - keys) % 4)) % 4, 0); // chaves alinhadas
                    std::memcpy(bytes.data() + row, &key_off, 2);
                    std::memcpy(bytes.data() + row + 2, &format, 2);
                    std::memcpy(bytes.data() + row + 4, &length, 4);
                    std::memcpy(bytes.data() + row + 8, &capacity, 4);
                    std::memcpy(bytes.data() + row + 12, &offset, 4);
                    values.insert(values.end(), kv[i].second.begin(), kv[i].second.end());
                    values.push_back(0);
                }
                const uint32_t values_off = static_cast<uint32_t>(bytes.size());
                bytes.insert(bytes.end(), values.begin(), values.end());
                std::memcpy(bytes.data(), "\0PSF", 4);
                const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
                put32(4, 0x101); put32(8, static_cast<uint32_t>(keys)); put32(12, values_off); put32(16, static_cast<uint32_t>(kv.size()));
                std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            };
            write_sfo(game / "sce_sys" / "param.sfo", {{"TITLE_ID", "CUSA01176"}});
            check(game_region::SfoString(game / "sce_sys" / "param.sfo", "TITLE_ID") == "CUSA01176");
            check(game_region::SfoString(game / "sce_sys" / "param.sfo", "CATEGORY").empty());
            const auto addcont = root / "dlc-check" / "assets" / "addcont";
            write_sfo(addcont / "UP1018-CUSA01176_00-LEGODIMENSION0001" / "sce_sys" / "param.sfo",
                      {{"CATEGORY", "ac"}, {"CONTENT_ID", "UP1018-CUSA01176_00-LEGODIMENSION0001"}});
            write_sfo(addcont / "CUSA01176-PATCH" / "sce_sys" / "param.sfo",
                      {{"CATEGORY", "gd"}, {"CONTENT_ID", "UP1018-CUSA01176_00-PATCH0000000000"}});
            std::filesystem::create_directories(addcont / "CUSA01176-SEMPON" / "sce_sys");
            check(game_region::AddcontPacks(addcont).size() == 1);
            check(game_region::AddcontPacks(addcont).at(0).label == "LEGODIMENSION000"); // 16 caracteres, como o HLE
            check(game_region::AddcontPacks(root / "dlc-check" / "nao-existe").empty());
            check(game_region::HasAddcontContent(game, "")); // <jogo>/../assets/addcont
            const auto isolated = root / "dlc-vazio" / "game";
            write_sfo(isolated / "sce_sys" / "param.sfo", {{"TITLE_ID", "CUSA01176"}});
            check(!game_region::HasAddcontContent(isolated, root / "appdata-vazio")); // sem addcont em lugar nenhum
        }
        for (const std::string tid : {"CUSA01176","CUSA00935"}) {
            const auto dir = root / tid, game = dir / "renamed-game", patch = dir / (tid+"-patch");
            sfo(game,tid); sfo(patch,tid);
            check(game_region::TitleId(game)==tid && game_region::FindPatch(game)==patch);
            const auto opposite = tid=="CUSA01176" ? "CUSA00935" : "CUSA01176";
            sfo(patch,opposite); check(game_region::FindPatch(game).empty());
            const auto dlc = dir / "shadPS4" / "addcont" / tid;
            std::filesystem::create_directories(dlc);
            check(game_region::Addcont(game,dir)==dlc);
            std::filesystem::remove(dlc);
            std::filesystem::create_directories(dir/"shadPS4"/"addcont"/opposite);
            check(game_region::Addcont(game,dir).empty());
            std::ofstream(game/"sce_sys"/"param.sfo",std::ios::binary)<<"bad";
            check(game_region::TitleId(game).empty() && game_region::FindPatch(game).empty());
        }
        constexpr std::array<uint8_t,8> signature{0x8A,0x83,0xE8,0,0,0,0x24,1};
        std::array<uint8_t,64> bytes{};
        std::array<std::span<uint8_t>,2> segments{std::span(bytes).first(32),std::span(bytes).subspan(32)};
        check(!cutscene_patch::Apply(segments));
        // uma ocorrencia: libera a leitura e desarma SO a saida "test eax,0x4000000; je; xor eax,eax" da mesma funcao
        constexpr std::array<uint8_t,9> gate_a{0xA9,0x00,0x00,0x00,0x04,0x74,0x04,0x31,0xC0}; // obrigatoria
        constexpr std::array<uint8_t,4> gate_b{0xA8,0x01,0x74,0x04};                             // menu/atracao
        std::array<uint8_t,9> gate_b_tail{0x31,0xC0,0xEB,0x1D,0,0,0,0,0};
        std::copy(gate_b.begin(),gate_b.end(),bytes.begin()+4);
        std::copy(gate_b_tail.begin(),gate_b_tail.end(),bytes.begin()+8);
        std::copy(gate_a.begin(),gate_a.end(),bytes.begin()+14);
        std::copy(signature.begin(),signature.end(),bytes.begin()+24);
        {
            const auto result=cutscene_patch::Apply(segments);
            check(result && result.applied && result.gates==1 && result.at==24);
            check(bytes[24]==0xB0 && bytes[25]==1);
            check(bytes[21]==0xB0 && bytes[22]==1);   // a saida 0x4000000 foi desarmada
            check(bytes[8]==0x31 && bytes[9]==0xC0); // a do bit 0 continua "xor eax,eax"
        }
        // duas ocorrencias: ambiguo, nao pode mexer em nada (nem nas saidas antecipadas)
        std::copy(signature.begin(),signature.end(),bytes.begin());
        std::copy(signature.begin(),signature.end(),bytes.begin()+24);
        const auto before=bytes; check(!cutscene_patch::Apply(segments) && bytes==before);
        // variante sem o "and al,1": libera com 6 bytes e nao passa do fim do segmento
        std::array<uint8_t,16> tiny{};
        std::copy(bytes.begin(),bytes.begin()+6,tiny.begin());
        std::array<std::span<uint8_t>,1> tiny_segment{std::span(tiny)};
        check(cutscene_patch::Apply(tiny_segment) && tiny[0]==0xB0 && tiny[1]==1 && tiny[2]==0x90);
        // padrao partido entre segmentos nao conta
        std::array<uint8_t,7> split{};
        std::copy(signature.begin(),signature.begin()+7,split.begin());
        const auto split_before=split;
        std::array<std::span<uint8_t>,2> split_segments{std::span(split).first(3),std::span(split).subspan(3)};
        check(!cutscene_patch::Apply(split_segments) && split==split_before);
        std::filesystem::remove_all(root);
        std::cout<<"Region discovery and bounded cutscene patch checks passed\n";
        return 0;
    } catch (...) { std::filesystem::remove_all(root); throw; }
}
