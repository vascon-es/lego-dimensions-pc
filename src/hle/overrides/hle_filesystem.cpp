// VFS do Orbis (/app0, /temp0, /savedata0) + I/O de arquivo do libkernel.
#include "hle/overrides/hle_radar.h"
#include "hle/overrides/hle_filesystem.h"
#include "runtime/lang.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <set>
#include <system_error>

#include <windows.h>

#include "hle/overrides/hle_override.h"

namespace hle::fs {

namespace {

struct Mount {
    std::string prefix; // "/app0"
    std::filesystem::path host;
    bool read_only;
};

std::mutex g_mounts_mutex;
std::vector<Mount> g_mounts;
std::filesystem::path g_app0;
std::filesystem::path g_app0_patch; // Fase 58: atualizacao em pasta separada (CUSA01176-patch), por cima do /app0
std::filesystem::path g_writable_root;
std::atomic<u64> g_dat_opened{0};
// Fase 64: mods
std::vector<std::filesystem::path> g_overlays;      // pastas no formato do /app0, a primeira vence
std::filesystem::path g_locale_overlay;             // overlay para traducao em portugues
std::map<std::string, PackRedirect> g_redirect_hdr;  // "data/chunk1/patch.hdr" -> pacote com arquivos de mod
std::map<std::string, PackRedirect> g_redirect_dat;  // "data/chunk1/patch.dat" -> idem
std::map<std::string, std::filesystem::path> g_anomaly_files; // Fase 67: "mods/allworld.dat" -> arquivo do mod

std::string LowerKey(const std::filesystem::path& rel) {
    const std::u8string generic = rel.generic_u8string();
    std::string key(generic.begin(), generic.end());
    for (char& c : key) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!key.empty() && key.back() == '/') {
        key.pop_back();
    }
    return key;
}

// Arquivo do /app0 sem os pacotes renomeados: mods de arquivos soltos, depois a atualizacao, depois o jogo.
std::filesystem::path FindApp0File(const std::filesystem::path& rel) {
    std::error_code ec;
    if (!g_locale_overlay.empty()) {
        if (const auto p = (g_locale_overlay / rel).lexically_normal(); std::filesystem::exists(p, ec)) {
            return p;
        }
        if (const auto p_flat = (g_locale_overlay / rel.filename()).lexically_normal(); std::filesystem::exists(p_flat, ec)) {
            return p_flat;
        }
    }
    for (const auto& dir : g_overlays) {
        if (const auto p = (dir / rel).lexically_normal(); std::filesystem::exists(p, ec)) {
            return p;
        }
    }
    if (!lang::Pt()) {
        const std::string lower = LowerKey(rel);
        if (lower == "data/chunk1/patch.dat") {
            const auto bak = g_app0 / "data" / "chunk1" / "backup" / "PATCH.DAT";
            if (std::filesystem::exists(bak, ec)) return bak;
        } else if (lower == "data/chunk1/patch.hdr") {
            const auto bak = g_app0 / "data" / "chunk1" / "backup" / "PATCH.HDR";
            if (std::filesystem::exists(bak, ec)) return bak;
        }
    }
    if (!g_app0_patch.empty()) {
        if (const auto p = (g_app0_patch / rel).lexically_normal(); std::filesystem::exists(p, ec)) {
            return p;
        }
    }
    const auto p = (g_app0 / rel).lexically_normal();
    return std::filesystem::exists(p, ec) ? p : std::filesystem::path{};
}

s64 FileTimeToUnix(const FILETIME& ft) {
    const u64 ticks = (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<s64>(ticks / 10000000ULL) - 11644473600LL;
}

int LastErrorToOrbis() {
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return kENOENT;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return kEEXIST;
    case ERROR_ACCESS_DENIED:
        return kEISDIR; // CreateFile em diretorio sem FILE_FLAG_BACKUP_SEMANTICS
    default:
        return kEIO;
    }
}

const char* ErrorName(int error) {
    switch (error) {
    case 0: return "OK";
    case kENOENT: return "NOT FOUND";
    case kEEXIST: return "ALREADY EXISTS";
    case kEISDIR: return "IS A DIRECTORY";
    case kENOTDIR: return "NOT A DIRECTORY";
    case kEROFS: return "READ ONLY (/app0)";
    default: return "I/O ERROR";
    }
}

void LogAccess(const char* api, const char* guest, const Resolved& r, int error, const char* extra = "") {
    if (!r.mounted) {
        log::Fs("%s \"%s\" -> NO MOUNT (outside /app0, /temp0, /download0 and mounted saves)", api, guest ? guest : "(null)");
    } else {
        log::Fs("%s \"%s\" -> %s%s  [%s]", api, guest, ErrorName(error), extra, PathText(r.host).c_str());
    }
}

} // namespace

// ------------------------------- File ---------------------------------------
File::File(void* handle, std::string guest_path, bool writable)
    : handle_(handle), guest_path_(std::move(guest_path)), writable_(writable) {}

File::~File() {
    CloseHandle(handle_);
    if (tail_) {
        CloseHandle(tail_);
    }
}

void File::SetTail(void* tail, u64 base, std::string pack) {
    tail_ = tail;
    tail_base_ = base;
    tail_radar_ = "/" + pack + "_MODS.DAT";
}

s64 File::PreadHost(void* handle, void* buffer, u64 size, s64 offset) {
    const auto t0 = std::chrono::steady_clock::now();
    u8* out = static_cast<u8*>(buffer);
    u64 total = 0;
    while (total < size) {
        OVERLAPPED ov{};
        const u64 at = static_cast<u64>(offset) + total;
        ov.Offset = static_cast<DWORD>(at);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        const DWORD chunk = static_cast<DWORD>(std::min<u64>(size - total, 0x40000000));
        DWORD got = 0;
        if (!ReadFile(handle, out + total, chunk, &got, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) {
                break;
            }
            return total ? static_cast<s64>(total) : -1;
        }
        if (!got) {
            break;
        }
        total += got;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms >= 2.0 || (ms >= 0.5 && (guest_path_.find(".DAT") != std::string::npos || guest_path_.find(".dat") != std::string::npos ||
                                    guest_path_.find(".HDR") != std::string::npos || guest_path_.find(".hdr") != std::string::npos))) {
        std::printf("[DIAG-IO] thread=%lu file=%s off=%lld sz=%llu took=%.2f ms\n",
                    GetCurrentThreadId(), guest_path_.c_str(), static_cast<long long>(offset), static_cast<unsigned long long>(total), ms);
    }
    return static_cast<s64>(total);
}

s64 File::Pread(void* buffer, u64 size, s64 offset) {
    if (tail_ && offset >= 0 && static_cast<u64>(offset) + size > tail_base_) {
        // Fase 64: .DAT com os arquivos de mod depois do fim: a parte de antes vem do .DAT, o resto da cauda
        u8* out = static_cast<u8*>(buffer);
        u64 head = static_cast<u64>(offset) < tail_base_ ? tail_base_ - static_cast<u64>(offset) : 0;
        s64 total = 0;
        if (head) {
            total = PreadHost(handle_, out, head, offset);
            if (total < static_cast<s64>(head)) {
                return total;
            }
            radar::OnRead(guest_path_, offset, head);
        }
        const u64 tail_at = static_cast<u64>(offset) + head - tail_base_;
        const s64 got = PreadHost(tail_, out + head, size - head, static_cast<s64>(tail_at));
        if (got > 0) {
            radar::OnRead(tail_radar_, static_cast<s64>(tail_at), static_cast<u64>(got));
            total += got;
        }
        return total ? total : got;
    }
    const s64 total = PreadHost(handle_, buffer, size, offset);
    if (total > 0) {
        radar::OnRead(guest_path_, offset, static_cast<u64>(total)); // radar de telas: qual arquivo do pacote e este
    }
    return total;
}

s64 File::Pwrite(const void* buffer, u64 size, s64 offset) {
    if (!writable_) {
        return -1;
    }
    const u8* in = static_cast<const u8*>(buffer);
    u64 total = 0;
    while (total < size) {
        OVERLAPPED ov{};
        const u64 at = static_cast<u64>(offset) + total;
        ov.Offset = static_cast<DWORD>(at);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        const DWORD chunk = static_cast<DWORD>(std::min<u64>(size - total, 0x40000000));
        DWORD put = 0;
        if (!WriteFile(handle_, in + total, chunk, &put, &ov) || !put) {
            return total ? static_cast<s64>(total) : -1;
        }
        total += put;
    }
    return static_cast<s64>(total);
}

s64 File::Read(void* buffer, u64 size) {
    std::lock_guard lock(mutex_);
    const s64 n = Pread(buffer, size, position_);
    if (n > 0) {
        position_ += n;
    }
    return n;
}

s64 File::Write(const void* buffer, u64 size) {
    std::lock_guard lock(mutex_);
    const s64 n = Pwrite(buffer, size, position_);
    if (n > 0) {
        position_ += n;
    }
    return n;
}

s64 File::Seek(s64 offset, int whence) {
    std::lock_guard lock(mutex_);
    s64 base = 0;
    if (whence == 1) {
        base = position_;
    } else if (whence == 2) {
        base = Size();
    } else if (whence != 0) {
        return -1;
    }
    if (base + offset < 0) {
        return -1;
    }
    position_ = base + offset;
    return position_;
}

s64 File::Size() {
    LARGE_INTEGER size;
    if (tail_) { // Fase 64: o .DAT ate o comeco da cauda + a cauda
        return GetFileSizeEx(tail_, &size) ? static_cast<s64>(tail_base_) + size.QuadPart : -1;
    }
    return GetFileSizeEx(handle_, &size) ? size.QuadPart : -1;
}

s64 File::ModifiedTime() {
    FILETIME write;
    return GetFileTime(handle_, nullptr, nullptr, &write) ? FileTimeToUnix(write) : 0;
}

// ----------------------------- resolvedor -----------------------------------
void Configure(const std::filesystem::path& app0, const std::filesystem::path& writable_root) {
    std::lock_guard lock(g_mounts_mutex);
    g_app0 = app0;
    g_writable_root = writable_root;
    // /savedataN nao e fixo: so existe entre sceSaveDataMount e sceSaveDataUmount.
    g_mounts = {
        {"/app0", app0, true},
        {"/temp0", writable_root / "temp0", false},
        {"/download0", writable_root / "download0", false},
    };
    std::error_code ec;
    std::filesystem::create_directories(writable_root / "savedata0", ec);
    for (const auto& m : g_mounts) {
        if (!m.read_only) {
            std::filesystem::create_directories(m.host, ec);
        }
    }
}

const std::filesystem::path& App0() {
    return g_app0;
}

void SetApp0Patch(const std::filesystem::path& patch_dir) {
    std::lock_guard lock(g_mounts_mutex);
    g_app0_patch = patch_dir;
}

void SetModOverlays(std::vector<std::filesystem::path> dirs) {
    std::lock_guard lock(g_mounts_mutex);
    g_overlays = std::move(dirs);
}

void SetLocaleOverlay(const std::filesystem::path& dir) {
    std::lock_guard lock(g_mounts_mutex);
    g_locale_overlay = dir;
}

void SetPackRedirects(std::vector<PackRedirect> redirects) {
    std::lock_guard lock(g_mounts_mutex);
    g_redirect_hdr.clear();
    g_redirect_dat.clear();
    for (PackRedirect& r : redirects) {
        g_redirect_hdr[LowerKey(r.real_hdr.lexically_normal())] = r;
        g_redirect_dat[LowerKey(r.real_dat.lexically_normal())] = std::move(r);
    }
}

void SetAnomalyFiles(std::map<std::string, std::filesystem::path> files) {
    std::lock_guard lock(g_mounts_mutex);
    g_anomaly_files = std::move(files);
}

namespace {
// Fase 64: o arquivo resolvido e um pacote com arquivos de mod? (tabela do cache / .DAT com cauda)
void ApplyPackRedirect(Resolved& r) {
    const std::string key = LowerKey(r.host);
    if (const auto it = g_redirect_hdr.find(key); it != g_redirect_hdr.end()) {
        r.host = it->second.cache_hdr;
    } else if (const auto dat = g_redirect_dat.find(key); dat != g_redirect_dat.end()) {
        r.tail = dat->second.tail;
        r.tail_base = dat->second.real_size;
        r.pack = dat->second.pack;
    }
}
} // namespace

const std::filesystem::path& App0Patch() {
    return g_app0_patch;
}

const std::filesystem::path& WritableRoot() {
    return g_writable_root;
}

void SetMount(const std::string& prefix, const std::filesystem::path& host, bool read_only) {
    std::lock_guard lock(g_mounts_mutex);
    std::erase_if(g_mounts, [&](const Mount& m) { return m.prefix == prefix; });
    g_mounts.push_back({prefix, host, read_only});
}

void RemoveMount(const std::string& prefix) {
    std::lock_guard lock(g_mounts_mutex);
    std::erase_if(g_mounts, [&](const Mount& m) { return m.prefix == prefix; });
}

u64 DatFilesOpened() {
    return g_dat_opened.load();
}

Resolved Resolve(const char* guest_path) {
    Resolved r;
    if (!guest_path || !*guest_path) {
        return r;
    }
    std::string p = guest_path;
    for (char& c : p) {
        if (c == '\\') {
            c = '/';
        }
    }
    // Caminho relativo: o diretorio corrente de um titulo do Orbis e /app0.
    if (p[0] != '/') {
        p = "/app0/" + p;
    }
    std::lock_guard lock(g_mounts_mutex);
    for (const auto& m : g_mounts) {
        if (p.compare(0, m.prefix.size(), m.prefix) != 0 ||
            (p.size() > m.prefix.size() && p[m.prefix.size()] != '/')) {
            continue;
        }
        const std::filesystem::path rel = std::filesystem::path(p.substr(m.prefix.size())).relative_path();
        const std::filesystem::path full = (m.host / rel).lexically_normal();
        // ".." nao pode escapar do mount
        const auto base = m.host.lexically_normal();
        const auto mismatch = std::mismatch(base.begin(), base.end(), full.begin(), full.end());
        if (mismatch.first != base.end() && !mismatch.first->empty()) {
            return r;
        }
        r.host = full;
        r.mounted = true;
        r.read_only = m.read_only;
        if (m.prefix == "/app0" && !g_anomaly_files.empty()) {
            // Fase 67: pacote de mod do Anomaly (/app0/mods/<nome>.HDR / .DAT)
            if (const auto it = g_anomaly_files.find(LowerKey(rel)); it != g_anomaly_files.end()) {
                r.host = it->second;
                if (!g_redirect_hdr.empty()) {
                    ApplyPackRedirect(r); // pacote de mod com arquivos consertados no cache (ex. hashes dos sons)
                }
                return r;
            }
        }
        if (m.prefix == "/app0") {
            // Fase 64 (mods de arquivos soltos) e Fase 58 (atualizacao separada): o primeiro que tiver o arquivo vence
            if (!g_overlays.empty() || !g_app0_patch.empty()) {
                if (const auto found = FindApp0File(rel); !found.empty()) {
                    r.host = found;
                }
            }
        }
        if (m.read_only && !g_redirect_hdr.empty()) {
            ApplyPackRedirect(r); // Fase 64: pacote do jogo ou de DLC com arquivos de mod
        }
        return r;
    }
    return r;
}

std::shared_ptr<File> Open(const char* api, const char* guest_path, OpenMode mode, int* error) {
    const Resolved r = Resolve(guest_path);
    int err = 0;
    HANDLE h = INVALID_HANDLE_VALUE;
    const bool wants_write = mode.write || mode.create || mode.truncate || mode.append;
    if (!r.mounted) {
        err = kENOENT;
    } else if (r.read_only && wants_write) {
        err = kEROFS;
    } else {
        std::error_code ec;
        if (std::filesystem::is_directory(r.host, ec)) {
            err = kEISDIR;
        } else {
            const DWORD access = GENERIC_READ | (wants_write ? GENERIC_WRITE : 0);
            const DWORD disposition = mode.create ? (mode.truncate ? CREATE_ALWAYS : OPEN_ALWAYS)
                                                  : (mode.truncate ? TRUNCATE_EXISTING : OPEN_EXISTING);
            h = CreateFileW(r.host.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disposition,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                err = LastErrorToOrbis();
            }
        }
    }
    std::shared_ptr<File> file;
    char extra[64] = "";
    if (!err) {
        const std::string ext = PathText(r.host.extension());
        if (_stricmp(ext.c_str(), ".dat") == 0) {
            g_dat_opened.fetch_add(1);
        }
        file = std::make_shared<File>(h, guest_path, wants_write);
        if (!r.tail.empty()) {
            HANDLE tail = CreateFileW(r.tail.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
            if (tail != INVALID_HANDLE_VALUE) {
                file->SetTail(tail, r.tail_base, r.pack);
            }
        }
        if (mode.append) {
            file->Seek(0, 2);
        }
        std::snprintf(extra, sizeof(extra), " (%s, %" PRId64 " bytes)", wants_write ? "write" : "read",
                      file->Size());
    }
    LogAccess(api, guest_path, r, err, extra);
    if (error) {
        *error = err;
    }
    return file;
}

int Stat(const char* api, const char* guest_path, StatInfo* out) {
    const Resolved r = Resolve(guest_path);
    int err = kENOENT;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (r.mounted && GetFileAttributesExW(r.host.c_str(), GetFileExInfoStandard, &data)) {
        err = 0;
        if (out) {
            out->is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            out->size = (static_cast<s64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            if (!r.tail.empty()) { // Fase 64: .DAT com a cauda dos mods
                std::error_code ec;
                out->size = static_cast<s64>(r.tail_base + std::filesystem::file_size(r.tail, ec));
            }
            out->mtime = FileTimeToUnix(data.ftLastWriteTime);
        }
    }
    LogAccess(api, guest_path, r, err);
    return err;
}

int ListDirectory(const char* api, const char* guest_path, std::vector<DirEntry>* out) {
    const Resolved r = Resolve(guest_path);
    int err = kENOENT;
    std::error_code ec;
    if (r.mounted && std::filesystem::exists(r.host, ec)) {
        if (!std::filesystem::is_directory(r.host, ec)) {
            err = kENOTDIR;
        } else {
            err = 0;
            // Fase 58/64: pasta do /app0 com mods de arquivos soltos ou atualizacao separada: junta todas (a
            // primeira que tiver o nome vence)
            std::vector<std::filesystem::path> dirs{r.host};
            bool app0 = false;
            {
                std::string p = guest_path ? guest_path : "";
                for (char& c : p) {
                    if (c == '\\') {
                        c = '/';
                    }
                }
                if (!p.empty() && p[0] != '/') {
                    p = "/app0/" + p;
                }
                app0 = p.compare(0, 5, "/app0") == 0 && (p.size() == 5 || p[5] == '/');
                if (app0) {
                    const auto rel = std::filesystem::path(p.substr(5)).relative_path();
                    if (!g_overlays.empty() || !g_app0_patch.empty()) {
                        dirs.clear();
                        for (const auto& over : g_overlays) {
                            dirs.push_back((over / rel).lexically_normal());
                        }
                        if (!g_app0_patch.empty()) {
                            dirs.push_back((g_app0_patch / rel).lexically_normal());
                        }
                        dirs.push_back((g_app0 / rel).lexically_normal());
                    }
                }
            }
            std::set<std::string> names;
            for (const auto& dir_path : dirs) {
                for (const auto& entry : std::filesystem::directory_iterator(dir_path, ec)) {
                    const std::string name = PathText(entry.path().filename());
                    std::string upper = name; // o Windows nao diferencia maiusculas: a mesma pasta em dois lugares conta uma vez
                    for (char& c : upper) {
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    }
                    if (!names.insert(upper).second) {
                        continue;
                    }
                    const bool dir = entry.is_directory(ec);
                    s64 size = dir ? 0 : static_cast<s64>(entry.file_size(ec));
                    if (!dir && !g_redirect_dat.empty()) { // Fase 64: .DAT com a cauda dos mods
                        if (const auto it = g_redirect_dat.find(LowerKey(entry.path().lexically_normal())); it != g_redirect_dat.end()) {
                            size = static_cast<s64>(it->second.real_size + std::filesystem::file_size(it->second.tail, ec));
                        }
                    }
                    out->push_back({name, dir, size});
                }
                ec.clear();
            }
        }
    }
    char extra[32] = "";
    if (!err) {
        std::snprintf(extra, sizeof(extra), " (%zu entries)", out->size());
    }
    LogAccess(api, guest_path, r, err, extra);
    return err;
}

int Remove(const char* api, const char* guest_path, bool directory) {
    const Resolved r = Resolve(guest_path);
    int err = 0;
    if (!r.mounted) {
        err = kENOENT;
    } else if (r.read_only) {
        err = kEROFS;
    } else if (directory ? !RemoveDirectoryW(r.host.c_str()) : !DeleteFileW(r.host.c_str())) {
        err = LastErrorToOrbis();
    }
    LogAccess(api, guest_path, r, err);
    return err;
}

int Rename(const char* api, const char* from, const char* to) {
    const Resolved a = Resolve(from);
    const Resolved b = Resolve(to);
    int err = 0;
    if (!a.mounted || !b.mounted) {
        err = kENOENT;
    } else if (a.read_only || b.read_only) {
        err = kEROFS;
    } else if (!MoveFileExW(a.host.c_str(), b.host.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = LastErrorToOrbis();
    }
    LogAccess(api, from, a, err);
    log::Fs("%s    to \"%s\"", api, to ? to : "(null)");
    return err;
}

int MakeDirectory(const char* api, const char* guest_path) {
    const Resolved r = Resolve(guest_path);
    int err = 0;
    if (!r.mounted) {
        err = kENOENT;
    } else if (r.read_only) {
        err = kEROFS;
    } else if (!CreateDirectoryW(r.host.c_str(), nullptr)) {
        err = LastErrorToOrbis();
    }
    LogAccess(api, guest_path, r, err);
    return err;
}

} // namespace hle::fs

// ============================ libkernel: arquivos ============================
namespace {

using namespace hle;

// Flags de open() do FreeBSD
constexpr int kOAccMode = 0x3;
constexpr int kOAppend = 0x8;
constexpr int kOCreat = 0x200;
constexpr int kOTrunc = 0x400;

std::mutex g_fd_mutex;
std::map<int, std::shared_ptr<fs::File>> g_fds;
int g_next_fd = 3; // 0-2 sao stdin/stdout/stderr

std::shared_ptr<fs::File> GetFd(int fd) {
    std::lock_guard lock(g_fd_mutex);
    const auto it = g_fds.find(fd);
    return it == g_fds.end() ? nullptr : it->second;
}

HLE_IMPL(int, sceKernelOpen, const char* path, int flags, u32 mode) {
    fs::OpenMode open_mode;
    open_mode.write = (flags & kOAccMode) != 0;
    open_mode.create = (flags & kOCreat) != 0;
    open_mode.truncate = (flags & kOTrunc) != 0;
    open_mode.append = (flags & kOAppend) != 0;
    int error = 0;
    auto file = fs::Open("sceKernelOpen", path, open_mode, &error);
    int result = error;
    if (file) {
        std::lock_guard lock(g_fd_mutex);
        result = g_next_fd++;
        g_fds[result] = std::move(file);
    }
    HLE_QUIET("\"%s\", 0x%x -> %d", path, flags, result);
    return result;
}
HLE_OVERRIDE(sceKernelOpen);

HLE_IMPL(int, sceKernelClose, int fd) {
    HLE_QUIET("%d", fd);
    std::lock_guard lock(g_fd_mutex);
    return g_fds.erase(fd) ? err::OK : fs::kEBADF;
}
HLE_OVERRIDE(sceKernelClose);

HLE_IMPL(s64, sceKernelRead, int fd, void* buffer, u64 size) {
    HLE_QUIET("%d, %p, %" PRIu64, fd, buffer, size);
    const auto file = GetFd(fd);
    if (!file) {
        return fs::kEBADF;
    }
    const s64 n = file->Read(buffer, size);
    return n < 0 ? fs::kEIO : n;
}
HLE_OVERRIDE(sceKernelRead);

HLE_IMPL(s64, sceKernelPread, int fd, void* buffer, u64 size, s64 offset) {
    HLE_QUIET("%d, %p, %" PRIu64 ", @%" PRId64, fd, buffer, size, offset);
    const auto file = GetFd(fd);
    if (!file) {
        return fs::kEBADF;
    }
    if (offset < 0) {
        return err::kEINVAL;
    }
    const s64 n = file->Pread(buffer, size, offset);
    return n < 0 ? fs::kEIO : n;
}
HLE_OVERRIDE(sceKernelPread);

HLE_IMPL(s64, sceKernelPwrite, int fd, const void* buffer, u64 size, s64 offset) {
    HLE_QUIET("%d, %p, %" PRIu64 ", @%" PRId64, fd, buffer, size, offset);
    const auto file = GetFd(fd);
    if (!file) {
        return fs::kEBADF;
    }
    if (offset < 0) {
        return err::kEINVAL;
    }
    const s64 n = file->Pwrite(buffer, size, offset);
    return n < 0 ? fs::kEBADF : n;
}
HLE_OVERRIDE(sceKernelPwrite);

HLE_IMPL(int, sceKernelUnlink, const char* path) {
    HLE_QUIET("\"%s\"", path);
    return fs::Remove("sceKernelUnlink", path, false);
}
HLE_OVERRIDE(sceKernelUnlink);

HLE_IMPL(int, sceKernelRename, const char* from, const char* to) {
    HLE_QUIET("\"%s\" -> \"%s\"", from, to);
    return fs::Rename("sceKernelRename", from, to);
}
HLE_OVERRIDE(sceKernelRename);

} // namespace
