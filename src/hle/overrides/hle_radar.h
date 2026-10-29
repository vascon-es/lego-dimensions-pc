#pragma once

#include <filesystem>
#include <string>

#include "common/types.h"

// "Radar de telas": diz QUAL arquivo do jogo esta sendo carregado.
//
// O eboot so abre os pacotes (.DAT); os arquivos de dentro sao lidos por offset, entao o nome
// nunca passa por sceKernelOpen / sceFios*. O radar traduz (pacote, offset lido) em nome usando
// os indices de tools/data/dat_index/<PACOTE>.tsv (gerados por tools/build_dat_index.py).
//
//   - Todo arquivo tocado pela primeira vez vai para o arquivo de log do radar, com o instante.
//   - Os que casam com o filtro de "tela" (.gsc, .txt, .csv, .lip, "screen") tambem saem no
//     terminal como  >>> [RADAR DE TELA] Carregando: <nome> <<<
namespace hle::radar {

void Configure(const std::filesystem::path& index_dir, const std::filesystem::path& log_file);
// Fase 64: pasta lida primeiro (assets\dat_index: indices dos pacotes do proprio jogador e dos mods).
void SetLocalIndexDir(const std::filesystem::path& dir);

// Chamado pelo VFS a cada leitura bem-sucedida. Ignora o que nao for pacote com indice.
void OnRead(const std::string& guest_path, s64 offset, u64 size);

// true se o nome casa com o filtro de "tela". `lower_name` em minusculas.
bool IsScreenFile(const std::string& lower_name);

// Arquivo guardado sem compressao num pacote que o jogo ja leu (o primeiro cujo nome termina em `lower_suffix`).
// O player de video usa para achar o .mp4: o jogo so passa "myfile.mp4" ao sceAvPlayerAddSource.
struct StoredFile {
    std::string guest_pack; // caminho do pacote no jogo ("/addcont15/DLC31.DAT2")
    std::string name;
    u64 offset = 0;
    u64 size = 0;
};
bool FindStoredFile(const std::string& lower_suffix, StoredFile* out);

u64 FilesSeen();
void PrintStats();

} // namespace hle::radar
