// Fase 49/50: launcher do LEGO Dimensions PC.
// Pede a pasta do jogo (dump do PS4, CUSA01176) e a das DLCs, roda a preparacao na primeira vez (tools\setup\preparar.py:
// eboot + shaders AOT) e abre o port (build\lego_dimensions.exe) com as opcoes escolhidas. Nada do jogo vem com o port.
//
// Visual (1.8.7): o do launcher do port de PC de inFAMOUS Second Son, de Castlesea (github.com/Castlesea,
// GPL-2.0-or-later): uma tela so, desenhada inteira (sem controles do Windows), menu a esquerda com JOGAR numa barra
// amarela, a arte do jogo atras escurecida a esquerda, barra de selecao que desliza, abas nas Configuracoes com linhas
// "rotulo / dica / < valor >", dicas de teclas embaixo e teclado / controle em tudo. A arte e a fonte sao as do LEGO
// Dimensions do proprio jogador: sce_sys\pic1.png do dump e a fonte do jogo (\gui\font\font_nxg.ft2), copiada para
// assets\launcher\ por tools\setup\launcher_art.py. Sem elas (antes de escolher o jogo) o launcher usa Segoe UI.
#define UNICODE
#define _UNICODE
#include "ui.h"
#include "../../src/common/game_region.h"

#include <shellapi.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <tlhelp32.h>
#include <xinput.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "      \
                        "processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
using ui::S;

namespace {

// A tela tem 960 x 600 unidades (como a do inFAMOUS), em pixels vezes ui::g_scale.
constexpr int kW = 960, kH = 600, kBar = 30;
constexpr UINT WM_APP_LINE = WM_APP + 1, WM_APP_DONE = WM_APP + 2, WM_APP_FONT = WM_APP + 3;

HWND g_main;
bool g_self_test = false;
bool Prepared();
bool AotReady();
std::wstring Utf8ToWide(const std::string& text);
void Redraw();

// Fase 61: o que o processo em segundo plano esta fazendo (preparar, procurar atualizacao, atualizar; Fase 68: extrair PKGs)
enum class Task { kPrepare, kCheck, kUpdate, kPkg } g_task = Task::kPrepare;
std::wstring g_version, g_new_version, g_notes;

// Fase 61: tamanhos da janela do jogo (o jogo desenha em 1920x1080 e a imagem e escalada para a janela)
const wchar_t* const kResolutions[] = {L"1920x1080", L"1280x720", L"1600x900", L"2560x1440", L"3840x2160"};
// Idioma do jogo e do port: codigo do --lang, nome no menu (pt / en). Fora pt / en, o port fala ingles.
struct Language {
    const wchar_t *code, *pt, *en;
};
const Language kLanguages[] = {
    {L"auto", L"Autom\u00e1tico (Windows)", L"Automatic (Windows)"},
    {L"pt", L"Portugu\u00eas (Brasil)", L"Portugu\u00eas (Brasil)"},
    {L"en", L"English", L"English"},
    {L"es", L"Espa\u00f1ol", L"Espa\u00f1ol"},
    {L"fr", L"Fran\u00e7ais", L"Fran\u00e7ais"},
    {L"de", L"Deutsch", L"Deutsch"},
    {L"it", L"Italiano", L"Italiano"},
    {L"nl", L"Nederlands", L"Nederlands"},
    {L"da", L"Dansk", L"Dansk"},
};
std::wstring g_res_value = L"1920x1080", g_lang_value = L"en";
// Fase 62: icones dos botoes (--button-icons): pelo controle ligado ou fixos numa plataforma
struct IconChoice {
    const wchar_t *code, *pt, *en;
};
const IconChoice kIconChoices[] = {
    {L"auto", L"Autom\u00e1tico", L"Automatic"}, {L"ps4", L"PS4", L"PS4"}, {L"ps3", L"PS3", L"PS3"},
    {L"360", L"Xbox 360", L"Xbox 360"},             {L"xboxone", L"Xbox One", L"Xbox One"}, {L"wiiu", L"Wii U", L"Wii U"},
};
std::wstring g_icons_value = L"auto";
// Fase 74: bibliotecas de pipeline do Vulkan (GPL). Automatico = o port decide pelo fabricante (NVIDIA / Intel sim, AMD nao).
const IconChoice kGplChoices[] = {
    {L"auto", L"Autom\u00e1tico", L"Automatic"}, {L"ligado", L"Ligado", L"On"}, {L"desligado", L"Desligado", L"Off"},
};
std::wstring g_gpl_value = L"auto";

const IconChoice& CurrentGpl() {
    for (const IconChoice& choice : kGplChoices) {
        if (g_gpl_value == choice.code) {
            return choice;
        }
    }
    return kGplChoices[0];
}

const IconChoice& CurrentIcons() {
    for (const IconChoice& choice : kIconChoices) {
        if (g_icons_value == choice.code) {
            return choice;
        }
    }
    return kIconChoices[0];
}
std::unique_ptr<Gdiplus::Bitmap> g_art;
HICON g_icon;
HFONT g_font, g_small, g_label, g_button; // a janelinha dos apelidos (controles do Windows)
HBRUSH g_field_brush, g_bg_brush;
fs::path g_root;
bool g_dev_tree = false; // Fase 66: launcher dentro da pasta de desenvolvimento (fontes + build\): sem Atualizar nem limpeza
bool g_busy = false;
int g_pct = 0;
std::wstring g_step;
struct Status {
    std::wstring game, patch, dlcs, shaders;
    COLORREF game_c, patch_c, dlcs_c, shaders_c;
} g_status;

// As pastas e as opcoes (launcher.ini)
std::wstring g_game_dir, g_patch_dir, g_dlc_dir;
bool g_opt_fps = true, g_opt_console = false, g_opt_fullscreen = false, g_opt_skip = false, g_opt_overlay = false, g_opt_toypad = false;
std::wstring g_lang_at_start; // o idioma do launcher mudou nas Configuracoes: ele reabre ao voltar
std::deque<std::wstring> g_log_lines; // o que a preparacao / os mods / o jogo disseram (as ultimas linhas)

fs::path Ini() { return g_root / L"launcher.ini"; }

// Caminho digitado / colado: sem aspas nem barra no fim.
std::wstring CleanPath(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\\' || s.back() == L'"')) {
        s.pop_back();
    }
    while (!s.empty() && s.front() == L'"') {
        s.erase(s.begin());
    }
    return s;
}

// Texto de um campo do Windows (a janelinha dos apelidos), sem aspas nem barra no fim.
std::wstring GetText(HWND h) {
    std::wstring s(GetWindowTextLengthW(h) + 1, L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()));
    s.resize(wcslen(s.c_str()));
    return CleanPath(s);
}
std::wstring ReadIni(const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t buffer[2048];
    GetPrivateProfileStringW(L"launcher", key, fallback, buffer, 2048, Ini().c_str());
    return buffer;
}

void WriteIni(const wchar_t* key, const std::wstring& value) { WritePrivateProfileStringW(L"launcher", key, value.c_str(), Ini().c_str()); }


void AppendLog(const std::wstring& text) {
    g_log_lines.push_back(ui::LogText(text));
    while (g_log_lines.size() > 200) {
        g_log_lines.pop_front();
    }
    Redraw();
}

void EnsureLegoFont();
void ResetBackdrop();

// ---- arte do jogo: PNG do proprio dump ----
void LoadArt(const fs::path& game) {
    g_art.reset();
    const fs::path icon_file=game/L"sce_sys"/L"icon0.png";
    std::error_code icon_error;
    if(!game.empty() && fs::is_regular_file(icon_file,icon_error)) {
        Gdiplus::Bitmap bitmap(icon_file.c_str()); HICON icon=nullptr;
        if(bitmap.GetLastStatus()==Gdiplus::Ok && bitmap.GetHICON(&icon)==Gdiplus::Ok) {
            HICON previous=g_icon;g_icon=icon;
            SendMessageW(g_main,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon));
            SendMessageW(g_main,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(icon));
            if(previous)DestroyIcon(previous);
        }
    }
    for (const wchar_t* name : {L"pic1.png", L"pic0.png", L"icon0.png"}) {
        std::error_code ec;
        const fs::path file = game / L"sce_sys" / name;
        if (!game.empty() && fs::is_regular_file(file, ec)) {
            auto bitmap = std::make_unique<Gdiplus::Bitmap>(file.c_str());
            if (bitmap->GetLastStatus() == Gdiplus::Ok && bitmap->GetWidth()) {
                g_art = std::move(bitmap);
                break;
            }
        }
    }
    ResetBackdrop();
    EnsureLegoFont();
    Redraw();
}

// ---- estado ----
bool Prepared() {
    std::error_code ec;
    return fs::is_regular_file(g_root / L"assets" / L"decrypted_elfs" / L"eboot.bin.elf", ec);
}

bool AotReady() {
    std::error_code ec;
    return fs::is_regular_file(g_root / L"assets" / L"shaders_aot" / L"aot_pairs.pak", ec);
}

// Fase 58: atualizacao separada (formato do shadPS4), dentro da pasta do jogo ou ao lado dela.
fs::path FindPatch(const fs::path& game) {
    return game_region::FindPatch(game);
}

// Arquivos .DAT da atualizacao (PATCH*.DAT) na pasta: o dump ja traz a atualizacao junto?
bool HasPatchDats(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator chunk(dir / L"data", ec), end; !ec && chunk != end; chunk.increment(ec)) {
        std::error_code ec2;
        for (fs::directory_iterator f(chunk->path(), ec2), e2; !ec2 && f != e2; f.increment(ec2)) {
            const std::wstring n = f->path().filename().wstring();
            if (n.size() > 5 && _wcsnicmp(n.c_str(), L"PATCH", 5) == 0) {
                return true;
            }
        }
    }
    return false;
}

int CountDlcs(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        return 0;
    }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        n += fs::is_regular_file(it->path() / L"sce_sys" / L"param.sfo", ec);
    }
    return n;
}

fs::path FindDlc(const fs::path& game) {
    std::error_code ec;
    if (game.empty()) {
        return {};
    }
    const std::wstring name = game.filename().wstring();
    for (const fs::path& dir : {game / L"dlcs", game / L"Dlcs",
                                game.parent_path() / L"dlcs" / name,
                                game.parent_path() / L"Dlcs" / name,
                                game.parent_path() / L"addcont" / name,
                                game / L"addcont" / name,
                                game.parent_path() / L"Dlcs" / L"CUSA00935",
                                game.parent_path() / L"Dlcs" / L"CUSA01176",
                                game.parent_path() / L"dlcs" / L"CUSA00935",
                                game.parent_path() / L"dlcs" / L"CUSA01176"}) {
        if (CountDlcs(dir) > 0) {
            return dir;
        }
    }
    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
        const fs::path shadps4 = fs::path(appdata) / L"shadPS4" / L"addcont";
        if (CountDlcs(shadps4 / name) > 0) return shadps4 / name;
        if (CountDlcs(shadps4 / L"CUSA00935") > 0) return shadps4 / L"CUSA00935";
        if (CountDlcs(shadps4 / L"CUSA01176") > 0) return shadps4 / L"CUSA01176";
    }
    return {};
}

std::wstring BrowseFolder(const wchar_t* title, const std::wstring& start) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(title);
        IShellItem* folder = nullptr;
        if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
        if (SUCCEEDED(dialog->Show(g_main))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    return result;
}


bool GameFolderOk() {
    std::error_code ec;
    const fs::path game = g_game_dir;
    return !game.empty() && fs::is_regular_file(game / L"eboot.bin", ec) && fs::is_regular_file(game / L"sce_sys" / L"param.sfo", ec);
}

void UpdateStatus() {
    const fs::path game = g_game_dir, dlc = g_dlc_dir;
    std::error_code ec;
    const bool game_ok = GameFolderOk();
    g_status.game = game.empty() ? ui::T(L"Escolha a pasta do jogo", L"Choose the game folder") : game_ok ? ui::T(L"Jogo encontrado", L"Game found") : ui::T(L"Pasta sem eboot.bin", L"Folder without eboot.bin");
    g_status.game_c = game.empty() ? ui::kAmber : game_ok ? ui::kGreen : ui::kRed;
    const fs::path patch = g_patch_dir;
    const bool patch_ok = !patch.empty() && (fs::is_regular_file(patch / L"eboot.bin", ec) || fs::is_directory(patch / L"data", ec));
    const bool merged = game_ok && HasPatchDats(game); // atualizacao ja junto do jogo (como no dump mesclado)
    g_status.patch = patch_ok ? ui::T(L"Atualização separada", L"Separate update") : !patch.empty() ? ui::T(L"Atualização: pasta inválida", L"Update: invalid folder")
                     : merged  ? ui::T(L"Atualização no jogo", L"Update included in the game")
                     : game_ok ? ui::T(L"Sem atualização (1.24 necessária)", L"No update (1.24 required)")
                               : ui::T(L"Atualização", L"Update");
    g_status.patch_c = patch_ok || merged ? ui::kGreen : !patch.empty() ? ui::kRed : game_ok ? ui::kAmber : ui::kMuted;
    const int dlcs = CountDlcs(dlc);
    g_status.dlcs = dlcs ? std::to_wstring(dlcs) + (dlcs == 1 ? L" DLC" : L" DLCs") : ui::T(L"Sem DLCs", L"No DLCs");
    g_status.dlcs_c = dlcs ? ui::kBlue : ui::kMuted;
    g_status.shaders = !Prepared() ? ui::T(L"Preparar pendente", L"Prepare pending") : AotReady() ? ui::T(L"Shaders AOT prontos", L"Prepared") : ui::T(L"Sem shaders AOT", L"Preparation needed");
    g_status.shaders_c = !Prepared() ? ui::kAmber : AotReady() ? ui::kGreen : ui::kAmber;
    // dica do proximo passo (so troca a dica anterior; o texto de uma tarefa fica ate a proxima)
    const std::wstring hints[] = {ui::T(L"Escolha a pasta do jogo em Configurações > Arquivos.", L"Choose the game folder in Settings > Files."),
                                  ui::T(L"Preparar: gera os arquivos e traduz os shaders a partir do seu jogo (só na primeira vez).",
                                        L"Prepare: it builds the files and translates the shaders from your game (first time only)."),
                                  ui::T(L"Tudo pronto. Bom jogo!", L"All set. Have fun!")};
    if (!g_busy && (g_step.empty() || std::find(std::begin(hints), std::end(hints), g_step) != std::end(hints))) {
        g_step = hints[!game_ok ? 0 : !Prepared() ? 1 : 2];
    }
    Redraw();
}

void SaveSettings() {
    if (g_self_test) return;
    WriteIni(L"jogo", g_game_dir);
    WriteIni(L"atualizacao", g_patch_dir);
    WriteIni(L"dlcs", g_dlc_dir);
    WriteIni(L"fps60", g_opt_fps ? L"1" : L"0");
    WriteIni(L"console", g_opt_console ? L"1" : L"0");
    WriteIni(L"telacheia", g_opt_fullscreen ? L"1" : L"0");
    WriteIni(L"resolucao", g_res_value);
    WriteIni(L"icones", g_icons_value);
    WriteIni(L"gpl", g_gpl_value); // Fase 74
    WriteIni(L"idioma", g_lang_value);
    WriteIni(L"pularcenas", g_opt_skip ? L"1" : L"0");
    WriteIni(L"contador", g_opt_overlay ? L"1" : L"0"); // Fase 65
    WriteIni(L"toypad_auto", g_opt_toypad ? L"1" : L"0");
}

const Language& CurrentLanguage() {
    for (const Language& language : kLanguages) {
        if (g_lang_value == language.code) {
            return language;
        }
    }
    return kLanguages[0];
}

// ---- as escolhas das Configuracoes: < valor > ----
template <typename List, typename Get>
size_t IndexIn(const List& list, const std::wstring& value, Get get) {
    for (size_t i = 0; i < std::size(list); ++i) {
        if (value == get(list[i])) return i;
    }
    return 0;
}

template <typename List, typename Get>
std::wstring Step(const List& list, const std::wstring& value, int step, Get get) {
    const size_t n = std::size(list);
    return get(list[(IndexIn(list, value, get) + n + (step < 0 ? n - 1 : 1)) % n]);
}

bool LauncherPt(const std::wstring& lang) {
    return lang == L"pt" || (lang == L"auto" && PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE);
}

// Os textos do launcher sao portugues so com "pt" (ou automatico num Windows em portugues): se isso mudou nas
// Configuracoes, o launcher reabre ao sair delas.
void RestartForLanguage() {
    if (LauncherPt(g_lang_value) == ui::Pt() || g_busy || g_self_test) {
        return;
    }
    SetEnvironmentVariableW(L"LD_LANG", LauncherPt(g_lang_value) ? L"pt" : L"en");
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    std::wstring command = L"\"" + std::wstring(exe) + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, g_root.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DestroyWindow(g_main);
    }
}

void BrowseGame() {
    const std::wstring folder = CleanPath(BrowseFolder(ui::T(L"Pasta do jogo (dump CUSA01176)", L"Game folder (CUSA01176 / CUSA00935 dump)"), g_game_dir));
    if (folder.empty()) {
        return;
    }
    g_game_dir = folder;
    LoadArt(folder);
    if (g_patch_dir.empty()) { // Fase 58: acha a pasta da atualizacao sozinha
        g_patch_dir = FindPatch(folder).wstring();
    }
    if (g_dlc_dir.empty() || CountDlcs(g_dlc_dir) == 0) {
        g_dlc_dir = FindDlc(folder).wstring();
    }
    SaveSettings();
    UpdateStatus();
}

void BrowsePatch() {
    const std::wstring folder = CleanPath(BrowseFolder(ui::T(L"Pasta da atualização (CUSA01176-patch)", L"Update folder (CUSA01176-patch / CUSA00935-patch)"),
                                                       g_patch_dir.empty() ? g_game_dir : g_patch_dir));
    if (!folder.empty()) {
        g_patch_dir = folder;
        SaveSettings();
        UpdateStatus();
    }
}

void BrowseDlc() {
    const std::wstring folder = CleanPath(BrowseFolder(ui::T(L"Pasta das DLCs (uma subpasta por DLC)", L"DLC folder (one subfolder per DLC)"), g_dlc_dir));
    if (!folder.empty()) {
        g_dlc_dir = folder;
        SaveSettings();
        UpdateStatus();
    }
}

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// ---- preparacao: python tools\setup\preparar.py, saida lida por uma thread ----
// Roda um comando em segundo plano; a saida chega linha a linha em OnLine e o fim em WM_APP_DONE.
bool RunCommand(std::wstring command, Task task, const wchar_t* fail_pt, const wchar_t* fail_en) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    CreatePipe(&read_pipe, &write_pipe, &sa, 0);
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, g_root.c_str(),
                        &si, &pi)) {
        AppendLog(ui::T(fail_pt, fail_en));
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return false;
    }
    CloseHandle(write_pipe);
    g_busy = true;
    g_task = task;
    g_pct = 1;
    std::thread([read_pipe, pi] {
        std::string pending;
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read_pipe, buffer, sizeof(buffer), &got, nullptr) && got) {
            pending.append(buffer, got);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                while (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const int n = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0);
                auto* text = new std::wstring(n, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), text->data(), n);
                PostMessageW(g_main, WM_APP_LINE, 0, reinterpret_cast<LPARAM>(text));
            }
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(read_pipe);
        PostMessageW(g_main, WM_APP_DONE, code, 0);
    }).detach();
    return true;
}

// Roda um script de tools\setup no Python portatil (RunCommand)
bool RunPython(const wchar_t* script, const std::wstring& args, Task task) {
    fs::path python = g_root / L"tools" / L"python" / L"python.exe";
    std::error_code ec;
    std::wstring command = fs::is_regular_file(python, ec) ? Quote(python.wstring()) : L"python";
    command += L" -u " + Quote((g_root / L"tools" / L"setup" / script).wstring()) + args;
    return RunCommand(command, task, L"ERROR: could not start Python.", L"ERROR: could not start Python.");
}

void RunPrepare() {
    SaveSettings();
    std::wstring args = L" --app0 " + Quote(g_game_dir);
    const std::wstring dlc = g_dlc_dir;
    if (!dlc.empty()) {
        args += L" --addcont " + Quote(dlc);
    }
    if (const std::wstring patch = g_patch_dir; !patch.empty()) {
        args += L" --patch " + Quote(patch); // Fase 58
    }
    if (RunPython(L"preparar.py", args, Task::kPrepare)) {
        g_step = ui::T(L"Preparando... (a tradu\u00e7\u00e3o dos shaders leva de 20 a 60 minutos, s\u00f3 na primeira vez)", L"Preparing... (translating the shaders takes 20 to 60 minutes, first time only)");
        UpdateStatus();
    }
}

// ---- Fase 61: atualizacao pelas releases do GitHub (tools\setup\atualizar.py) ----
bool GameRunning() {
    bool running = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snap, &entry); ok && !running; ok = Process32NextW(snap, &entry)) {
        running = _wcsicmp(entry.szExeFile, L"lego_dimensions.exe") == 0;
    }
    CloseHandle(snap);
    return running;
}

void CheckUpdate() {
    if (g_busy) {
        return;
    }
    if (g_dev_tree) { // Fase 66: a release do GitHub trocaria o build e as ferramentas da pasta de desenvolvimento
        MessageBoxW(g_main,
                    ui::T(L"Este launcher está na pasta de desenvolvimento: ele usa o build\\lego_dimensions.exe compilado aqui, com os seus shaders e "
                          L"pipelines. Atualize compilando (build.bat); o Atualizar do GitHub só vale na versão instalada.",
                          L"This launcher is in the development folder: it uses the build\\lego_dimensions.exe built here, with your shaders and pipelines. "
                          L"Update by building (build.bat); Check updates from GitHub only applies to the installed version."),
                    L"LEGO Dimensions PC", MB_ICONINFORMATION);
        return;
    }
    g_new_version.clear();
    g_notes.clear();
    if (RunPython(L"atualizar.py", L" --check", Task::kCheck)) {
        g_step = ui::T(L"Procurando atualiza\u00e7\u00f5es...", L"Looking for updates...");
        UpdateStatus();
    }
}

void ApplyUpdate() {
    if (GameRunning()) {
        MessageBoxW(g_main, ui::T(L"Feche o jogo antes de atualizar.", L"Close the game before updating."), L"LEGO Dimensions PC", MB_ICONINFORMATION);
        return;
    }
    SaveSettings();
    if (RunPython(L"atualizar.py", L" --apply", Task::kUpdate)) {
        g_step = ui::T(L"Atualizando...", L"Updating...");
        UpdateStatus();
    }
}

void RestartLauncher() {
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    std::wstring command = Quote(exe);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, g_root.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DestroyWindow(g_main);
    }
}

// Updater backups are preserved. A suffix alone does not identify an updater-owned
// file: a recursive .novo/.antigo cleanup can otherwise remove modder backups.

std::wstring ReadVersion() {
    // Fase 66: na pasta de desenvolvimento a versao vem do tools\dist\VERSION
    std::ifstream in(g_dev_tree ? g_root / L"tools" / L"dist" / L"VERSION" : g_root / L"version.txt");
    std::string text;
    std::getline(in, text);
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return (text.empty() ? L"?" : std::wstring(text.begin(), text.end())) + (g_dev_tree ? L" dev" : L"");
}

void OnLine(const std::wstring& line) {
    if (g_task == Task::kPkg) { // Fase 68: pastas extraidas vao para os campos
        if (line.rfind(L"JOGO ", 0) == 0) {
            g_game_dir = line.substr(5);
            LoadArt(line.substr(5));
            if (g_patch_dir.empty()) {
                g_patch_dir = FindPatch(line.substr(5)).wstring();
            }
            return;
        }
        if (line.rfind(L"ATUALIZACAO ", 0) == 0) {
            g_patch_dir = line.substr(12);
            return;
        }
        if (line.rfind(L"DLCS ", 0) == 0) {
            g_dlc_dir = line.substr(5);
            return;
        }
    }
    if (line.rfind(L"VERSAO_NOVA ", 0) == 0) { // Fase 61
        g_new_version = line.substr(12, line.find(L' ', 12) - 12);
        return;
    }
    if (line.rfind(L"NOTA ", 0) == 0) {
        g_notes += L"\u2022 " + line.substr(5) + L"\n";
        return;
    }
    if (line.rfind(L"SEM_ATUALIZACAO", 0) == 0) {
        return;
    }
    if (line.rfind(L"ATUALIZADO ", 0) == 0 || line.rfind(L"arquivos trocados", 0) == 0 || line.rfind(L"files replaced", 0) == 0) {
        AppendLog(line);
        return;
    }
    if (line.rfind(L"PROGRESSO ", 0) == 0) {
        g_pct = std::max(1, _wtoi(line.c_str() + 10));
        const size_t space = line.find(L' ', 10);
        g_step = space == std::wstring::npos ? L"" : line.substr(space + 1);
        if (!g_step.empty()) g_step[0] = towupper(g_step[0]);
        Redraw();
        return;
    }
    if (line.rfind(L"ERRO ", 0) == 0) {
        AppendLog(std::wstring(ui::T(L"ERRO: ", L"ERROR: ")) + line.substr(5));
        g_step = std::wstring(ui::T(L"Erro: ", L"Error: ")) + line.substr(5);
        Redraw();
        MessageBoxW(g_main, line.substr(5).c_str(), L"LEGO Dimensions PC", MB_ICONERROR);
        return;
    }
    if (line.find(L"EXTRAIDO") != std::wstring::npos || line.find(L"COMPILADO") != std::wstring::npos || line.find(L"PLANO") != std::wstring::npos ||
        line.find(L"EXTRACTED") != std::wstring::npos || line.find(L"COMPILED") != std::wstring::npos || line.find(L"PLAN:") == 0 ||
        line.find(L"jogo:") == 0 || line.find(L"game:") == 0 || line.find(L"DLC") != std::wstring::npos || line.find(L"AVISO") == 0 ||
        line.find(L"WARNING") == 0 || line.find(L"eboot") == 0) {
        AppendLog(line);
    }
}

// ---- mods (Fase 64) ----
fs::path ModsDir() { return g_root / L"mods"; }

// Roda um script de tools\setup e espera; a saida fica em build\logs\<log> e as linhas de aviso vao para o log da janela.
int RunSetupScript(const wchar_t* script, const std::wstring& args, const wchar_t* log_name, DWORD timeout_ms) {
    std::error_code ec;
    fs::create_directories(g_root / L"build" / L"logs", ec);
    const fs::path log_path = g_root / L"build" / L"logs" / log_name;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    fs::path python = g_root / L"tools" / L"python" / L"python.exe";
    std::wstring command = (fs::is_regular_file(python, ec) ? Quote(python.wstring()) : L"python") + L" -I " +
                           Quote((g_root / L"tools" / L"setup" / script).wstring()) + args + (ui::Pt() ? L" --lingua pt" : L" --lingua en");
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    DWORD code = 1;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g_root.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, timeout_ms);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    std::ifstream in(log_path);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("AVISO ", 0) == 0 || line.rfind("ERRO ", 0) == 0 || line.rfind("MOD ", 0) == 0 || line.rfind("INSTALADO ", 0) == 0) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0);
            std::wstring text(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), text.data(), n);
            AppendLog(text);
        }
    }
    return static_cast<int>(code);
}

// Antes de abrir o jogo: tools\setup\mods.py prepara (ou confere que ja estao prontos) os mods da pasta mods\,
// da <jogo>-mods do shadPS4 e da <jogo>\Mods. Sem mod nenhum, o manifesto fica vazio e o jogo abre normal.
void PrepareMods() {
    std::wstring args = L" --root " + Quote(g_root.wstring()) + L" --app0 " + Quote(g_game_dir);
    if (const std::wstring patch = g_patch_dir; !patch.empty()) {
        args += L" --patch " + Quote(patch);
    }
    if (const std::wstring dlc = g_dlc_dir; !dlc.empty()) {
        args += L" --addcont " + Quote(dlc);
    }
    // Fase 67: pacotes de mod com nome proprio pelo Anomaly (connorh315) ou pelo carregador antigo do port
    args += ReadIni(L"anomaly", L"1") == L"1" ? L" --carregador anomaly" : L" --carregador port";
    g_step = ui::T(L"Preparando os mods...", L"Preparing the mods...");
    Redraw();
    UpdateWindow(g_main);
    RunSetupScript(L"mods.py", args, L"mods.log", 300000);
    g_step.clear();
    UpdateStatus();
}

std::vector<std::wstring> ListMods() {
    std::vector<std::wstring> mods;
    std::error_code ec;
    for (fs::directory_iterator it(ModsDir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            mods.push_back(it->path().filename().wstring());
        }
    }
    std::sort(mods.begin(), mods.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return mods;
}

// mods\desligados.txt: um nome de pasta por linha (o mods.py deixa essas de fora)
std::vector<std::wstring> ReadDisabledMods() {
    std::vector<std::wstring> off;
    std::ifstream in(ModsDir() / L"desligados.txt",std::ios::binary);
    std::string raw;
    while (std::getline(in, raw)) {
        if(raw.rfind("\xEF\xBB\xBF",0)==0)raw.erase(0,3);
        std::wstring line=Utf8ToWide(raw);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            off.push_back(line);
        }
    }
    return off;
}

void WriteDisabledMods(const std::vector<std::wstring>& off) {
    std::error_code ec;
    fs::create_directories(ModsDir(), ec);
    std::ofstream out(ModsDir() / L"desligados.txt", std::ios::trunc);
    for (const std::wstring& name : off) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
        std::string text(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), text.data(), n, nullptr, nullptr);
        out << text << "\n";
    }
}

// .rar e .7z sao extraidos pelo tar.exe do Windows (libarchive) dentro do mods.py.
std::wstring BrowseModArchive() {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        const COMDLG_FILTERSPEC filter[] = {{L"Mod (.zip, .rar, .7z)", L"*.zip;*.rar;*.7z"}};
        dialog->SetFileTypes(1, filter);
        dialog->SetTitle(ui::T(L"Mod para instalar (.zip, .rar ou .7z)", L"Mod to install (.zip, .rar or .7z)"));
        if (SUCCEEDED(dialog->Show(g_main))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    return result;
}

// ---- Fase 68: extrair PKGs do PS4 (build\pkg_extractor.exe, a extracao do shadPS4 0.7.0) ----
std::vector<std::wstring> BrowsePkgs() {
    std::vector<std::wstring> result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
        const COMDLG_FILTERSPEC filter[] = {{L"PKG (PS4)", L"*.pkg"}};
        dialog->SetFileTypes(1, filter);
        dialog->SetTitle(ui::T(L"PKGs para extrair (jogo, atualização, DLCs; pode escolher vários)",
                               L"PKGs to extract (game, update, DLCs; you can pick several)"));
        if (SUCCEEDED(dialog->Show(g_main))) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dialog->GetResults(&items))) {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD i = 0; i < count; i++) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(items->GetItemAt(i, &item))) {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            result.push_back(path);
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                }
                items->Release();
            }
        }
        dialog->Release();
    }
    return result;
}

void ExtractPkgs() {
    if (g_busy) {
        return;
    }
    std::error_code ec;
    const fs::path exe = g_root / L"build" / L"pkg_extractor.exe";
    if (!fs::is_regular_file(exe, ec)) {
        MessageBoxW(g_main, ui::T(L"Não achei o build\\pkg_extractor.exe na pasta do port.", L"build\\pkg_extractor.exe was not found in the port folder."),
                    L"LEGO Dimensions PC", MB_ICONERROR);
        return;
    }
    const std::vector<std::wstring> pkgs = BrowsePkgs();
    if (pkgs.empty()) {
        return;
    }
    // onde extrair: perto do jogo que ja esta no campo, senao na pasta dos PKGs
    const std::wstring game = g_game_dir;
    const std::wstring start = !game.empty() ? fs::path(game).parent_path().wstring() : fs::path(pkgs[0]).parent_path().wstring();
    const std::wstring destination = BrowseFolder(ui::T(L"Pasta onde extrair (o jogo fica em <pasta>\\CUSA01176)", L"Folder to extract into (uses the package title ID)"), start);
    if (destination.empty()) {
        return;
    }
    std::wstring args = L" --destino " + Quote(destination);
    // DLCs: na pasta de DLCs ja escolhida quando ela e a pasta do jogo dentro do addcont (CUSA01176), senao <pasta>\addcont\CUSA01176
    const std::wstring dlc = g_dlc_dir;
    const bool dlc_here = !dlc.empty() && _wcsnicmp(fs::path(dlc).filename().c_str(), L"CUSA", 4) == 0;
    if (dlc_here) {
        args += L" --dlcs " + Quote(dlc);
    }
    const fs::path dest(destination);
    const std::wstring text =
        std::to_wstring(pkgs.size()) + ui::T(L" PKG(s) serão extraídos:\n\n", L" PKG(s) will be extracted:\n\n") + ui::T(L"Jogo: ", L"Game: ") +
        (dest / L"<TITLE_ID>").wstring() + L"\n" + ui::T(L"Atualização: ", L"Update: ") + (dest / L"<TITLE_ID>-patch").wstring() + L"\nDLCs: " +
        (dlc_here ? dlc : (dest / L"addcont" / L"<TITLE_ID>").wstring()) + L"\n\n" +
        ui::T(L"Arquivos com o mesmo nome são substituídos. Os PKGs não são apagados. Continuar?",
              L"Files with the same name are replaced. The PKGs are not deleted. Continue?");
    if (MessageBoxW(g_main, text.c_str(), L"LEGO Dimensions PC", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    for (const std::wstring& pkg : pkgs) {
        args += L" " + Quote(pkg);
    }
    args += ui::Pt() ? L" --lingua pt" : L" --lingua en";
    if (RunCommand(Quote(exe.wstring()) + args, Task::kPkg, L"ERROR: could not start build\\pkg_extractor.exe.",
                   L"ERROR: could not start build\\pkg_extractor.exe.")) {
        g_step = ui::T(L"Extraindo os PKGs...", L"Extracting the PKGs...");
        UpdateStatus();
    }
}

std::wstring ModField(const fs::path& folder,const wchar_t* key,const std::wstring& fallback=L"") {
    wchar_t value[2048]{};
    GetPrivateProfileStringW(L"mod",key,fallback.c_str(),value,2048,(folder/L"mod.ini").c_str());
    return value;
}

void ModDetails(const fs::path& folder,bool enabled,bool external=false) {
    uint64_t bytes=0;size_t count=0,archives=0,definitions=0,animations=0,textures=0;bool incomplete=false;
    std::error_code ec;
    for(fs::recursive_directory_iterator it(folder,fs::directory_options::skip_permission_denied,ec),end;!ec&&it!=end;it.increment(ec)) {
        const DWORD attributes=GetFileAttributesW(it->path().c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT)) {it.disable_recursion_pending();incomplete=true;continue;}
        if(!it->is_regular_file(ec))continue;
        if(++count>50000){incomplete=true;break;}
        bytes+=it->file_size(ec);
        auto ext=it->path().extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),towlower);
        archives+=ext==L".dat";definitions+=ext==L".cd";animations+=ext==L".an4";textures+=ext==L".tex"||ext==L".dds";
    }
    wchar_t size[64]{};swprintf_s(size,L"%.1f MiB",double(bytes)/(1024*1024));
    std::wstring text=ModField(folder,L"name",folder.filename().wstring());
    if(auto version=ModField(folder,L"version");!version.empty())text+=L"  v"+version;
    text+=L"\n\nStatus: "+std::wstring(enabled?L"Enabled for the next launch":L"Disabled");
    if(auto description=ModField(folder,L"description");!description.empty())text+=L"\n\n"+description;
    if(auto character=ModField(folder,L"character_id");!character.empty())text+=L"\nToypad character ID: "+character;
    text+=L"\n\nFiles: "+std::to_wstring(count)+L"  |  Size: "+size+L"\nDAT packs: "+std::to_wstring(archives)+L"  |  Definitions: "+std::to_wstring(definitions)+L"\nAnimations: "+std::to_wstring(animations)+L"  |  Textures: "+std::to_wstring(textures);
    text+=L"\n\nFolder:\n"+folder.wstring();
    if(external)text+=L"\n\nExternal game-folder mods are managed from their own folder.";
    if(ec||incomplete)text+=L"\n\nFile scan incomplete; linked files were not followed.";
    MessageBoxW(g_main,text.c_str(),L"Installed mod details",MB_OK|MB_ICONINFORMATION);
}

// Each installed mod exposes its state, metadata, files and actual source folder.
void ModsMenu(POINT at) {
    const std::vector<std::wstring> mods = ListMods();
    std::vector<std::wstring> off = ReadDisabledMods();
    const auto is_off = [&](const std::wstring& name) {
        return std::any_of(off.begin(), off.end(), [&](const std::wstring& o) { return _wcsicmp(o.c_str(), name.c_str()) == 0; });
    };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu,MF_STRING|MF_GRAYED,0,(L"Installed mods: "+std::to_wstring(mods.size())).c_str());
    if (mods.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, ui::T(L"Nenhum mod instalado", L"No mods installed"));
    }
    for (size_t i = 0; i < mods.size(); ++i) {
        if(i>=512)break;
        HMENU sub=CreatePopupMenu();
        AppendMenuW(sub,MF_STRING,100+i,is_off(mods[i])?L"Enable for next launch":L"Disable for next launch");
        AppendMenuW(sub,MF_STRING,10000+i,L"Details and file counts...");
        AppendMenuW(sub,MF_STRING,20000+i,L"Open this mod folder");
        const fs::path folder=ModsDir()/mods[i];
        std::wstring label=ModField(folder,L"name",mods[i]);
        if(auto version=ModField(folder,L"version");!version.empty())label+=L"  v"+version;
        label+=is_off(mods[i])?L"  [Disabled]":L"  [Enabled]";
        // Ampersands in names must stay literal in a Windows menu.
        for(size_t p=0;(p=label.find(L'&',p))!=std::wstring::npos;p+=2)label.insert(p,1,L'&');
        AppendMenuW(menu,MF_POPUP|(is_off(mods[i])?0:MF_CHECKED),reinterpret_cast<UINT_PTR>(sub),label.c_str());
    }
    std::error_code ec;
    const fs::path game = g_game_dir;
    fs::path shad = game;
    shad += L"-mods";
    if (!game.empty() && (fs::is_directory(game / L"Mods", ec) || fs::is_directory(shad, ec))) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, ui::T(L"(também os da pasta do jogo: Mods / -mods)", L"(also the ones in the game folder: Mods / -mods)"));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Fase 67: o carregador de mods Anomaly, de connorh315 (github.com/connorh315/anomaly), refeito dentro do port
    const bool anomaly = ReadIni(L"anomaly", L"1") == L"1";
    AppendMenuW(menu, MF_STRING | (anomaly ? MF_CHECKED : 0), 3,
                ui::T(L"Anomaly mod loader (por connorh315)", L"Anomaly mod loader (by connorh315)"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1, ui::T(L"Instalar mod (.zip, .rar, .7z)...", L"Install a character or mod..."));
    AppendMenuW(menu, MF_STRING, 2, ui::T(L"Abrir a pasta de mods", L"Open the mods folder"));
    AppendMenuW(menu,MF_STRING,4,L"View mod loading log");
    const int picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, at.x, at.y, 0, g_main, nullptr);
    DestroyMenu(menu);
    if(picked>=10000&&picked<10000+static_cast<int>(std::min<size_t>(mods.size(),512))) {
        const auto& name=mods[picked-10000];ModDetails(ModsDir()/name,!is_off(name));
    }else if(picked>=20000&&picked<20000+static_cast<int>(std::min<size_t>(mods.size(),512))) {
        ShellExecuteW(g_main,L"open",(ModsDir()/mods[picked-20000]).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    }else if (picked >= 100 && picked < 100 + static_cast<int>(mods.size())) {
        const std::wstring& name = mods[picked - 100];
        if (is_off(name)) {
            std::erase_if(off, [&](const std::wstring& o) { return _wcsicmp(o.c_str(), name.c_str()) == 0; });
            AppendLog(ui::T(L"Mod ligado: ", L"Mod on: ") + name);
        } else {
            off.push_back(name);
            AppendLog(ui::T(L"Mod desligado: ", L"Mod off: ") + name);
        }
        WriteDisabledMods(off);
    } else if (picked == 1) {
        if (const std::wstring archive = BrowseModArchive(); !archive.empty()) {
            RunSetupScript(L"mods.py", L" --instalar " + Quote(archive) + L" --mods " + Quote(ModsDir().wstring()), L"mods_instalar.log", 300000);
        }
    } else if (picked == 2) {
        fs::create_directories(ModsDir(), ec);
        ShellExecuteW(g_main, L"open", ModsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (picked == 3) {
        WriteIni(L"anomaly", anomaly ? L"0" : L"1");
        AppendLog(anomaly ? ui::T(L"Anomaly desligado: os pacotes de mod usam o carregador do port",
                                  L"Anomaly off: mod packs use the port's own loader")
                          : ui::T(L"Anomaly ligado (connorh315): os pacotes de mod entram na frente dos do jogo",
                                  L"Anomaly on (connorh315): mod packs load ahead of the game's"));
    }else if(picked==4){
        const fs::path log=g_root/L"build"/L"logs"/L"mods.log";
        if(fs::is_regular_file(log,ec))ShellExecuteW(g_main,L"open",log.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        else MessageBoxW(g_main,L"No mod loading log yet. The launcher creates it when preparing mods for a game launch.",L"Mods",MB_OK|MB_ICONINFORMATION);
    }
}

// ---- apelidos dos jogadores (Fase 65) ----
// jogadores.txt na pasta do port: um apelido por linha (jogador 1 a 4), em UTF-8; linha vazia = PlayerN.
constexpr int kPlayers = 4;
constexpr UINT kNamesLimit = 16; // o PS4 aceita nomes de ate 16 caracteres

std::wstring Utf8ToWide(const std::string& text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::vector<std::wstring> ReadPlayerNames() {
    std::vector<std::wstring> names(kPlayers);
    std::ifstream in(g_root / L"jogadores.txt", std::ios::binary);
    std::string line;
    for (int i = 0; i < kPlayers && std::getline(in, line); ++i) {
        if (i == 0 && line.rfind("\xEF\xBB\xBF", 0) == 0) {
            line.erase(0, 3); // BOM do Bloco de Notas
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        std::erase(line, '"');
        names[i] = Utf8ToWide(line).substr(0, kNamesLimit);
    }
    return names;
}

void WritePlayerNames(const std::vector<std::wstring>& names) {
    std::ofstream out(g_root / L"jogadores.txt", std::ios::binary | std::ios::trunc);
    for (const std::wstring& name : names) {
        out << WideToUtf8(name) << "\r\n";
    }
}

HWND g_names_window = nullptr;
HWND g_name_edits[kPlayers] = {};
HWND g_names_focus = nullptr;
constexpr int kNamesW = 440, kNamesH = 430;
enum NamesId { kNamesSave = 1, kNamesCancel = 2, kNamesEdit = 300 };

LRESULT CALLBACK NamesProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC target = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        {
            ui::Buffered buffer(target, client);
            HBRUSH bg = CreateSolidBrush(ui::kBg);
            FillRect(buffer.dc, &client, bg);
            DeleteObject(bg);
            Gdiplus::Graphics g(buffer.dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            for (HWND edit : g_name_edits) {
                ui::FieldBox(g, hwnd, edit, g_names_focus == edit);
            }
            g.Flush();
            RECT title{S(28), S(18), S(kNamesW - 28), S(46)};
            ui::Text(buffer.dc, ui::T(L"Apelidos dos jogadores", L"Player nicknames"), title, g_button, ui::kText);
            RECT hint{S(28), S(48), S(kNamesW - 28), S(92)};
            ui::Text(buffer.dc, ui::T(L"O nome que o jogo mostra no lugar de Player1, Player2... (até 16 letras; vazio = padrão)",
                                      L"The name the game shows instead of Player1, Player2... (up to 16 letters; empty = default)"),
                     hint, g_small, ui::kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
            for (int i = 0; i < kPlayers; ++i) {
                RECT label{S(28), S(104.0f + i * 64), S(kNamesW - 28), S(122.0f + i * 64)};
                ui::Text(buffer.dc, std::wstring(ui::T(L"JOGADOR ", L"PLAYER ")) + std::to_wstring(i + 1), label, g_label, ui::kMuted);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM:
        ui::DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, ui::kField);
        SetTextColor(dc, ui::kText);
        return reinterpret_cast<LRESULT>(g_field_brush);
    }
    case WM_COMMAND:
        if (LOWORD(wp) >= kNamesEdit && LOWORD(wp) < kNamesEdit + kPlayers && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) {
            g_names_focus = HIWORD(wp) == EN_SETFOCUS ? reinterpret_cast<HWND>(lp) : nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (LOWORD(wp) == kNamesSave) {
            std::vector<std::wstring> names;
            for (HWND edit : g_name_edits) {
                std::wstring name = GetText(edit);
                std::erase(name, L'"');
                names.push_back(name);
            }
            WritePlayerNames(names);
            std::wstring summary;
            for (int i = 0; i < kPlayers; ++i) {
                summary += (i ? L", " : L"") + (names[i].empty() ? L"Player" + std::to_wstring(i + 1) : names[i]);
            }
            AppendLog(ui::T(L"Apelidos: ", L"Nicknames: ") + summary);
            DestroyWindow(hwnd);
        } else if (LOWORD(wp) == kNamesCancel) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        EnableWindow(g_main, TRUE);
        SetForegroundWindow(g_main);
        g_names_window = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Janelinha com os 4 apelidos; o launcher fica travado enquanto ela esta aberta.
void OpenNamesDialog() {
    if (g_names_window) {
        SetForegroundWindow(g_names_window);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = NamesProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = g_bg_brush;
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"LegoDimensionsPcNames";
        RegisterClassExW(&wc);
        registered = true;
    }
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT size{0, 0, S(kNamesW), S(kNamesH)};
    AdjustWindowRect(&size, style, FALSE);
    RECT owner;
    GetWindowRect(g_main, &owner);
    const int w = size.right - size.left, h = size.bottom - size.top;
    g_names_window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"LegoDimensionsPcNames", ui::T(L"Apelidos", L"Nicknames"), style,
                                     owner.left + ((owner.right - owner.left) - w) / 2, owner.top + ((owner.bottom - owner.top) - h) / 2, w, h, g_main,
                                     nullptr, GetModuleHandleW(nullptr), nullptr);
    ui::DarkTitle(g_names_window);
    const std::vector<std::wstring> names = ReadPlayerNames();
    for (int i = 0; i < kPlayers; ++i) {
        g_name_edits[i] = ui::Edit(g_names_window, S(38), S(132.0f + i * 64), S(kNamesW - 76), S(24), kNamesEdit + i, g_font);
        SendMessageW(g_name_edits[i], EM_LIMITTEXT, kNamesLimit, 0);
        SetWindowTextW(g_name_edits[i], names[i].c_str());
        SendMessageW(g_name_edits[i], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>((L"Player" + std::to_wstring(i + 1)).c_str()));
    }
    ui::Button(g_names_window, ui::T(L"Cancelar", L"Cancel"), ui::Style::kSecondary, S(kNamesW - 316), S(kNamesH - 62), S(130), S(44), kNamesCancel, g_button);
    ui::Button(g_names_window, ui::T(L"Salvar", L"Save"), ui::Style::kPrimary, S(kNamesW - 176), S(kNamesH - 62), S(148), S(44), kNamesSave, g_button);
    EnableWindow(g_main, FALSE);
    ShowWindow(g_names_window, SW_SHOW);
    SetFocus(g_name_edits[0]);
}

void LaunchLegoToypad(bool notify_if_running = false);

// ---- jogar ----
void RunGame() {
    SaveSettings();
    if (!Prepared()) {
        MessageBoxW(g_main, ui::T(L"Primeiro clique em Preparar: o port precisa gerar os arquivos a partir do seu jogo.", L"First click Prepare: the port needs to build its files from your game."), L"LEGO Dimensions PC", MB_ICONINFORMATION);
        return;
    }
    if (!AotReady() && MessageBoxW(g_main,
                                   ui::T(L"Os shaders AOT ainda nao foram preparados: o jogo vai compilar os shaders na hora (mais pop-in e "
                                         L"travamentos na primeira vez). Jogar assim mesmo?",
                                         L"The AOT shaders have not been prepared yet: the game will compile shaders on the fly "
                                         L"(more pop-in and stutter the first time). Play anyway?"),
                                   L"LEGO Dimensions PC", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    std::error_code icons_ec;
    if (!fs::is_regular_file(g_root / L"assets" / L"buttons" / L"ps4_dxt5.hash", icons_ec)) {
        // Fase 62: folhas de icones dos botoes (feitas do dump; o Preparar ja faz, mas quem atualizou pelo launcher nao)
        fs::path python = g_root / L"tools" / L"python" / L"python.exe";
        std::wstring make = (fs::is_regular_file(python, icons_ec) ? Quote(python.wstring()) : L"python") + L" -I " +
                            Quote((g_root / L"tools" / L"setup" / L"botoes.py").wstring()) + L" --app0 " + Quote(g_game_dir);
        if (const std::wstring patch = g_patch_dir; !patch.empty()) {
            make += L" --patch " + Quote(patch);
        }
        STARTUPINFOW make_si{sizeof(make_si)};
        PROCESS_INFORMATION make_pi{};
        g_step = ui::T(L"Montando os \u00edcones dos bot\u00f5es...", L"Building the button icons...");
        Redraw();
        UpdateWindow(g_main);
        if (CreateProcessW(nullptr, make.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_root.c_str(), &make_si, &make_pi)) {
            WaitForSingleObject(make_pi.hProcess, 120000);
            CloseHandle(make_pi.hThread);
            CloseHandle(make_pi.hProcess);
        }
        g_step.clear();
        UpdateStatus();
    }
    if (g_opt_toypad) {
        LaunchLegoToypad();
    }
    PrepareMods(); // Fase 64
    std::wstring command = Quote((g_root / L"build" / L"lego_dimensions.exe").wstring()) + L" --app0 " + Quote(g_game_dir);
    const std::wstring dlc = g_dlc_dir;
    command += dlc.empty() ? L" --no-dlc" : L" --addcont " + Quote(dlc);
    if (const std::wstring patch = g_patch_dir; !patch.empty()) {
        command += L" --patch " + Quote(patch); // Fase 58
    }
    if (g_opt_fps) {
        command += L" --fps60";
    }
    if (g_opt_fullscreen) {
        command += L" --fullscreen"; // Fase 60: tela cheia sem bordas (F11 alterna dentro do jogo)
    }
    command += L" --resolution " + g_res_value; // Fase 61: tamanho da janela
    command += L" --button-icons " + g_icons_value; // Fase 62
    if (g_gpl_value != L"auto") {
        command += g_gpl_value == L"ligado" ? L" --gpl" : L" --no-gpl"; // Fase 74
    }
    AppendLog(ui::Pt() ? (L"Iniciando em " + g_res_value + (g_opt_fullscreen ? L" (Tela Cheia)..." : L" (Janela)..."))
                       : (L"Starting at " + g_res_value + (g_opt_fullscreen ? L" (Fullscreen)..." : L" (Windowed)...")));
    if (g_lang_value != L"auto") {
        command += L" --lang " + g_lang_value; // Fase 61: idioma do jogo (e do port)
    }
    if (g_opt_skip) {
        command += L" --skip-cutscenes"; // Fase 61: qualquer cutscene pode ser pulada, mesmo sem 100%
    }
    if (g_opt_overlay) {
        command += L" --stats-overlay"; // Fase 65: contador de FPS / info
    }
    const std::vector<std::wstring> names = ReadPlayerNames(); // Fase 65: apelidos (vazio = PlayerN)
    for (size_t i = 0; i < names.size(); ++i) {
        if (!names[i].empty()) {
            command += L" --player-name " + std::to_wstring(i + 1) + L" " + Quote(names[i]);
        }
    }
    command += L" --hitch-log";
    if (GetEnvironmentVariableW(L"LD_LAUNCHER_SEM_JOGO", nullptr, 0)) { // teste do launcher: faz tudo menos abrir o jogo
        AppendLog(L"[teste] " + command);
        return;
    }
    const bool console = g_opt_console;
    STARTUPINFOW si{sizeof(si)};
    HANDLE log = INVALID_HANDLE_VALUE;
    if (!console) { // sem console: a saida vai para build\logs\sessao_*.log (ajuda a achar problemas)
        std::error_code ec;
        fs::create_directories(g_root / L"build" / L"logs", ec);
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[64];
        swprintf(name, 64, L"sessao_%04u%02u%02u_%02u%02u%02u.log", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        log = CreateFileW((g_root / L"build" / L"logs" / name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = si.hStdError = log;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }
    }
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE, console ? 0 : CREATE_NO_WINDOW, nullptr, g_root.c_str(),
                       &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        g_step = ui::T(L"Jogo aberto. Bom jogo!", L"Game started. Have fun!");
        Redraw();
        ShowWindow(g_main, SW_MINIMIZE);
    } else {
        AppendLog(ui::T(L"ERRO: nao consegui abrir build\\lego_dimensions.exe", L"ERROR: could not start build\\lego_dimensions.exe"));
    }
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
}


fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        out = path;
        CoTaskMemFree(path);
    }
    return out;
}

bool CreateShortcut(const fs::path& link, const fs::path& target, const fs::path& dir, const wchar_t* desc) {
    IShellLinkW* shell_link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link)))) {
        return false;
    }
    shell_link->SetPath(target.c_str());
    shell_link->SetWorkingDirectory(dir.c_str());
    shell_link->SetDescription(desc);
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(shell_link->QueryInterface(IID_PPV_ARGS(&file)))) {
        ok = SUCCEEDED(file->Save(link.c_str(), TRUE));
        file->Release();
    }
    shell_link->Release();
    return ok;
}

void CreateDesktopShortcut() {
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
    const fs::path launcher(self);
    const fs::path root = launcher.parent_path();
    std::error_code ec;
    const fs::path desktop = KnownFolder(FOLDERID_Desktop);
    const fs::path programs = KnownFolder(FOLDERID_Programs) / L"LEGO Dimensions PC";
    fs::create_directories(programs, ec);
    const bool ok1 = CreateShortcut(desktop / L"LEGO Dimensions PC.lnk", launcher, root, L"LEGO Dimensions PC");
    const bool ok2 = CreateShortcut(programs / L"LEGO Dimensions PC.lnk", launcher, root, L"LEGO Dimensions PC");
    if (ok1 || ok2) {
        MessageBoxW(g_main,
                    ui::T(L"Atalho criado na Área de Trabalho e no Menu Iniciar com sucesso!",
                          L"Shortcut created on Desktop and Start Menu successfully!"),
                    L"LEGO Dimensions PC", MB_ICONINFORMATION);
    } else {
        MessageBoxW(g_main,
                    ui::T(L"Não foi possível criar o atalho.",
                          L"Could not create shortcut."),
                    L"LEGO Dimensions PC", MB_ICONERROR);
    }
}

bool IsProcessRunning(const wchar_t* process_name) {
    bool running = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, process_name) == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return running;
}

fs::path FindLegoToypadExe() {
    std::error_code ec;
    const fs::path candidates[] = {
        g_root.parent_path() / L"tools" / L"LegoToypad" / L"LegoToypad.exe",
        g_root / L"tools" / L"LegoToypad.exe",
        g_root / L"tools" / L"LegoToypad" / L"LegoToypad.exe",
        g_root / L"LegoToypad.exe",
        g_root / L"tools" / L"LegoToypad.v1.9.Web.App.Extended.exe"
    };
    for (const auto& path : candidates) {
        if (fs::is_regular_file(path, ec)) {
            return path;
        }
    }
    for (fs::directory_iterator it(g_root / L"tools", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (name.rfind(L"LegoToypad", 0) == 0 && name.size() >= 4 && name.substr(name.size() - 4) == L".exe") {
                return it->path();
            }
        }
    }
    return {};
}

void LaunchLegoToypad(bool notify_if_running) {
    if (IsProcessRunning(L"LegoToypad.exe") || IsProcessRunning(L"LegoToypad.v1.9.Web.App.Extended.exe")) {
        if (notify_if_running) {
            MessageBoxW(g_main, ui::T(L"O LegoToypad já está em execução.", L"LegoToypad is already running."), L"LEGO Dimensions PC", MB_ICONINFORMATION);
        }
        return;
    }
    const fs::path exe = FindLegoToypadExe();
    if (exe.empty()) {
        if (MessageBoxW(g_main,
                        ui::T(L"O executável do LegoToypad não foi encontrado em tools\\LegoToypad.exe.\n\nDeseja abrir a página do LegoToypad v1.9 para baixar?",
                              L"LegoToypad executable not found in tools\\LegoToypad.exe.\n\nDo you want to open the LegoToypad v1.9 download page?"),
                        L"LEGO Dimensions PC", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            ShellExecuteW(nullptr, L"open", L"https://github.com/harrysof/LegoToypad/releases/tag/v1.9_web_app_extended", nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    // Garante que o atalho padrao do controle venha como SELECT (Back, mascara 32)
    const fs::path ini = exe.parent_path() / L"LegoToypad.ini";
    wchar_t shortcut_type[64]{};
    GetPrivateProfileStringW(L"Shortcut", L"Type", L"", shortcut_type, 64, ini.c_str());
    if (shortcut_type[0] == L'\0' || _wcsicmp(shortcut_type, L"Keyboard") == 0) {
        WritePrivateProfileStringW(L"Shortcut", L"Type", L"Controller", ini.c_str());
        WritePrivateProfileStringW(L"Shortcut", L"ControllerMask", L"32", ini.c_str());
    }

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = Quote(exe.wstring());
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        AppendLog(ui::T(L"LegoToypad v1.9 iniciado em segundo plano.", L"LegoToypad started in background."));
    }
}

void OpenTransfer() {
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
    std::error_code ec;
    fs::path exe = fs::path(self).parent_path() / L"LD_PC_Transfer.exe"; // ao lado do launcher (Fase 66: tambem no build\)
    if (!fs::is_regular_file(exe, ec)) {
        exe = g_root / L"LD_PC_Transfer.exe";
    }
    std::wstring command = Quote(exe.wstring());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!fs::is_regular_file(exe, ec) || !CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, g_root.c_str(), &si, &pi)) {
        MessageBoxW(g_main, ui::T(L"Não achei o LD_PC_Transfer.exe na pasta do port.", L"LD_PC_Transfer.exe was not found in the port folder."), L"LEGO Dimensions PC", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}


// ---- a fonte do LEGO Dimensions (NuFont .ft2 da TT; tools\setup\launcher_art.py a copia do dump) ----
// Big-endian. "TNFN" em +8; a altura da linha (float) 20 bytes antes da 1a tabela; "ROTV" + quantidade + (x, y, largura)
// em float por letra; "ROTV" + quantidade + (caractere, letra) em u16; em 4 + (u32 do inicio) uma DDS DXT5 com as letras
// brancas de contorno preto, cada uma em (x, y, largura, altura da linha) com o contorno dentro.
struct LegoFont {
    struct Glyph {
        float x, y, w;
    };
    std::vector<Glyph> glyphs;
    std::map<wchar_t, int> chars;
    float line = 0;
    int width = 0, height = 0;
    std::vector<uint32_t> argb; // a folha decodificada (ARGB)
};

enum Tint { kTintWhite, kTintYellow, kTintDim, kTintCount };
std::unique_ptr<LegoFont> g_lego;
std::unique_ptr<Gdiplus::Bitmap> g_lego_atlas[kTintCount];

uint32_t Be32(const std::vector<BYTE>& d, size_t at) {
    return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) | d[at + 3];
}
float BeFloat(const std::vector<BYTE>& d, size_t at) {
    const uint32_t bits = Be32(d, at);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
uint32_t Le32(const std::vector<BYTE>& d, size_t at) {
    return d[at] | (uint32_t(d[at + 1]) << 8) | (uint32_t(d[at + 2]) << 16) | (uint32_t(d[at + 3]) << 24);
}

void DecodeDxt5(const BYTE* src, int w, int h, uint32_t* out) {
    for (int by = 0; by < h / 4; ++by) {
        for (int bx = 0; bx < w / 4; ++bx, src += 16) {
            BYTE alpha[8] = {src[0], src[1]};
            if (alpha[0] > alpha[1]) {
                for (int i = 1; i < 7; ++i) alpha[i + 1] = BYTE(((7 - i) * alpha[0] + i * alpha[1]) / 7);
            } else {
                for (int i = 1; i < 5; ++i) alpha[i + 1] = BYTE(((5 - i) * alpha[0] + i * alpha[1]) / 5);
                alpha[6] = 0;
                alpha[7] = 255;
            }
            uint64_t abits = 0;
            for (int i = 0; i < 6; ++i) abits |= uint64_t(src[2 + i]) << (8 * i);
            const uint16_t c0 = src[8] | (src[9] << 8), c1 = src[10] | (src[11] << 8);
            const uint32_t cbits = src[12] | (src[13] << 8) | (src[14] << 16) | (uint32_t(src[15]) << 24);
            int rgb[4][3];
            for (int i = 0; i < 2; ++i) {
                const uint16_t c = i ? c1 : c0;
                rgb[i][0] = ((c >> 11) & 31) * 255 / 31;
                rgb[i][1] = ((c >> 5) & 63) * 255 / 63;
                rgb[i][2] = (c & 31) * 255 / 31;
            }
            for (int k = 0; k < 3; ++k) {
                rgb[2][k] = (2 * rgb[0][k] + rgb[1][k]) / 3;
                rgb[3][k] = (rgb[0][k] + 2 * rgb[1][k]) / 3;
            }
            for (int p = 0; p < 16; ++p) {
                const int* c = rgb[(cbits >> (2 * p)) & 3];
                const uint32_t a = alpha[(abits >> (3 * p)) & 7];
                out[(by * 4 + p / 4) * w + bx * 4 + p % 4] = (a << 24) | (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | uint32_t(c[2]);
            }
        }
    }
}

// false se nao for uma NuFont que o launcher entende (o launcher segue com Segoe UI).
bool ParseLegoFont(const std::vector<BYTE>& d, LegoFont* font) {
    if (d.size() < 0x60 || std::memcmp(d.data() + 8, "TNFN", 4) != 0) {
        return false;
    }
    size_t r1 = 0x10;
    while (r1 + 8 <= d.size() && std::memcmp(d.data() + r1, "ROTV", 4) != 0) ++r1;
    if (r1 + 8 > d.size() || r1 < 0x24) {
        return false;
    }
    const uint32_t n1 = Be32(d, r1 + 4);
    const size_t r2 = r1 + 8 + size_t(n1) * 12;
    if (n1 == 0 || n1 > 65536 || r2 + 8 > d.size() || std::memcmp(d.data() + r2, "ROTV", 4) != 0) {
        return false;
    }
    const uint32_t n2 = Be32(d, r2 + 4);
    const size_t dds = 4 + size_t(Be32(d, 0));
    if (r2 + 8 + size_t(n2) * 4 > d.size() || dds + 128 > d.size() || std::memcmp(d.data() + dds, "DDS ", 4) != 0 ||
        std::memcmp(d.data() + dds + 84, "DXT5", 4) != 0) {
        return false;
    }
    font->line = BeFloat(d, r1 - 0x14);
    font->height = int(Le32(d, dds + 12));
    font->width = int(Le32(d, dds + 16));
    if (!(font->line > 4 && font->line < 1024) || font->width < 4 || font->height < 4 || font->width > 8192 || font->height > 8192 ||
        dds + 128 + size_t(font->width) * font->height > d.size()) {
        return false;
    }
    for (uint32_t i = 0; i < n1; ++i) {
        font->glyphs.push_back({BeFloat(d, r1 + 8 + i * 12), BeFloat(d, r1 + 12 + i * 12), BeFloat(d, r1 + 16 + i * 12)});
    }
    for (uint32_t i = 0; i < n2; ++i) {
        const wchar_t c = wchar_t((d[r2 + 8 + i * 4] << 8) | d[r2 + 9 + i * 4]);
        const int g = (d[r2 + 10 + i * 4] << 8) | d[r2 + 11 + i * 4];
        if (g < int(n1)) font->chars[c] = g;
    }
    font->argb.resize(size_t(font->width) * font->height);
    DecodeDxt5(d.data() + dds + 128, font->width, font->height, font->argb.data());
    return true;
}

void LoadLegoFont() {
    g_lego.reset();
    for (auto& atlas : g_lego_atlas) atlas.reset();
    std::ifstream in(g_root / L"assets" / L"launcher" / L"font.ft2", std::ios::binary);
    const std::vector<BYTE> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto font = std::make_unique<LegoFont>();
    if (!ParseLegoFont(data, font.get())) {
        return;
    }
    // folhas ja tingidas (branca, amarela, cinza): o contorno preto continua preto
    const COLORREF tints[kTintCount] = {RGB(255, 255, 255), ui::kAccent, RGB(150, 152, 160)};
    for (int t = 0; t < kTintCount; ++t) {
        g_lego_atlas[t] = std::make_unique<Gdiplus::Bitmap>(font->width, font->height, PixelFormat32bppARGB);
        Gdiplus::BitmapData bits;
        Gdiplus::Rect all(0, 0, font->width, font->height);
        if (g_lego_atlas[t]->LockBits(&all, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bits) != Gdiplus::Ok) {
            return;
        }
        const uint32_t tr = GetRValue(tints[t]), tg = GetGValue(tints[t]), tb = GetBValue(tints[t]);
        for (int y = 0; y < font->height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<BYTE*>(bits.Scan0) + y * bits.Stride);
            for (int x = 0; x < font->width; ++x) {
                const uint32_t p = font->argb[size_t(y) * font->width + x];
                row[x] = (p & 0xFF000000) | ((((p >> 16) & 255) * tr / 255) << 16) | ((((p >> 8) & 255) * tg / 255) << 8) | ((p & 255) * tb / 255);
            }
        }
        g_lego_atlas[t]->UnlockBits(&bits);
    }
    font->argb.clear();
    font->argb.shrink_to_fit();
    g_lego = std::move(font);
}

// Texto com a fonte do jogo; `size` = altura da linha em unidades da tela. Sem a fonte, Segoe UI em negrito.
float LegoTextWidth(Gdiplus::Graphics& g, const std::wstring& text, float size) {
    if (!g_lego) {
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font font(&family, S(size * 0.56f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::RectF box;
        g.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), Gdiplus::StringFormat::GenericTypographic(), &box);
        return box.Width;
    }
    const float k = S(size) / g_lego->line, overlap = g_lego->line * 0.05f;
    float w = 0;
    for (wchar_t c : text) {
        const auto it = g_lego->chars.find(c);
        w += it == g_lego->chars.end() ? g_lego->line * 0.3f * k : (g_lego->glyphs[it->second].w - overlap) * k;
    }
    return w;
}

void LegoText(Gdiplus::Graphics& g, const std::wstring& text, float x, float y, float size, int tint) {
    if (!g_lego || !g_lego_atlas[tint]) {
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font font(&family, S(size * 0.56f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        const COLORREF colors[kTintCount] = {ui::kText, ui::kAccent, ui::kMuted};
        Gdiplus::SolidBrush brush(ui::C(colors[tint]));
        g.DrawString(text.c_str(), -1, &font, Gdiplus::PointF(x, y + S(size * 0.16f)), Gdiplus::StringFormat::GenericTypographic(), &brush);
        return;
    }
    const float k = S(size) / g_lego->line, overlap = g_lego->line * 0.05f;
    for (wchar_t c : text) {
        const auto it = g_lego->chars.find(c);
        if (it == g_lego->chars.end()) {
            x += g_lego->line * 0.3f * k;
            continue;
        }
        const LegoFont::Glyph& glyph = g_lego->glyphs[it->second];
        g.DrawImage(g_lego_atlas[tint].get(), Gdiplus::RectF(x, y, glyph.w * k, S(size)), glyph.x, glyph.y, glyph.w, g_lego->line, Gdiplus::UnitPixel);
        x += (glyph.w - overlap) * k;
    }
}

// A fonte ainda nao foi copiada do jogo: tools\setup\launcher_art.py em segundo plano (alguns segundos, uma vez).
void EnsureLegoFont() {
    static bool running = false;
    std::error_code ec;
    if (running || g_self_test || g_lego || g_game_dir.empty() || !fs::is_regular_file(fs::path(g_game_dir) / L"eboot.bin", ec)) {
        return;
    }
    if (fs::is_regular_file(g_root / L"assets" / L"launcher" / L"font.ft2", ec)) {
        LoadLegoFont();
        return;
    }
    running = true;
    const fs::path python = g_root / L"tools" / L"python" / L"python.exe";
    std::wstring command = (fs::is_regular_file(python, ec) ? L"\"" + python.wstring() + L"\"" : std::wstring(L"python")) + L" -I \"" +
                           (g_root / L"tools" / L"setup" / L"launcher_art.py").wstring() + L"\" --app0 \"" + g_game_dir + L"\"";
    if (!g_patch_dir.empty()) {
        command += L" --patch \"" + g_patch_dir + L"\"";
    }
    std::thread([command]() mutable {
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                           g_root.c_str(), &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 120000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        PostMessageW(g_main, WM_APP_FONT, 0, 0);
    }).detach();
}

// ---- a tela (desenho do launcher do port de inFAMOUS Second Son, de Castlesea; ver o comeco do arquivo) ----
enum class Screen { kMain, kSettings };
Screen g_screen = Screen::kMain;
enum SettingsTab { kTabFiles, kTabDisplay, kTabGame, kTabAdvanced, kTabCount };
int g_tab = kTabFiles;

enum Hot {
    kHotNone, kHotClose, kHotMinimize, kHotPlay, kHotPrepare, kHotSettings, kHotMods, kHotTools, kHotQuit, kHotBack, kHotTabs,
    kHotTab0 = 20, kHotRow0 = 100, kHotLeft0 = 200, kHotRight0 = 300
};
struct Spot {
    Gdiplus::RectF rect;
    int id;
};
std::vector<Spot> g_spots;
int g_hover = kHotNone;     // debaixo do mouse
int g_focus = kHotPlay;     // teclado / controle
bool g_pad_used = false;    // a ultima tecla veio do controle: as dicas mostram os botoes dele
bool g_animating = false;   // a barra de selecao ainda desliza / JOGAR pulsa: o timer redesenha
Gdiplus::RectF g_bar;       // a barra de selecao (desliza ate o item escolhido)
float g_bar_alpha = 0;
bool g_bar_placed = false;
ULONGLONG g_last_paint = 0;
float g_time = 0;

void Redraw() {
    if (g_main) InvalidateRect(g_main, nullptr, FALSE);
}

// ---- as linhas das Configuracoes ----
// kind: pasta (Enter abre o seletor), escolha (< valor >), acao (Enter abre algo)
enum class RowKind { kFolder, kChoice, kAction };
struct Row {
    int tab;
    RowKind kind;
    std::wstring label, hint, value;
    std::function<void(int)> change; // passo -1 / +1 (escolhas) ou 0 (Enter / clique)
};

std::wstring OnOff(bool on) { return on ? ui::T(L"Ligado", L"On") : ui::T(L"Desligado", L"Off"); }
void OpenNamesDialog();
std::vector<std::wstring> ReadPlayerNames();

std::vector<Row> Rows() {
    std::vector<Row> rows;
    const auto toggle = [](bool& option) {
        return [&option](int) {
            option = !option;
            SaveSettings();
        };
    };
    // Arquivos
    rows.push_back({kTabFiles, RowKind::kFolder, ui::T(L"Pasta do jogo", L"Game folder"),
                    ui::T(L"O dump do PS4 (CUSA01176 ou CUSA00935). Enter: escolher.", L"Your PS4 dump (CUSA01176 or CUSA00935). Enter: choose."),
                    g_game_dir.empty() ? ui::T(L"Escolher...", L"Choose...") : g_game_dir, [](int) { BrowseGame(); }});
    rows.push_back({kTabFiles, RowKind::kFolder, ui::T(L"Pasta da atualização", L"Update folder"),
                    ui::T(L"Só se a 1.24 estiver separada (CUSA01176-patch).", L"Only if the 1.24 update is separate (CUSA01176-patch)."),
                    g_patch_dir.empty() ? ui::T(L"Nenhuma / junto do jogo", L"None / included in the game") : g_patch_dir, [](int) { BrowsePatch(); }});
    const int dlcs = CountDlcs(g_dlc_dir);
    rows.push_back({kTabFiles, RowKind::kFolder, ui::T(L"Pasta das DLCs", L"DLC folder"),
                    dlcs ? std::to_wstring(dlcs) + ui::T(L" DLCs encontradas. Enter: escolher.", L" DLCs found. Enter: choose.")
                         : ui::T(L"Uma subpasta por DLC (formato addcont do shadPS4).", L"One subfolder per DLC (shadPS4 addcont layout)."),
                    g_dlc_dir.empty() ? ui::T(L"Nenhuma", L"None") : g_dlc_dir, [](int) { BrowseDlc(); }});
    // Tela
    rows.push_back({kTabDisplay, RowKind::kChoice, ui::T(L"Resolução", L"Resolution"),
                    ui::T(L"Tamanho da janela; o jogo desenha em 1920x1080.", L"Window size; the game renders at 1920x1080."), g_res_value,
                    [](int step) {
                        g_res_value = Step(kResolutions, g_res_value, step, [](const wchar_t* r) { return std::wstring(r); });
                        SaveSettings();
                    }});
    rows.push_back({kTabDisplay, RowKind::kChoice, ui::T(L"Tela cheia", L"Fullscreen"),
                    ui::T(L"Sem bordas. F11 alterna dentro do jogo.", L"Borderless. F11 switches in game."), OnOff(g_opt_fullscreen), toggle(g_opt_fullscreen)});
    rows.push_back({kTabDisplay, RowKind::kChoice, L"60 FPS", ui::T(L"Desligado: os 30 FPS do PS4.", L"Off: the PS4's 30 FPS."), OnOff(g_opt_fps), toggle(g_opt_fps)});
    rows.push_back({kTabDisplay, RowKind::kChoice, ui::T(L"Bibliotecas de pipeline (GPL)", L"Pipeline libraries (GPL)"),
                    ui::T(L"Automático: ligado na NVIDIA / Intel, desligado na AMD.", L"Automatic: on for NVIDIA / Intel, off for AMD."),
                    ui::T(CurrentGpl().pt, CurrentGpl().en), [](int step) {
                        g_gpl_value = Step(kGplChoices, g_gpl_value, step, [](const IconChoice& c) { return std::wstring(c.code); });
                        SaveSettings();
                    }});
    // Jogo
    rows.push_back({kTabGame, RowKind::kChoice, ui::T(L"Idioma", L"Language"),
                    ui::T(L"Do jogo e do port. Fora pt / en o port fala inglês.", L"For the game and the port."),
                    ui::T(CurrentLanguage().pt, CurrentLanguage().en), [](int step) {
                        g_lang_value = Step(kLanguages, g_lang_value, step, [](const Language& l) { return std::wstring(l.code); });
                        SaveSettings();
                    }});
    rows.push_back({kTabGame, RowKind::kChoice, ui::T(L"Ícones dos botões", L"Button icons"),
                    ui::T(L"Automático: pelo controle ligado.", L"Automatic: from the connected controller."), ui::T(CurrentIcons().pt, CurrentIcons().en),
                    [](int step) {
                        g_icons_value = Step(kIconChoices, g_icons_value, step, [](const IconChoice& c) { return std::wstring(c.code); });
                        SaveSettings();
                    }});
    rows.push_back({kTabGame, RowKind::kChoice, ui::T(L"Pular cutscenes", L"Skip cutscenes"),
                    ui::T(L"Qualquer cutscene pode ser pulada, mesmo sem 100%.", L"Any cutscene can be skipped, even without 100%."), OnOff(g_opt_skip),
                    toggle(g_opt_skip)});
    rows.push_back({kTabGame, RowKind::kChoice, ui::T(L"Abrir o LegoToypad com o jogo", L"Start LegoToypad with the game"),
                    ui::T(L"O portal na tela (tools\\LegoToypad.exe).", L"The on-screen portal (tools\\LegoToypad.exe)."), OnOff(g_opt_toypad), toggle(g_opt_toypad)});
    std::wstring names;
    for (const std::wstring& name : ReadPlayerNames()) {
        if (!name.empty()) names += (names.empty() ? L"" : L", ") + name;
    }
    rows.push_back({kTabGame, RowKind::kAction, ui::T(L"Apelidos dos jogadores", L"Player nicknames"),
                    ui::T(L"O nome no lugar de Player1, Player2... Enter: editar.", L"The name shown instead of Player1, Player2... Enter: edit."),
                    names.empty() ? ui::T(L"Padrão", L"Default") : names, [](int) { OpenNamesDialog(); }});
    // Avancado
    rows.push_back({kTabAdvanced, RowKind::kChoice, ui::T(L"Contador de desempenho (F10)", L"Performance overlay (F10)"),
                    ui::T(L"FPS, tempo de quadro, CPU / GPU e memória no jogo.", L"FPS, frame time, CPU / GPU and memory in game."), OnOff(g_opt_overlay),
                    toggle(g_opt_overlay)});
    rows.push_back({kTabAdvanced, RowKind::kChoice, ui::T(L"Mostrar o console", L"Show console"),
                    ui::T(L"Desligado: o log vai para build\\logs\\sessao_*.log.", L"Off: the log goes to build\\logs\\sessao_*.log."), OnOff(g_opt_console),
                    toggle(g_opt_console)});
    return rows;
}

const wchar_t* TabName(int tab) {
    switch (tab) {
    case kTabFiles: return ui::T(L"ARQUIVOS", L"FILES");
    case kTabDisplay: return ui::T(L"TELA", L"DISPLAY");
    case kTabGame: return ui::T(L"JOGO", L"GAME");
    default: return ui::T(L"AVANÇADO", L"ADVANCED");
    }
}

bool NeedsPrepare() { return !Prepared() || !AotReady(); }
bool Enabled(int id) {
    if (g_busy) return id == kHotQuit || id == kHotClose || id == kHotMinimize;
    if (id == kHotPlay) return GameFolderOk();
    if (id == kHotPrepare) return GameFolderOk();
    return true;
}

// Os itens que o foco percorre, de cima para baixo, na tela atual.
std::vector<int> FocusOrder() {
    std::vector<int> order;
    if (g_screen == Screen::kMain) {
        order.push_back(kHotPlay);
        if (NeedsPrepare()) order.push_back(kHotPrepare);
        order.insert(order.end(), {kHotSettings, kHotMods, kHotTools, kHotQuit});
    } else {
        order.push_back(kHotTabs);
        const std::vector<Row> rows = Rows();
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].tab == g_tab) order.push_back(kHotRow0 + int(i));
        }
        order.push_back(kHotBack);
    }
    return order;
}

void FixFocus() {
    const auto order = FocusOrder();
    if (std::find(order.begin(), order.end(), g_focus) == order.end()) g_focus = order.front();
}

float Approach(float cur, float target, float dt, float speed) { return target + (cur - target) * std::exp(-speed * dt); }

// ---- o fundo: a arte do jogo (sce_sys\pic1.png) a direita, escurecida para a esquerda; refeito so quando muda ----
std::unique_ptr<Gdiplus::Bitmap> g_backdrop;
void ResetBackdrop() { g_backdrop.reset(); }

void BuildBackdrop(int width, int height) {
    g_backdrop = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(g_backdrop.get());
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.Clear(Gdiplus::Color(255, 7, 9, 18));
    const float bar = float(S(kBar));
    if (g_art) {
        const float iw = float(g_art->GetWidth()), ih = float(g_art->GetHeight());
        const float area = height - bar, k = std::max(width / iw, area / ih);
        g.DrawImage(g_art.get(), Gdiplus::RectF(width - iw * k, bar + (area - ih * k) / 2, iw * k, ih * k));
    }
    Gdiplus::LinearGradientBrush shade(Gdiplus::PointF(0, 0), Gdiplus::PointF(width * 0.62f, 0), Gdiplus::Color(250, 6, 8, 16),
                                       Gdiplus::Color(0, 6, 8, 16));
    const Gdiplus::REAL positions[] = {0.0f, 0.45f, 1.0f};
    const Gdiplus::Color colors[] = {Gdiplus::Color(245, 6, 8, 16), Gdiplus::Color(205, 6, 8, 16), Gdiplus::Color(0, 6, 8, 16)};
    shade.SetInterpolationColors(colors, positions, 3);
    shade.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    g.FillRectangle(&shade, Gdiplus::RectF(0, bar, width * 0.62f + 1, float(height)));
    Gdiplus::LinearGradientBrush floor(Gdiplus::PointF(0, height * 0.70f), Gdiplus::PointF(0, float(height)), Gdiplus::Color(0, 0, 0, 0),
                                       Gdiplus::Color(200, 0, 0, 0));
    floor.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    g.FillRectangle(&floor, Gdiplus::RectF(0, height * 0.70f, float(width), height * 0.30f + 1));
    Gdiplus::SolidBrush title(Gdiplus::Color(235, 5, 6, 12));
    g.FillRectangle(&title, Gdiplus::RectF(0, 0, float(width), bar));
}

// Texto comum (Segoe UI) numa caixa; reticencias no fim se nao couber.
void PlainText(Gdiplus::Graphics& g, const std::wstring& text, Gdiplus::RectF box, float size, COLORREF color, bool bold = false,
               Gdiplus::StringAlignment align = Gdiplus::StringAlignmentNear, bool wrap = false, BYTE alpha = 255, bool path = false) {
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, S(size), bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat format;
    format.SetAlignment(align);
    format.SetTrimming(path ? Gdiplus::StringTrimmingEllipsisPath : Gdiplus::StringTrimmingEllipsisCharacter); // caminho: corta no meio
    if (!wrap) format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush brush(ui::C(color, alpha));
    g.DrawString(text.c_str(), -1, &font, box, &format, &brush);
}

Gdiplus::RectF U(float x, float y, float w, float h) { return Gdiplus::RectF(float(S(x)), float(S(y)), float(S(w)), float(S(h))); }

bool BarItem(int id) {
    return id == kHotPrepare || id == kHotSettings || id == kHotMods || id == kHotTools || id == kHotQuit || id == kHotBack || id == kHotTabs ||
           (id >= kHotRow0 && id < kHotLeft0);
}

void Paint(Gdiplus::Graphics& g, int width, int height) {
    const ULONGLONG now = GetTickCount64();
    const float dt = g_last_paint ? std::clamp((now - g_last_paint) / 1000.0f, 0.0f, 0.05f) : 0.0f;
    g_last_paint = now;
    g_time += dt;
    g_animating = false;
    auto ease = [&](float& v, float target, float speed) {
        v = Approach(v, target, dt, speed);
        if (std::fabs(v - target) > 0.002f * ui::g_scale) g_animating = true;
        else v = target;
    };
    if (!g_backdrop || int(g_backdrop->GetWidth()) != width || int(g_backdrop->GetHeight()) != height) {
        BuildBackdrop(width, height);
    }
    g.DrawImage(g_backdrop.get(), 0, 0, width, height);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    FixFocus();
    const std::vector<Spot> previous = g_spots;
    g_spots.clear();

    // barra de titulo: nome, minimizar e fechar
    PlainText(g, L"LEGO DIMENSIONS PC", U(14, 7, 400, 18), 11.5f, ui::kMuted);
    for (int id : {kHotMinimize, kHotClose}) {
        const Gdiplus::RectF box = U(id == kHotClose ? kW - 46 : kW - 92, 0, 46, kBar);
        if (g_hover == id) {
            Gdiplus::SolidBrush hl(id == kHotClose ? Gdiplus::Color(255, 196, 43, 28) : Gdiplus::Color(40, 255, 255, 255));
            g.FillRectangle(&hl, box);
        }
        Gdiplus::Pen pen(ui::C(ui::kText), float(S(1.2f)));
        const float cx = box.X + box.Width / 2, cy = box.Y + box.Height / 2, r = float(S(5));
        if (id == kHotClose) {
            g.DrawLine(&pen, cx - r, cy - r, cx + r, cy + r);
            g.DrawLine(&pen, cx - r, cy + r, cx + r, cy - r);
        } else {
            g.DrawLine(&pen, cx - r, cy, cx + r, cy);
        }
        g_spots.push_back({box, id});
    }

    // a barra de selecao, atras dos itens (no lugar do quadro anterior)
    {
        const int target = g_hover != kHotNone && BarItem(g_hover) ? g_hover : g_focus;
        const Spot* spot = nullptr;
        for (const auto& s : previous) {
            if (s.id == target && BarItem(target)) spot = &s;
        }
        if (spot) {
            if (!g_bar_placed || g_bar_alpha < 0.01f) {
                g_bar = spot->rect;
                g_bar_placed = true;
            }
            ease(g_bar.X, spot->rect.X, 18.0f);
            ease(g_bar.Y, spot->rect.Y, 18.0f);
            ease(g_bar.Width, spot->rect.Width, 18.0f);
            ease(g_bar.Height, spot->rect.Height, 18.0f);
        }
        const float before = g_bar_alpha;
        g_bar_alpha = Approach(g_bar_alpha, spot ? 1.0f : 0.0f, dt, 14.0f);
        if (std::fabs(g_bar_alpha - before) > 0.002f) g_animating = true;
        if (g_bar_alpha > 0.01f) {
            Gdiplus::SolidBrush hl(Gdiplus::Color(BYTE(40 * g_bar_alpha), 255, 255, 255));
            g.FillRectangle(&hl, g_bar);
            Gdiplus::SolidBrush mark(ui::C(ui::kAccent, BYTE(255 * g_bar_alpha)));
            g.FillRectangle(&mark, Gdiplus::RectF(g_bar.X, g_bar.Y, float(S(4)), g_bar.Height));
        }
    }

    const float left = 78;
    const bool settings = g_screen == Screen::kSettings;
    // titulo com a fonte do jogo: LEGO pequeno, DIMENSIONS grande e amarelo (menor nas Configuracoes)
    float y = kBar + (settings ? 22.0f : 44.0f);
    LegoText(g, L"LEGO", float(S(left)), float(S(y)), settings ? 22.0f : 34.0f, kTintWhite);
    y += settings ? 18.0f : 28.0f;
    LegoText(g, L"DIMENSIONS", float(S(left - 3)), float(S(y)), settings ? 40.0f : 66.0f, kTintYellow);
    y += settings ? 44.0f : 70.0f;
    PlainText(g, std::wstring(ui::T(L"PORT PARA PC  ·  ", L"PC PORT  ·  ")) + g_version + L"  BETA", U(left + 2, y, 400, 20), 12.5f, ui::kMuted);

    const auto menu_item = [&](const std::wstring& text, int id, float at) {
        const bool lit = (g_focus == id || g_hover == id) && Enabled(id);
        LegoText(g, text, float(S(left + 18 + (lit ? 4 : 0))), float(S(at + 3)), 30.0f, !Enabled(id) ? kTintDim : lit ? kTintYellow : kTintWhite);
        g_spots.push_back({U(left, at, 300, 40), id});
    };

    if (!settings) {
        // JOGAR: barra amarela com o triangulo; brilha devagar quando escolhido
        const float play_y = kBar + 186;
        const bool lit = (g_focus == kHotPlay || g_hover == kHotPlay) && Enabled(kHotPlay);
        Gdiplus::RectF play = U(left, play_y, 270, 54);
        if (lit) {
            const float pulse = 0.55f + 0.45f * std::sin(g_time * 3.2f);
            for (int i = 3; i >= 1; --i) {
                Gdiplus::SolidBrush glow(Gdiplus::Color(BYTE(24 * pulse * (4 - i)), 255, 205, 60));
                const float e = float(S(4.0f * i));
                g.FillRectangle(&glow, Gdiplus::RectF(play.X - e, play.Y - e, play.Width + 2 * e, play.Height + 2 * e));
            }
            if (GetForegroundWindow() == g_main) g_animating = true;
        }
        Gdiplus::SolidBrush play_brush(Enabled(kHotPlay) ? ui::C(lit ? RGB(255, 216, 70) : ui::kAccent) : Gdiplus::Color(255, 90, 80, 40));
        g.FillRectangle(&play_brush, play);
        const float ty = play.Y + play.Height / 2;
        Gdiplus::SolidBrush dark(Gdiplus::Color(255, 20, 16, 4));
        Gdiplus::PointF tri[3] = {Gdiplus::PointF(play.X + S(24), ty - S(10)), Gdiplus::PointF(play.X + S(24), ty + S(10)),
                                  Gdiplus::PointF(play.X + S(42), ty)};
        g.FillPolygon(&dark, tri, 3);
        LegoText(g, ui::T(L"JOGAR", L"PLAY"), play.X + S(60), ty - S(17), 34.0f, kTintWhite);
        g_spots.push_back({play, kHotPlay});
        float at = play_y + 72;
        if (NeedsPrepare()) {
            menu_item(ui::T(L"PREPARAR", L"PREPARE"), kHotPrepare, at);
            at += 48;
        }
        menu_item(ui::T(L"CONFIGURAÇÕES", L"SETTINGS"), kHotSettings, at);
        menu_item(L"MODS", kHotMods, at + 48);
        menu_item(ui::T(L"FERRAMENTAS", L"TOOLS"), kHotTools, at + 96);
        menu_item(ui::T(L"SAIR", L"QUIT"), kHotQuit, at + 144);

        // painel a direita: situacao, o passo atual, progresso e as ultimas linhas do log
        const Gdiplus::RectF panel = U(560, kBar + 60, 372, 410);
        Gdiplus::SolidBrush panel_bg(Gdiplus::Color(215, 8, 10, 18));
        g.FillRectangle(&panel_bg, panel);
        Gdiplus::Pen panel_pen(ui::C(ui::kAccent, 200), float(S(1)));
        g.DrawRectangle(&panel_pen, panel);
        LegoText(g, ui::T(L"SITUAÇÃO", L"STATUS"), panel.X + S(16), panel.Y + S(10), 22.0f, kTintYellow);
        float sy = kBar + 60 + 44;
        for (const auto& [text, color] : std::vector<std::pair<std::wstring, COLORREF>>{
                 {g_status.game, g_status.game_c}, {g_status.patch, g_status.patch_c}, {g_status.dlcs, g_status.dlcs_c}, {g_status.shaders, g_status.shaders_c}}) {
            Gdiplus::SolidBrush dot(ui::C(color));
            g.FillEllipse(&dot, U(576, sy + 6, 9, 9));
            PlainText(g, text, U(594, sy, 326, 22), 13.5f, ui::kText);
            sy += 26;
        }
        PlainText(g, g_step, U(576, sy + 8, 340, 58), 13.0f, ui::kMuted, false, Gdiplus::StringAlignmentNear, true);
        sy += 70;
        if (g_busy) {
            ui::Progress(g, RECT{S(576), S(sy), S(870), S(sy + 7)}, g_pct);
            PlainText(g, std::to_wstring(g_pct) + L"%", U(876, sy - 7, 44, 20), 12.0f, ui::kMuted, false, Gdiplus::StringAlignmentFar);
            sy += 22;
        }
        Gdiplus::Pen line(Gdiplus::Color(60, 255, 255, 255), float(S(1)));
        g.DrawLine(&line, float(S(576)), float(S(sy)), float(S(916)), float(S(sy)));
        sy += 8;
        const int room = int((kBar + 60 + 410 - 10 - sy) / 17);
        const size_t first = g_log_lines.size() > size_t(std::max(room, 0)) ? g_log_lines.size() - room : 0;
        for (size_t i = first; i < g_log_lines.size(); ++i, sy += 17) {
            PlainText(g, g_log_lines[i], U(576, sy, 340, 17), 11.0f, ui::kMuted);
        }
    } else {
        // abas
        float tx = left;
        const float ty = kBar + 128;
        static float underline_x = 0, underline_w = 0;
        for (int t = 0; t < kTabCount; ++t) {
            const float w = LegoTextWidth(g, TabName(t), 22.0f) / ui::g_scale;
            const bool hot = g_hover == kHotTab0 + t;
            LegoText(g, TabName(t), float(S(tx)), float(S(ty)), 22.0f, t == g_tab ? kTintYellow : hot ? kTintWhite : kTintDim);
            if (t == g_tab) {
                if (underline_w == 0) {
                    underline_x = tx;
                    underline_w = w;
                }
                ease(underline_x, tx, 16.0f);
                ease(underline_w, w, 16.0f);
            }
            g_spots.push_back({U(tx - 6, ty - 4, w + 12, 30), kHotTab0 + t});
            tx += w + 34;
        }
        g_spots.push_back({U(left - 10, ty - 6, tx - left, 34), kHotTabs});
        Gdiplus::SolidBrush underline(ui::C(ui::kAccent));
        g.FillRectangle(&underline, U(underline_x, ty + 27, underline_w, 2.5f));
        PlainText(g, g_pad_used ? L"LB / RB" : ui::T(L"← / →", L"← / →"), U(tx, ty + 4, 120, 20), 12.0f, ui::kMuted);
        // linhas num painel escuro (a arte fica atras): rotulo e dica a esquerda, < valor > a direita
        const Gdiplus::RectF panel = U(60, kBar + 160, 846, 316);
        Gdiplus::SolidBrush panel_bg(Gdiplus::Color(225, 8, 10, 18));
        g.FillRectangle(&panel_bg, panel);
        Gdiplus::Pen panel_pen(ui::C(ui::kAccent, 200), float(S(1)));
        g.DrawRectangle(&panel_pen, panel);
        float ry = kBar + 176;
        const std::vector<Row> rows = Rows();
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& row = rows[i];
            if (row.tab != g_tab) continue;
            const int id = kHotRow0 + int(i);
            const bool lit = g_focus == id || g_hover == id || g_hover == kHotLeft0 + int(i) || g_hover == kHotRight0 + int(i);
            LegoText(g, row.label, float(S(left + 8 + (lit ? 4 : 0))), float(S(ry + 2)), 24.0f, kTintWhite);
            PlainText(g, row.hint, U(left + 10, ry + 28, 470, 18), 11.5f, ui::kMuted);
            g_spots.push_back({U(left - 10, ry - 4, 812, 54), id});
            if (row.kind == RowKind::kChoice) {
                const float vw = LegoTextWidth(g, row.value, 22.0f) / ui::g_scale;
                LegoText(g, row.value, float(S(727 - vw / 2)), float(S(ry + 9)), 22.0f, kTintYellow);
                for (int side = 0; side < 2; ++side) {
                    const Gdiplus::RectF arrow = U(side ? 830 : 594, ry + 6, 30, 34);
                    const bool hot = g_hover == (side ? kHotRight0 : kHotLeft0) + int(i);
                    Gdiplus::SolidBrush brush(ui::C(hot || lit ? ui::kAccent : ui::kMuted));
                    const float cx = arrow.X + arrow.Width / 2, cy = arrow.Y + arrow.Height / 2, r = float(S(6));
                    Gdiplus::PointF tri[3] = {Gdiplus::PointF(cx + (side ? -r / 2 : r / 2), cy - r), Gdiplus::PointF(cx + (side ? -r / 2 : r / 2), cy + r),
                                              Gdiplus::PointF(cx + (side ? r : -r), cy)};
                    g.FillPolygon(&brush, tri, 3);
                    g_spots.push_back({arrow, (side ? kHotRight0 : kHotLeft0) + int(i)});
                }
            } else {
                PlainText(g, row.value, U(560, ry + 13, 300, 22), 13.0f, ui::kAccent, true, Gdiplus::StringAlignmentFar, false, 255,
                          row.kind == RowKind::kFolder);
            }
            ry += 58;
        }
        menu_item(ui::T(L"VOLTAR", L"BACK"), kHotBack, kH - 92);
    }

    // dicas das teclas, embaixo a direita
    const std::wstring hints = g_pad_used ? ui::T(L"A  Escolher      B  Voltar      Start  Jogar", L"A  Select      B  Back      Start  Play")
                                          : ui::T(L"Enter  Escolher      Esc  Voltar      ← →  Valor", L"Enter  Select      Esc  Back      ← →  Value");
    PlainText(g, hints, U(kW - 520, kH - 28, 500, 20), 12.0f, ui::kMuted, false, Gdiplus::StringAlignmentFar);
}

int HitTest(int x, int y) {
    for (auto it = g_spots.rbegin(); it != g_spots.rend(); ++it) {
        if (it->rect.Contains(float(x), float(y))) return it->id;
    }
    return kHotNone;
}

// Menu do Windows logo abaixo de um item da tela; devolve o escolhido ou 0.
POINT BelowSpot(int id) {
    POINT at{S(78), S(300)};
    for (const auto& s : g_spots) {
        if (s.id == id) at = POINT{LONG(s.rect.X + S(18)), LONG(s.rect.GetBottom())};
    }
    ClientToScreen(g_main, &at);
    return at;
}

enum ToolId { kToolToypad = 1, kToolShortcut, kToolPrepare, kToolPkg, kToolTransfer, kToolUpdate };
void ModsMenu(POINT at);
void ExtractPkgs();
void OpenTransfer();
void CheckUpdate();
void RunPrepare();
void RunGame();
void CreateDesktopShortcut();
void LaunchLegoToypad(bool notify_if_running);

void AskPrepare() {
    if (!Prepared() || MessageBoxW(g_main, ui::T(L"Ja esta preparado. Preparar de novo (refaz os shaders AOT)?", L"Already prepared. Prepare again (rebuilds the AOT shaders)?"),
                                   L"LEGO Dimensions PC", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        RunPrepare();
    }
}

void ToolsMenu(POINT at) {
    HMENU menu = CreatePopupMenu();
    for (const auto& [id, pt, en] : std::vector<std::tuple<int, const wchar_t*, const wchar_t*>>{
             {kToolToypad, L"Abrir o LegoToypad", L"Open Toypad"},
             {kToolShortcut, L"Criar atalho na Área de Trabalho", L"Create desktop shortcut"},
             {kToolPrepare, L"Preparar o jogo", L"Prepare game"},
             {kToolPkg, L"Extrair PKG...", L"Extract PKG..."},
             {kToolTransfer, L"Transferir um save...", L"Transfer a save..."},
             {kToolUpdate, L"Procurar atualizações", L"Check for updates"}}) {
        const bool off = (id == kToolPrepare && !GameFolderOk());
        AppendMenuW(menu, MF_STRING | (off ? MF_GRAYED : 0), id, ui::T(pt, en));
    }
    const int picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, at.x, at.y, 0, g_main, nullptr);
    DestroyMenu(menu);
    switch (picked) {
    case kToolToypad: LaunchLegoToypad(true); break;
    case kToolShortcut: CreateDesktopShortcut(); break;
    case kToolPrepare: AskPrepare(); break;
    case kToolPkg: ExtractPkgs(); break;
    case kToolTransfer: OpenTransfer(); break;
    case kToolUpdate: CheckUpdate(); break;
    }
}

void SwitchTab(int step) {
    g_tab = (g_tab + kTabCount + step) % kTabCount;
    Redraw();
}

void LeaveSettings() {
    SaveSettings();
    g_screen = Screen::kMain;
    g_focus = kHotSettings;
    UpdateStatus();
    RestartForLanguage();
}

void Activate(int id) {
    if (!Enabled(id)) {
        if (id == kHotPlay && !g_busy) { // sem o jogo: vai direto escolher a pasta
            g_screen = Screen::kSettings;
            g_tab = kTabFiles;
            g_focus = kHotRow0;
            Redraw();
        }
        return;
    }
    if (id >= kHotTab0 && id < int(kHotTab0) + int(kTabCount)) {
        g_tab = id - kHotTab0;
        g_focus = kHotTabs;
        Redraw();
        return;
    }
    if (id >= kHotRow0 && id < kHotRight0 + 100) {
        const int index = (id - kHotRow0) % 100;
        const std::vector<Row> rows = Rows();
        if (index < int(rows.size())) {
            rows[index].change(id >= kHotRight0 ? 1 : id >= kHotLeft0 ? -1 : 0);
            g_focus = kHotRow0 + index;
        }
        Redraw();
        return;
    }
    switch (id) {
    case kHotClose: case kHotQuit: PostMessageW(g_main, WM_CLOSE, 0, 0); break;
    case kHotMinimize: ShowWindow(g_main, SW_MINIMIZE); break;
    case kHotPlay: RunGame(); break;
    case kHotPrepare: AskPrepare(); break;
    case kHotSettings:
        g_screen = Screen::kSettings;
        g_focus = kHotTabs;
        g_lang_at_start = g_lang_value;
        break;
    case kHotMods: ModsMenu(BelowSpot(kHotMods)); break;
    case kHotTools: ToolsMenu(BelowSpot(kHotTools)); break;
    case kHotBack: LeaveSettings(); break;
    case kHotTabs: SwitchTab(1); break;
    }
    Redraw();
}

// Esquerda / direita: o valor da linha escolhida, ou a aba.
void Cycle(int step) {
    if (g_screen != Screen::kSettings) return;
    if (g_focus == kHotTabs) {
        SwitchTab(step);
    } else if (g_focus >= kHotRow0 && g_focus < kHotLeft0) {
        const int index = g_focus - kHotRow0;
        const std::vector<Row> rows = Rows();
        if (index < int(rows.size()) && rows[index].kind == RowKind::kChoice) {
            rows[index].change(step);
        }
    }
    Redraw();
}

void Navigate(int step) {
    const auto order = FocusOrder();
    auto it = std::find(order.begin(), order.end(), g_focus);
    int i = it == order.end() ? 0 : int(it - order.begin());
    i = (i + int(order.size()) + step) % int(order.size());
    g_focus = order[i];
    g_hover = kHotNone;
    Redraw();
}

void Back() {
    if (g_screen == Screen::kSettings) LeaveSettings();
    Redraw();
}

constexpr UINT_PTR kPadTimer = 3;
enum PadDir { kPadNone = -1, kPadUp, kPadDown, kPadLeft, kPadRight };
WORD g_pad_buttons = 0;
int g_pad_dir = kPadNone;
ULONGLONG g_pad_repeat_at = 0;
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinput_get_state = nullptr;

void SonyPadThread();

void LoadXInput() {
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        if (HMODULE module = LoadLibraryW(dll)) {
            g_xinput_get_state = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
            if (g_xinput_get_state) {
                break;
            }
        }
    }
    std::thread(SonyPadThread).detach();
}

// DualShock 4 / DualSense: o relatorio de entrada lido numa thread, convertido para os bits do XInput.
std::atomic<WORD> g_sony_buttons{0};
std::atomic<SHORT> g_sony_lx{0}, g_sony_ly{0};

HANDLE OpenSonyPad(bool* dualsense, DWORD* report_bytes) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO devices = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(devices, nullptr, &hid_guid, i, &iface); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &size, nullptr);
        std::vector<BYTE> storage(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, size, nullptr, nullptr)) {
            continue;
        }
        HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            continue;
        }
        HIDD_ATTRIBUTES attributes{sizeof(attributes)};
        const USHORT pid = HidD_GetAttributes(device, &attributes) && attributes.VendorID == 0x054C ? attributes.ProductID : 0;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps{};
        const bool gamepad = pid && HidD_GetPreparsedData(device, &preparsed) && HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                             caps.UsagePage == 0x01 && caps.Usage == 0x05;
        if (preparsed) {
            HidD_FreePreparsedData(preparsed);
        }
        // DualShock 4 (1a e 2a versao, adaptador sem fio), DualSense e DualSense Edge
        if (gamepad && (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0 || pid == 0x0CE6 || pid == 0x0DF2)) {
            *dualsense = pid == 0x0CE6 || pid == 0x0DF2;
            *report_bytes = caps.InputReportByteLength;
            found = device;
        } else {
            CloseHandle(device);
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
}

// USB: 0x01 (DualSense: botoes no byte 8; DualShock 4 e o relatorio simples do Bluetooth: byte 5).
// Bluetooth completo: 0x11 do DualShock 4 (+2 bytes) e 0x31 do DualSense (+1 byte).
void ParseSonyReport(const BYTE* r, DWORD n, bool dualsense) {
    size_t stick = 0, buttons = 0;
    if (r[0] == 0x01 && n >= 10) {
        stick = 1;
        buttons = dualsense && n >= 64 ? 8 : 5;
    } else if (r[0] == 0x11 && !dualsense && n >= 10) {
        stick = 3;
        buttons = 7;
    } else if (r[0] == 0x31 && dualsense && n >= 12) {
        stick = 2;
        buttons = 9;
    } else {
        return;
    }
    static const WORD kHat[8] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT,
                                 XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN,
                                 XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT,
                                 XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT};
    const BYTE face = r[buttons];
    WORD out = (face & 0x0F) < 8 ? kHat[face & 0x0F] : 0;
    if (face & 0x20) out |= XINPUT_GAMEPAD_A;     // cruz
    if (face & 0x40) out |= XINPUT_GAMEPAD_B;     // circulo
    if (r[buttons + 1] & 0x20) out |= XINPUT_GAMEPAD_START; // options
    if (r[buttons + 1] & 0x01) out |= XINPUT_GAMEPAD_LEFT_SHOULDER;  // L1: aba anterior nas Configuracoes
    if (r[buttons + 1] & 0x02) out |= XINPUT_GAMEPAD_RIGHT_SHOULDER; // R1: proxima aba
    g_sony_buttons = out;
    g_sony_lx = static_cast<SHORT>((r[stick] - 128) * 256);
    g_sony_ly = static_cast<SHORT>((128 - r[stick + 1]) * 256); // no HID o Y cresce para baixo
}

void SonyPadThread() {
    for (;;) {
        bool dualsense = false;
        DWORD bytes = 0;
        HANDLE device = OpenSonyPad(&dualsense, &bytes);
        if (device == INVALID_HANDLE_VALUE) {
            Sleep(2000);
            continue;
        }
        std::vector<BYTE> report(std::max<DWORD>(bytes, 64));
        DWORD got = 0;
        while (ReadFile(device, report.data(), static_cast<DWORD>(report.size()), &got, nullptr)) {
            ParseSonyReport(report.data(), got, dualsense);
        }
        CloseHandle(device); // desconectou
        g_sony_buttons = 0;
        g_sony_lx = g_sony_ly = 0;
    }
}

void PadKey(WORD vk) {
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = vk;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, input, sizeof(INPUT));
}


void LoadSettings() {
    std::wstring game = ReadIni(L"jogo");
    std::wstring dlc = ReadIni(L"dlcs", L"\x01");
    if (dlc == L"\x01") { // primeira vez: a pasta de DLCs do shadPS4, se existir
        dlc.clear();
        wchar_t appdata[MAX_PATH];
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
            dlc = game_region::Addcont(game, appdata).wstring();
        }
    }
    g_game_dir = CleanPath(game);
    std::wstring patch = ReadIni(L"atualizacao");
    if (patch.empty()) {
        patch = FindPatch(game).wstring(); // Fase 58
    }
    g_patch_dir = CleanPath(patch);
    if (dlc.empty() || CountDlcs(dlc) == 0) {
        const fs::path auto_dlc = FindDlc(game);
        if (!auto_dlc.empty()) {
            dlc = auto_dlc.wstring();
        }
    }
    g_dlc_dir = CleanPath(dlc);
    g_opt_fps = ReadIni(L"fps60", L"1") == L"1";
    g_opt_console = ReadIni(L"console", L"0") == L"1";
    g_opt_fullscreen = ReadIni(L"telacheia", L"0") == L"1";
    g_opt_skip = ReadIni(L"pularcenas", L"0") == L"1";
    g_opt_overlay = ReadIni(L"contador", L"0") == L"1"; // Fase 65
    g_opt_toypad = ReadIni(L"toypad_auto", L"0") == L"1";
    g_icons_value = ReadIni(L"icones", L"auto"); // Fase 62
    g_icons_value = CurrentIcons().code;
    g_gpl_value = ReadIni(L"gpl", L"auto"); // Fase 74
    g_gpl_value = CurrentGpl().code;
    g_res_value = ReadIni(L"resolucao", L"1920x1080");
    if (std::find(std::begin(kResolutions), std::end(kResolutions), g_res_value) == std::end(kResolutions)) {
        g_res_value = L"1920x1080";
    }
    g_lang_value = ReadIni(L"idioma", L"en");
    g_lang_value = CurrentLanguage().code;
    LoadArt(game);
    AppendLog(ui::T(L"Bem-vindo! Escolha as pastas em Configura\u00e7\u00f5es, Prepare na primeira vez e depois JOGAR.",
                    L"Welcome! Choose the folders in Settings, Prepare the first time and then PLAY."));
    UpdateStatus();
}

void PollPad() {
    WORD buttons = g_sony_buttons;
    SHORT lx = g_sony_lx, ly = g_sony_ly;
    for (DWORD user = 0; g_xinput_get_state && user < 4; ++user) {
        XINPUT_STATE state{};
        if (g_xinput_get_state(user, &state) == ERROR_SUCCESS) {
            buttons |= state.Gamepad.wButtons;
            if (std::abs(lx) < 8000 && std::abs(ly) < 8000) {
                lx = state.Gamepad.sThumbLX;
                ly = state.Gamepad.sThumbLY;
            }
        }
    }
    DWORD pid = 0;
    HWND front = GetForegroundWindow();
    GetWindowThreadProcessId(front, &pid);
    const WORD pressed = buttons & ~g_pad_buttons;
    g_pad_buttons = buttons;
    int dir = kPadNone;
    constexpr SHORT kStick = 16000;
    if ((buttons & XINPUT_GAMEPAD_DPAD_UP) || ly > kStick) dir = kPadUp;
    else if ((buttons & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -kStick) dir = kPadDown;
    else if ((buttons & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -kStick) dir = kPadLeft;
    else if ((buttons & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > kStick) dir = kPadRight;
    const ULONGLONG now = GetTickCount64();
    bool step = false;
    if (dir != g_pad_dir) {
        g_pad_dir = dir;
        step = dir != kPadNone;
        g_pad_repeat_at = now + 400;
    } else if (dir != kPadNone && now >= g_pad_repeat_at) {
        step = true;
        g_pad_repeat_at = now + 130;
    }
    if (pid != GetCurrentProcessId() || (!step && !pressed)) {
        return; // o launcher nao esta na frente (o jogo esta): o controle e dele
    }
    GUITHREADINFO gui{sizeof(gui)};
    GetGUIThreadInfo(GetCurrentThreadId(), &gui);
    if ((gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE)) || front != g_main) { // menu ou caixa de mensagem
        static const WORD kDirKeys[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
        if (step) PadKey(kDirKeys[dir]);
        if (pressed & XINPUT_GAMEPAD_A) PadKey(VK_RETURN);
        if (pressed & XINPUT_GAMEPAD_B) PadKey(VK_ESCAPE);
        return;
    }
    if (!g_pad_used) {
        g_pad_used = true;
        Redraw();
    }
    if (step) {
        if (dir == kPadUp || dir == kPadDown) Navigate(dir == kPadUp ? -1 : 1);
        else Cycle(dir == kPadLeft ? -1 : 1);
    }
    if (pressed & XINPUT_GAMEPAD_A) Activate(g_focus);
    if (pressed & XINPUT_GAMEPAD_B) Back();
    if ((pressed & XINPUT_GAMEPAD_START) && g_screen == Screen::kMain) Activate(kHotPlay);
    if (g_screen == Screen::kSettings && (pressed & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))) {
        SwitchTab(pressed & XINPUT_GAMEPAD_LEFT_SHOULDER ? -1 : 1);
    }
}

// Hidden, asset-free layout regression. Never loads/saves user settings, starts tools or a game, displays a window,
// or touches the installed game folder.
int SelfTest(const fs::path& report) {
    std::vector<std::string> failures;
    auto check=[&](bool ok,const std::string& message){if(!ok)failures.push_back(message);};
    check(ui::LogText(L"INSTALADO Example")==L"INSTALLED Example","mod protocol display translation");
    check(ui::LogText(L"C:\\Mods\\ERRO folder")==L"C:\\Mods\\ERRO folder","path changed by log translation");
    check(Utf8ToWide("Example \xC3\xA9")==L"Example \u00e9","UTF-8 disabled-mod name round trip");
    // a fonte do jogo: uma NuFont pequena montada aqui (2 letras, folha DXT5 de 8x4 com um bloco branco e um vazio)
    {
        std::vector<BYTE> blob(0x40, 0);
        auto be32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) blob.push_back(BYTE(v >> s)); };
        auto bef = [&](float f) { uint32_t v; std::memcpy(&v, &f, 4); be32(v); };
        std::memcpy(blob.data() + 8, "TNFN", 4);
        blob.resize(0x36);
        blob.insert(blob.end(), {'R', 'O', 'T', 'V'});
        be32(2);
        bef(0); bef(0); bef(4);
        bef(4); bef(0); bef(3);
        blob.insert(blob.end(), {'R', 'O', 'T', 'V'});
        be32(2);
        blob.insert(blob.end(), {0, 'A', 0, 0, 0, 'B', 0, 1});
        const uint32_t line_bits = 0x41000000; // 8.0f
        for (int i = 0; i < 4; ++i) blob[0x36 - 0x14 + i] = BYTE(line_bits >> (24 - 8 * i));
        const size_t dds = blob.size();
        for (int i = 0; i < 4; ++i) blob[i] = BYTE((dds - 4) >> (24 - 8 * i));
        std::vector<BYTE> header(128, 0);
        std::memcpy(header.data(), "DDS ", 4);
        header[12] = 4;  // altura
        header[16] = 8;  // largura
        std::memcpy(header.data() + 84, "DXT5", 4);
        blob.insert(blob.end(), header.begin(), header.end());
        const BYTE white[16] = {255, 255, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
        const BYTE clear[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        blob.insert(blob.end(), std::begin(white), std::end(white));
        blob.insert(blob.end(), std::begin(clear), std::end(clear));
        LegoFont font;
        const bool parsed = ParseLegoFont(blob, &font);
        check(parsed, "game font parse");
        if (parsed) {
            check(font.glyphs.size() == 2 && font.chars.count(L'B') && font.chars[L'B'] == 1 && font.glyphs[1].x == 4 && font.glyphs[1].w == 3,
                  "game font glyph table");
            check(font.line == 8.0f && font.width == 8 && font.height == 4, "game font sheet size");
            check(font.argb[0] == 0xFFFFFFFF && (font.argb[4] >> 24) == 0, "game font DXT5 decode");
        }
        blob[9] = 'X';
        check(!ParseLegoFont(blob, &font), "game font accepts a foreign file");
    }
    // cada tela: tudo dentro da janela, itens escolhiveis sem sobreposicao, e o foco passa por todos
    const int width = S(kW), height = S(kH);
    Gdiplus::Bitmap canvas(width, height, PixelFormat32bppPARGB);
    auto layout = [&](Screen screen, int tab, bool busy, const char* name) {
        g_screen = screen;
        g_tab = tab;
        g_busy = busy;
        g_focus = FocusOrder().front();
        {
            Gdiplus::Graphics g(&canvas);
            Paint(g, width, height);
        }
        const Gdiplus::RectF client(0, 0, float(width), float(height));
        for (const auto& s : g_spots) {
            check(client.Contains(s.rect.X, s.rect.Y) && s.rect.GetRight() <= width + 0.5f && s.rect.GetBottom() <= height + 0.5f,
                  std::string("spot outside window: ") + name + " " + std::to_string(s.id));
        }
        const auto order = FocusOrder();
        for (int id : order) {
            const auto it = std::find_if(g_spots.begin(), g_spots.end(), [&](const Spot& s) { return s.id == id; });
            check(it != g_spots.end(), std::string("focus item not drawn: ") + name + " " + std::to_string(id));
            if (it == g_spots.end() || id == kHotTabs) continue;
            for (int other : order) {
                if (other == id || other == kHotTabs) continue;
                const auto jt = std::find_if(g_spots.begin(), g_spots.end(), [&](const Spot& s) { return s.id == other; });
                Gdiplus::RectF overlap;
                if (jt != g_spots.end() && Gdiplus::RectF::Intersect(overlap, it->rect, jt->rect) && overlap.Width > 1 && overlap.Height > 1) {
                    check(false, std::string("overlapping items: ") + name + " " + std::to_string(id) + " " + std::to_string(other));
                }
            }
        }
        std::vector<int> seen{g_focus};
        for (size_t i = 1; i < order.size(); ++i) {
            Navigate(1);
            seen.push_back(g_focus);
        }
        std::sort(seen.begin(), seen.end());
        check(std::unique(seen.begin(), seen.end()) - seen.begin() == std::ptrdiff_t(order.size()), std::string("focus does not visit every item: ") + name);
    };
    layout(Screen::kMain, 0, false, "main");
    layout(Screen::kMain, 0, true, "main busy");
    for (int tab = 0; tab < kTabCount; ++tab) layout(Screen::kSettings, tab, false, ("settings " + std::to_string(tab)).c_str());
    g_busy = true;
    check(!Enabled(kHotPlay) && !Enabled(kHotSettings) && !Enabled(kHotMods) && !Enabled(kHotTools) && Enabled(kHotQuit), "busy items enabled");
    g_busy = false;
    std::ofstream out(report,std::ios::trunc);
    if(!out)return 2;
    out<<"{\"passed\":"<<(failures.empty()?"true":"false")<<",\"pages\":"<<(2+kTabCount)<<",\"scale\":"<<ui::g_scale<<",\"failures\":[";
    for(size_t i=0;i<failures.size();++i)out<<(i?",":"")<<'"'<<failures[i]<<'"';
    out<<"]}\n";out.close();
    return failures.empty()?0:1;
}

// --screenshot <pasta>: desenha cada tela num PNG, sem mostrar a janela (para conferir o visual e para o README).
int Screenshots(const fs::path& dir) {
    LoadSettings();
    LoadLegoFont();
    UINT count = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    std::vector<BYTE> buffer(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(count, size, codecs);
    const CLSID* png = nullptr;
    for (UINT i = 0; i < count; ++i) {
        if (std::wstring(codecs[i].MimeType) == L"image/png") png = &codecs[i].Clsid;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    const int width = S(kW), height = S(kH);
    Gdiplus::Bitmap canvas(width, height, PixelFormat32bppARGB);
    int saved = 0;
    for (int page = -1; page < kTabCount; ++page) {
        g_screen = page < 0 ? Screen::kMain : Screen::kSettings;
        g_tab = std::max(page, 0);
        g_focus = page < 0 ? kHotPlay : kHotRow0;
        FixFocus();
        g_bar_placed = false;
        for (int pass = 0; pass < 2; ++pass) { // a 2a passada ja tem a barra de selecao no lugar
            g_bar_alpha = 1;
            Gdiplus::Graphics g(&canvas);
            Paint(g, width, height);
            g_last_paint = 0;
        }
        const fs::path file = dir / (page < 0 ? std::wstring(L"main.png") : L"settings_" + std::to_wstring(page) + L".png");
        saved += png && canvas.Save(file.c_str(), png, nullptr) == Gdiplus::Ok;
    }
    return saved == 1 + kTabCount ? 0 : 1;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        static std::unique_ptr<Gdiplus::Bitmap> back;
        if (!back || int(back->GetWidth()) != client.right || int(back->GetHeight()) != client.bottom) {
            back = std::make_unique<Gdiplus::Bitmap>(std::max<LONG>(client.right, 1), std::max<LONG>(client.bottom, 1), PixelFormat32bppPARGB);
        }
        {
            Gdiplus::Graphics g(back.get());
            Paint(g, client.right, client.bottom);
        }
        Gdiplus::Graphics screen(dc);
        screen.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        screen.DrawImage(back.get(), 0, 0);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        const int hit = HitTest(p.x, p.y);
        if (p.y < S(kBar) && hit != kHotClose && hit != kHotMinimize) return HTCAPTION; // arrastar pela barra de titulo
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        const int hit = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hit != g_hover) {
            g_hover = hit;
            Redraw();
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        SetCursor(LoadCursorW(nullptr, hit != kHotNone && hit != kHotTabs ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = kHotNone;
        Redraw();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE; // o WM_MOUSEMOVE escolhe a seta / a mao
        break;
    case WM_LBUTTONDOWN: {
        const int hit = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        g_pad_used = false;
        if (hit != kHotNone && hit != kHotClose && hit != kHotMinimize && !(hit >= kHotLeft0 && hit < kHotRight0 + 100)) {
            if (std::find(FocusOrder().begin(), FocusOrder().end(), hit) != FocusOrder().end()) g_focus = hit;
        }
        if (hit != kHotNone) Activate(hit);
        return 0;
    }
    case WM_KEYDOWN:
        g_pad_used = false;
        switch (wp) {
        case VK_UP: Navigate(-1); break;
        case VK_DOWN: Navigate(1); break;
        case VK_TAB: Navigate(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); break;
        case VK_LEFT: Cycle(-1); break;
        case VK_RIGHT: Cycle(1); break;
        case VK_PRIOR: if (g_screen == Screen::kSettings) SwitchTab(-1); break;
        case VK_NEXT: if (g_screen == Screen::kSettings) SwitchTab(1); break;
        case VK_RETURN: case VK_SPACE: Activate(g_focus); break;
        case VK_ESCAPE: case VK_BACK: Back(); break;
        }
        return 0;
    case WM_ACTIVATE:
        Redraw();
        break;
    case WM_APP_LINE: {
        std::wstring* line = reinterpret_cast<std::wstring*>(lp);
        OnLine(*line);
        delete line;
        return 0;
    }
    case WM_APP_FONT:
        LoadLegoFont();
        Redraw();
        return 0;
    case WM_APP_DONE:
        g_busy = false;
        if (g_task == Task::kCheck) { // Fase 61
            g_pct = 0;
            g_step.clear();
            UpdateStatus();
            if (wp == 2) {
                MessageBoxW(hwnd, (ui::T(L"Voc\u00ea j\u00e1 tem a vers\u00e3o mais nova (", L"You already have the latest version (") + g_version + L").").c_str(),
                            L"LEGO Dimensions PC", MB_ICONINFORMATION);
            } else if (wp == 0 && !g_new_version.empty()) {
                std::wstring text = ui::T(L"A vers\u00e3o ", L"Version ") + g_new_version + ui::T(L" est\u00e1 dispon\u00edvel (voc\u00ea tem a ", L" is available (you have ") +
                                    g_version + L").\n\n" + g_notes + (g_notes.empty() ? L"" : L"\n") +
                                    ui::T(L"Baixar e instalar agora? Seus saves, os arquivos preparados e as op\u00e7\u00f5es ficam como est\u00e3o; o launcher reabre no fim.",
                                          L"Download and install it now? Your saves, the prepared files and the options stay as they are; the launcher reopens at the end.");
                if (MessageBoxW(hwnd, text.c_str(), L"LEGO Dimensions PC", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    ApplyUpdate();
                }
            }
            return 0;
        }
        if (g_task == Task::kUpdate) {
            UpdateStatus();
            if (wp == 0) {
                MessageBoxW(hwnd, ui::T(L"Atualizado! O launcher vai reabrir.", L"Updated! The launcher will reopen."), L"LEGO Dimensions PC", MB_ICONINFORMATION);
                RestartLauncher();
            } else {
                AppendLog(ui::T(L"A atualiza\u00e7\u00e3o falhou (veja acima). Nada do que j\u00e1 estava funcionando foi apagado.",
                                L"The update failed (see above). Nothing that was already working was removed."));
            }
            return 0;
        }
        if (g_task == Task::kPkg) { // Fase 68
            SaveSettings();
            g_pct = wp == 0 ? 100 : 0;
            g_step.clear();
            UpdateStatus();
            if (wp == 0) {
                AppendLog(ui::T(L"PKGs extraídos.", L"PKGs extracted."));
                MessageBoxW(hwnd, (std::wstring(ui::T(L"Extração concluída! As pastas já estão nas Configurações do launcher.",
                                                      L"Extraction finished! The folders are already in the launcher Settings.")) +
                                   (Prepared() ? L"" : ui::T(L"\n\nAgora escolha Preparar.", L"\n\nNow choose Prepare.")))
                                      .c_str(),
                            L"LEGO Dimensions PC", MB_ICONINFORMATION);
            } else {
                AppendLog(ui::T(L"A extração dos PKGs terminou com erro (veja acima).", L"The PKG extraction failed (see above)."));
            }
            return 0;
        }
        if (wp == 0) {
            g_pct = 100;
            g_step = ui::T(L"Pronto! Escolha JOGAR.", L"Done! Choose PLAY.");
            AppendLog(ui::T(L"Preparacao concluida.", L"Preparation finished."));
        } else {
            AppendLog(ui::T(L"A preparacao terminou com erro (veja acima).", L"The preparation failed (see above)."));
        }
        UpdateStatus();
        return 0;
    case WM_TIMER:
        if (wp == kPadTimer) {
            if (g_animating && !IsIconic(hwnd)) Redraw();
            PollPad();
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_busy && MessageBoxW(hwnd, ui::T(L"A preparacao ainda esta rodando. Fechar mesmo assim?", L"The preparation is still running. Close anyway?"), L"LEGO Dimensions PC", MB_YESNO) != IDYES) {
            return 0;
        }
        SaveSettings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    fs::path test_report;
    if(argv&&argc==3&&std::wstring(argv[1])==L"--self-test") {g_self_test=true;test_report=argv[2];}
    fs::path screenshots;
    if(argv&&argc==3&&std::wstring(argv[1])==L"--screenshot") screenshots=argv[2];
    if(argv)LocalFree(argv);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ui::InitProcess();
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    g_root = fs::path(exe).parent_path();
    {
        // Fase 66: o launcher tambem pode ficar dentro do build\ (ao lado do lego_dimensions.exe): a raiz e a pasta de cima
        std::error_code ec;
        if (fs::is_regular_file(g_root / L"lego_dimensions.exe", ec) && fs::is_directory(g_root.parent_path() / L"assets", ec)) {
            g_root = g_root.parent_path();
        }
        g_dev_tree = fs::is_regular_file(g_root / L"CMakeLists.txt", ec) && fs::is_directory(g_root / L"src", ec);
    }
    // Fase 61: idioma escolhido no launcher (launcher.ini); "auto" segue LD_LANG / Windows
    if (const std::wstring choice = g_self_test?L"en":ReadIni(L"idioma", L"en"); choice != L"auto" && !choice.empty()) {
        SetEnvironmentVariableW(L"LD_LANG", choice == L"pt" ? L"pt" : L"en");
    }
    SetEnvironmentVariableW(L"LD_LANG", ui::Pt() ? L"pt" : L"en"); // Fase 60
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8"); // Fase 60: o log da preparacao chega em UTF-8 (nomes de DLC)
    const DWORD style = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const RECT place = ui::FitWindow(kW, kH, style); // Fase 61: cabe em telas pequenas
    g_font = ui::Font(15);
    g_small = ui::Font(13);
    g_label = ui::Font(11.5f, FW_SEMIBOLD);
    g_button = ui::Font(15, FW_SEMIBOLD);
    g_field_brush = CreateSolidBrush(ui::kField);
    g_bg_brush = CreateSolidBrush(ui::kBg);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bg_brush;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, S(16), S(16), 0));
    wc.lpszClassName = L"LegoDimensionsPcLauncher";
    RegisterClassExW(&wc);
    g_main = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"LEGO Dimensions PC", style, place.left, place.top, place.right, place.bottom,
                             nullptr, nullptr, instance, nullptr);
    const DWORD round = 2; // DWMWCP_ROUND: cantos arredondados no Windows 11
    DwmSetWindowAttribute(g_main, 33, &round, sizeof(round));
    g_version = ReadVersion();
    if(g_self_test) {
        const int result=SelfTest(test_report);DestroyWindow(g_main);CoUninitialize();return result;
    }
    if(!screenshots.empty()) {
        const int result=Screenshots(screenshots);DestroyWindow(g_main);CoUninitialize();return result;
    }
    LoadSettings();
    ShowWindow(g_main, show);
    LoadXInput(); // issue #3: controle no launcher
    SetTimer(g_main, kPadTimer, 16, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!g_names_window || !IsDialogMessageW(g_names_window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return 0;
}
