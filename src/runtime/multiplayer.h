#pragma once
// Fase 79 (4.0): multiplayer online estilo Marvel's Spider-Man (cada um no seu jogo, os outros aparecem como
// personagens). So liga com --mp-server/--mp-room (caixa do launcher). Desligado: nenhum gancho, nenhuma rede,
// nenhum byte do jogo alterado. Detalhes em FASE79_MULTIPLAYER_PESQUISA.md.
#include <cstdint>
#include <string>
#include <vector>

namespace runtime::multiplayer {

struct Config {
    std::string server; // ws(s)://host/caminho
    std::string room;
    std::string name;
    std::string password; // senha da sala (LD_MP_PASS, posta pelo launcher fora da linha de comando)
};

void Configure(const Config& config); // antes do Install (argumentos da linha de comando)
bool Enabled();
bool Connect(const std::string& server, const std::string& room, const std::string& name, const std::string& password = {});
void Install(std::uint8_t* eboot_base); // ganchos no eboot + rede (so se Enabled)
void Tick();                            // thread principal, uma vez por quadro (leitura do controle do jogador 1)
void OnArea(const std::string& area);   // radar: o jogo leu o arquivo principal de uma area (\levels\...\X\X.txt)

// Nome em cima de cada jogador visivel (posicao no mundo, um pouco acima da cabeca). Qualquer thread.
struct NameTag {
    float x, y, z;
    std::string name;
};
std::vector<NameTag> NameTags();
bool InWorld(); // o jogador 1 esta numa fase (nao em menus/carregamento)

// 4.1.1: campanha junto: os outros jogadores vao para a fase em que o dono da sala esta (padrao: ligado)
void SetFollowOwner(bool on);
bool FollowOwnerEnabled();

} // namespace runtime::multiplayer
