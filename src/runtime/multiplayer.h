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
void Disconnect(); // Fase 83: sai da sala (os fantasmas somem, depois o multiplayer desliga)
bool Leaving();    // saindo (esperando os fantasmas sumirem)
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
std::string LastError(); // 4.4.5: motivo da ultima falha de conexao (vazio = nenhum)
void ClearLastError();       // 4.4.6: a janela do F3 limpa o aviso antigo ao abrir
// 4.4.9: estado do multiplayer numa linha, para o painel de logs (F9 -> MULTIPLAYER)
std::string DebugSummary();
// 4.4.17: espaco da direcao da caminhada, mudavel na hora pelo painel F4 (vale para a sala toda)
// 0 = automatico (o port descobre), 1 = espaco do corpo (local), 2 = espaco do mundo
int MoveMode();
void SetMoveMode(int mode, bool broadcast = true);
std::string MoveModeName();

// 4.1.1: campanha junto: os outros jogadores vao para a fase em que o dono da sala esta (padrao: ligado)
void SetFollowOwner(bool on);
bool FollowOwnerEnabled();

} // namespace runtime::multiplayer
