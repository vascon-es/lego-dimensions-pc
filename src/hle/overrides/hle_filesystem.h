#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/types.h"

// Sistema de arquivos virtual do Orbis, compartilhado por libkernel (sceKernelOpen...),
// libSceFios2 e fopen da libc. Todo acesso gera uma linha "[FS]" no log.
//
//   /app0/...       -> diretorio do jogo (--app0). SOMENTE LEITURA: e o dump original.
//   /temp0/...      -> assets/temp0/
//   /savedataN/...  -> assets/savedata0/<dirName>/, so enquanto sceSaveDataMount estiver ativo
//   /download0/...  -> assets/download0/
namespace hle::fs {

// Fase 60: caminho do host como texto UTF-8 (para log e mensagens). path::string() converte para a pagina de
// codigo ANSI e lanca excecao com caracteres fora dela (ex.: pastas de DLC com dois-pontos de largura cheia, U+FF1A, no nome).
inline std::string PathText(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

// Erros do Orbis usados pelo VFS (0x80020000 | errno do FreeBSD)
constexpr int kENOENT = static_cast<int>(0x80020002);
constexpr int kEBADF = static_cast<int>(0x80020009);
constexpr int kEEXIST = static_cast<int>(0x80020011);
constexpr int kENOTDIR = static_cast<int>(0x80020014);
constexpr int kEISDIR = static_cast<int>(0x80020015);
constexpr int kEROFS = static_cast<int>(0x8002001E);
constexpr int kEIO = static_cast<int>(0x80020005);

struct Resolved {
    std::filesystem::path host;
    bool mounted = false;   // false: caminho fora de qualquer mount conhecido
    bool read_only = false; // /app0
    // Fase 64: .DAT com a cauda dos mods (vazio = arquivo normal)
    std::filesystem::path tail;
    u64 tail_base = 0;
    std::string pack;
};

struct OpenMode {
    bool write = false;
    bool create = false;
    bool truncate = false;
    bool append = false;
};

struct DirEntry {
    std::string name;
    bool is_directory;
    s64 size;
};

struct StatInfo {
    bool is_directory;
    s64 size;
    s64 mtime; // segundos desde 1970
};

// Arquivo aberto no host. Leitura/escrita posicional nao mexe no cursor, como pread/pwrite.
class File {
public:
    File(void* handle, std::string guest_path, bool writable);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    s64 Read(void* buffer, u64 size);
    s64 Write(const void* buffer, u64 size);
    s64 Pread(void* buffer, u64 size, s64 offset);
    s64 Pwrite(const void* buffer, u64 size, s64 offset);
    s64 Seek(s64 offset, int whence); // 0=SET 1=CUR 2=END; devolve a nova posicao ou -1
    s64 Size();
    s64 ModifiedTime();
    const std::string& guest_path() const { return guest_path_; }
    // Fase 64: a partir de `base`, as leituras vem de `tail` (bytes dos mods depois do fim do .DAT do jogo)
    void SetTail(void* tail, u64 base, std::string pack);

private:
    s64 PreadHost(void* handle, void* buffer, u64 size, s64 offset);

    void* handle_;
    std::string guest_path_;
    void* tail_ = nullptr;
    u64 tail_base_ = 0;
    std::string tail_radar_; // "/PATCH_MODS.DAT": o radar acha o nome no indice que o mods.py escreveu
    bool writable_;
    s64 position_ = 0;
    std::mutex mutex_;
};

// `app0` e o dump do jogo; `writable_root` recebe temp0/, savedata0/ e download0/ (criados se faltarem).
void Configure(const std::filesystem::path& app0, const std::filesystem::path& writable_root);
const std::filesystem::path& App0();
// Fase 58: atualizacao numa pasta separada (formato do shadPS4: CUSA01176-patch). Os arquivos dela vencem
// os do jogo base no /app0; as listagens de pasta juntam as duas.
void SetApp0Patch(const std::filesystem::path& patch_dir);
const std::filesystem::path& App0Patch();
// Raiz dos mounts gravaveis (assets/).
const std::filesystem::path& WritableRoot();

// Fase 64: mods.
// Pastas no formato do /app0 (arquivos soltos, como a CUSA01176-mods do shadPS4): a primeira vence, e todas
// vencem a atualizacao e o jogo base; as listagens de pasta juntam tudo.
void SetModOverlays(std::vector<std::filesystem::path> dirs);
void SetLocaleOverlay(const std::filesystem::path& dir);
// Pacote do jogo com arquivos trocados por mods (preparado pelo tools/setup/mods.py): o jogo ve a tabela mudada
// (.HDR do cache) e o .DAT de verdade com a "cauda" dos bytes dos mods depois do fim dele.
// Vale para qualquer mount (o /app0 e as pastas de DLC): a troca e pelo arquivo do host.
struct PackRedirect {
    std::string pack;                // "PATCH", "DLC10"
    std::filesystem::path real_hdr;  // a tabela do jogo (.HDR / .HDR2), trocada pela do cache
    std::filesystem::path cache_hdr; // tabela mudada
    std::filesystem::path real_dat;  // o .DAT / .DAT2 do jogo (so lido)
    u64 real_size = 0;               // tamanho dele quando o cache foi feito: a cauda comeca ai
    std::filesystem::path tail;      // bytes dos mods
};
void SetPackRedirects(std::vector<PackRedirect> redirects);
// Fase 67: pasta /app0/mods virtual do Anomaly: "mods/<nome>.hdr" / "mods/<nome>.dat" (minusculas) -> arquivo do
// mod na pasta de mods do port.
void SetAnomalyFiles(std::map<std::string, std::filesystem::path> files);

// Mounts criados em tempo de execucao (saves). SetMount substitui um prefixo ja existente.
void SetMount(const std::string& prefix, const std::filesystem::path& host, bool read_only);
void RemoveMount(const std::string& prefix);

// Quantas aberturas bem-sucedidas de arquivos .DAT (pacotes de dados do jogo) ja ocorreram.
u64 DatFilesOpened();

Resolved Resolve(const char* guest_path);

// `api` e o nome da funcao do Orbis, so para o log. Em erro devolve nullptr e preenche *error.
std::shared_ptr<File> Open(const char* api, const char* guest_path, OpenMode mode, int* error);
int Stat(const char* api, const char* guest_path, StatInfo* out);
int ListDirectory(const char* api, const char* guest_path, std::vector<DirEntry>* out);
int Remove(const char* api, const char* guest_path, bool directory);
int Rename(const char* api, const char* from, const char* to);
int MakeDirectory(const char* api, const char* guest_path);

} // namespace hle::fs
