// Fase 49/50: launcher do LEGO Dimensions PC.
// Pede a pasta do jogo (dump do PS4, CUSA01176) e a das DLCs, mostra a arte do proprio dump
// (sce_sys\pic1.png), roda a preparacao na primeira vez (tools\setup\preparar.py: eboot + shaders AOT)
// e abre o port (build\lego_dimensions.exe) com as opcoes escolhidas. Nada do jogo vem com o port.
#define UNICODE
#define _UNICODE
#include "ui.h"
#include "../../src/common/game_region.h"

#include <shellapi.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "      \
                        "processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
using ui::S;

namespace {

constexpr int kW = 760, kH = 730, kHero = 140;
constexpr UINT WM_APP_LINE = WM_APP + 1, WM_APP_DONE = WM_APP + 2;
enum Id { kGameEdit = 100, kGameBrowse, kDlcEdit, kDlcBrowse, kPrepare, kPlay, kFps, kConsole, kLog, kPatchEdit, kPatchBrowse, kFullscreen,
          kResolution, kLanguage, kSkipCutscenes, kUpdate, kIcons, kTransfer, kMods, kOverlay, kNames, kPkg, kSettings, kTools, kDetails, kToypad, kShortcut, kOpenToypad, kGpl };

HWND g_main, g_game, g_patch, g_dlc, g_log, g_prepare, g_play, g_fps, g_console, g_fullscreen, g_focus;
HWND g_resolution, g_language, g_skip, g_update; // Fase 61
HWND g_icons; // Fase 62
HWND g_mods;  // Fase 64
HWND g_overlay, g_names_button; // Fase 65: contador de FPS / info, apelidos dos jogadores
HWND g_settings, g_tools, g_details;
bool g_settings_page=false, g_show_details=false;
bool g_self_test=false;
void LayoutPage(bool resize=true);
bool Prepared();
bool AotReady();
std::wstring Utf8ToWide(const std::string& text);

HWND g_pkg; // Fase 68: extrair PKGs
HWND g_toypad; // Abrir com LegoToypad
HWND g_gpl; // Fase 74: bibliotecas de pipeline (GPL)

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
HFONT g_font, g_small, g_label, g_title, g_subtitle, g_button, g_play_font;
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

fs::path Ini() { return g_root / L"launcher.ini"; }

std::wstring GetText(HWND h) {
    std::wstring s(GetWindowTextLengthW(h) + 1, L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()));
    s.resize(wcslen(s.c_str()));
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\\' || s.back() == L'"')) {
        s.pop_back();
    }
    while (!s.empty() && s.front() == L'"') {
        s.erase(s.begin());
    }
    return s;
}

std::wstring ReadIni(const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t buffer[2048];
    GetPrivateProfileStringW(L"launcher", key, fallback, buffer, 2048, Ini().c_str());
    return buffer;
}

void WriteIni(const wchar_t* key, const std::wstring& value) { WritePrivateProfileStringW(L"launcher", key, value.c_str(), Ini().c_str()); }

void AppendLog(const std::wstring& text) {
    if (GetWindowTextLengthW(g_log) > 60000) {
        SendMessageW(g_log, EM_SETSEL, 0, 20000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
    const int end = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, end, end);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>((ui::LogText(text) + L"\r\n").c_str()));
}

void InvalidateCard() {
    RECT r{0, S(kHero - 70), S(kW), S(kH)};
    InvalidateRect(g_main, &r, FALSE);
}

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
    RECT r{0, 0, S(kW), S(kHero)};
    InvalidateRect(g_main, &r, FALSE);
}

int PageHeight() { return g_settings_page?kH:(g_show_details?700:490); }

void PaintHero(HDC dc,Gdiplus::Graphics& g) {
    const int width=S(kW),height=S(kHero);
    if(g_art) {
        const float scale=std::max(float(width)/g_art->GetWidth(),float(height)/g_art->GetHeight());
        const float sw=width/scale,sh=height/scale;
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(g_art.get(),Gdiplus::Rect(0,0,width,height),(g_art->GetWidth()-sw)/2,(g_art->GetHeight()-sh)/2,sw,sh,Gdiplus::UnitPixel);
    }
    Gdiplus::SolidBrush shade(ui::C(ui::kBg,225));g.FillRectangle(&shade,0,0,width,height);
    g.Flush();
    if(g_icon)DrawIconEx(dc,S(26),S(34),g_icon,S(56),S(56),0,nullptr,DI_NORMAL);
    ui::Text(dc,L"LEGO Dimensions",RECT{S(104),S(32),S(kW-24),S(74)},g_title,ui::kText);
    ui::Text(dc,g_settings_page?L"Settings":L"PC port  \u00b7  "+g_version+L" beta",RECT{S(106),S(79),S(kW-24),S(106)},g_small,ui::kMuted);
}

void PaintCard(HDC dc,Gdiplus::Graphics& g) {
    if(g_settings_page) {
        for(auto field:{g_game,g_patch,g_dlc})ui::FieldBox(g,g_main,field,g_focus==field);
        g.Flush();
        for(const auto& row:std::vector<std::pair<int,const wchar_t*>>{{170,L"Game files"},{245,L"Update folder"},{320,L"DLC folder"},{405,L"Display and controls"}})
            ui::Text(dc,row.second,RECT{S(24),S(row.first),S(kW-24),S(row.first+22)},g_label,ui::kMuted);
        ui::Text(dc,L"Options are saved automatically.",RECT{S(266),S(620),S(kW-24),S(648)},g_small,ui::kMuted);
        return;
    }
    int x=S(24);const int y=S(162);
    for(const auto& state:std::vector<std::pair<std::wstring,COLORREF>>{{g_status.game,g_status.game_c},{g_status.patch,g_status.patch_c},{g_status.dlcs,g_status.dlcs_c},{g_status.shaders,g_status.shaders_c}})
        x+=ui::Pill(dc,g,x,y,state.first,state.second,g_small)+S(8);
    ui::Text(dc,g_step,RECT{S(24),S(200),S(kW-24),S(233)},g_small,ui::kMuted,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    if(g_busy) {
        ui::Progress(g,RECT{S(24),S(438),S(kW-72),S(445)},g_pct);
        ui::Text(dc,std::to_wstring(g_pct)+L"%",RECT{S(kW-66),S(423),S(kW-24),S(455)},g_small,ui::kMuted,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
    }
    if(g_show_details)ui::FieldBox(g,g_main,g_log,false);
}

void Paint(HDC target) {
    RECT client;GetClientRect(g_main,&client);ui::Buffered buffer(target,client);
    HBRUSH background=CreateSolidBrush(ui::kBg);FillRect(buffer.dc,&client,background);DeleteObject(background);
    Gdiplus::Graphics graphics(buffer.dc);graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    PaintHero(buffer.dc,graphics);PaintCard(buffer.dc,graphics);
}

void LayoutPage(bool resize) {
    const int settings_ids[]={kGameEdit,kGameBrowse,kPatchEdit,kPatchBrowse,kDlcEdit,kDlcBrowse,kResolution,kLanguage,kIcons,kFps,kConsole,kFullscreen,kSkipCutscenes,kOverlay,kNames,kToypad,kGpl};
    for(int id:settings_ids)ShowWindow(GetDlgItem(g_main,id),g_settings_page?SW_SHOW:SW_HIDE);
    for(int id:{kPlay,kMods,kTools,kDetails})ShowWindow(GetDlgItem(g_main,id),g_settings_page?SW_HIDE:SW_SHOW);
    ShowWindow(g_log,!g_settings_page&&g_show_details?SW_SHOW:SW_HIDE);
    ShowWindow(g_prepare,!g_settings_page&&(!Prepared()||!AotReady())?SW_SHOW:SW_HIDE);
    for(int id:{kUpdate,kPkg,kTransfer,kOpenToypad,kShortcut})ShowWindow(GetDlgItem(g_main,id),SW_HIDE);
    SetWindowTextW(g_settings,g_settings_page?L"Back":L"Settings");
    MoveWindow(g_settings,S(g_settings_page?24:266),S(g_settings_page?666:326),S(g_settings_page?168:228),S(44),TRUE);
    SetWindowTextW(g_details,g_show_details?L"Hide details":L"Show details");
    EnableWindow(g_settings,!g_busy);EnableWindow(g_tools,!g_busy);EnableWindow(g_mods,!g_busy);
    if(resize) {
        RECT size{0,0,S(kW),S(PageHeight())};AdjustWindowRect(&size,GetWindowLongW(g_main,GWL_STYLE),FALSE);
        SetWindowPos(g_main,nullptr,0,0,size.right-size.left,size.bottom-size.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    InvalidateRect(g_main,nullptr,FALSE);
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

void UpdateStatus() {
    const fs::path game = GetText(g_game), dlc = GetText(g_dlc);
    std::error_code ec;
    const bool game_ok = !game.empty() && fs::is_regular_file(game / L"eboot.bin", ec) && fs::is_regular_file(game / L"sce_sys" / L"param.sfo", ec);
    g_status.game = game.empty() ? ui::T(L"Escolha a pasta do jogo", L"Choose the game folder") : game_ok ? ui::T(L"Jogo encontrado", L"Game found") : ui::T(L"Pasta sem eboot.bin", L"Folder without eboot.bin");
    g_status.game_c = game.empty() ? ui::kAmber : game_ok ? ui::kGreen : ui::kRed;
    const fs::path patch = GetText(g_patch);
    const bool patch_ok = !patch.empty() && (fs::is_regular_file(patch / L"eboot.bin", ec) || fs::is_directory(patch / L"data", ec));
    const bool merged = game_ok && HasPatchDats(game); // atualizacao ja junto do jogo (como no dump mesclado)
    g_status.patch = patch_ok ? ui::T(L"Atualiza\u00e7\u00e3o separada", L"Separate update") : !patch.empty() ? ui::T(L"Atualiza\u00e7\u00e3o: pasta inv\u00e1lida", L"Update: invalid folder")
                     : merged  ? ui::T(L"Atualiza\u00e7\u00e3o no jogo", L"Update included in the game")
                     : game_ok ? ui::T(L"Sem atualiza\u00e7\u00e3o (1.24 necess\u00e1ria)", L"No update (1.24 required)")
                               : ui::T(L"Atualiza\u00e7\u00e3o", L"Update");
    g_status.patch_c = patch_ok || merged ? ui::kGreen : !patch.empty() ? ui::kRed : game_ok ? ui::kAmber : ui::kMuted;
    const int dlcs = CountDlcs(dlc);
    g_status.dlcs = dlcs ? std::to_wstring(dlcs) + (dlcs == 1 ? L" DLC" : L" DLCs") : ui::T(L"Sem DLCs", L"No DLCs");
    g_status.dlcs_c = dlcs ? ui::kBlue : ui::kMuted;
    g_status.shaders = !Prepared() ? ui::T(L"Preparar pendente", L"Prepare pending") : AotReady() ? ui::T(L"Shaders AOT prontos", L"Prepared") : ui::T(L"Sem shaders AOT", L"Preparation needed");
    g_status.shaders_c = !Prepared() ? ui::kAmber : AotReady() ? ui::kGreen : ui::kAmber;
    if (!g_busy && (g_step.empty() || g_step.rfind(L"Clique", 0) == 0 || g_step.rfind(L"Tudo", 0) == 0 || g_step.rfind(L"Click", 0) == 0 || g_step.rfind(L"All set", 0) == 0)) { // dica do proximo passo
        g_step = !game_ok ? ui::T(L"Clique em Procurar e escolha a pasta do jogo.", L"Click Browse and choose the game folder.") : !Prepared()
                     ? ui::T(L"Clique em Preparar: gera os arquivos e traduz os shaders a partir do seu jogo (s\u00f3 na primeira vez).", L"Click Prepare: it builds the files and translates the shaders from your game (first time only).")
                     : ui::T(L"Tudo pronto. Clique em JOGAR!", L"All set. Click PLAY!");
    }
    EnableWindow(g_play, !g_busy && game_ok);
    EnableWindow(g_prepare, !g_busy && game_ok);
    EnableWindow(g_update, !g_busy);
    EnableWindow(g_pkg, !g_busy); // Fase 68
    LayoutPage(false);
    InvalidateCard();
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

void SaveSettings() {
    if (g_self_test) return;
    WriteIni(L"jogo", GetText(g_game));
    WriteIni(L"atualizacao", GetText(g_patch));
    WriteIni(L"dlcs", GetText(g_dlc));
    WriteIni(L"fps60", ui::Checked(g_fps) ? L"1" : L"0");
    WriteIni(L"console", ui::Checked(g_console) ? L"1" : L"0");
    WriteIni(L"telacheia", ui::Checked(g_fullscreen) ? L"1" : L"0");
    WriteIni(L"resolucao", g_res_value);
    WriteIni(L"icones", g_icons_value);
    WriteIni(L"gpl", g_gpl_value); // Fase 74
    WriteIni(L"idioma", g_lang_value);
    WriteIni(L"pularcenas", ui::Checked(g_skip) ? L"1" : L"0");
    WriteIni(L"contador", ui::Checked(g_overlay) ? L"1" : L"0"); // Fase 65
    WriteIni(L"toypad_auto", ui::Checked(g_toypad) ? L"1" : L"0");
}

const Language& CurrentLanguage() {
    for (const Language& language : kLanguages) {
        if (g_lang_value == language.code) {
            return language;
        }
    }
    return kLanguages[0];
}

void UpdateChoiceLabels() {
    SetWindowTextW(g_resolution, (ui::T(L"Resolu\u00e7\u00e3o:  ", L"Resolution:  ") + g_res_value + L"  \u25be").c_str());
    SetWindowTextW(g_language, (ui::T(L"Idioma:  ", L"Language:  ") + std::wstring(ui::T(CurrentLanguage().pt, CurrentLanguage().en)) + L"  \u25be").c_str());
    SetWindowTextW(g_icons, (ui::T(L"\u00cdcones:  ", L"Icons:  ") + std::wstring(ui::T(CurrentIcons().pt, CurrentIcons().en)) + L"  \u25be").c_str());
    InvalidateRect(g_resolution, nullptr, FALSE);
    InvalidateRect(g_language, nullptr, FALSE);
    InvalidateRect(g_icons, nullptr, FALSE);
    SetWindowTextW(g_gpl, (L"GPL:  " + std::wstring(ui::T(CurrentGpl().pt, CurrentGpl().en)) + L"  \u25be").c_str()); // Fase 74
    InvalidateRect(g_gpl, nullptr, FALSE);
}

// Menu suspenso logo abaixo do botao; devolve o indice escolhido ou -1.
int PopupChoice(HWND button, const std::vector<std::wstring>& items, int current) {
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i) {
        AppendMenuW(menu, MF_STRING | (static_cast<int>(i) == current ? MF_CHECKED : 0), i + 1, items[i].c_str());
    }
    RECT r;
    GetWindowRect(button, &r);
    const int picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, r.left, r.bottom + S(2), 0, g_main, nullptr);
    DestroyMenu(menu);
    return picked - 1;
}

void PickResolution() {
    std::vector<std::wstring> items;
    int current = -1;
    for (const wchar_t* res : kResolutions) {
        if (g_res_value == res) {
            current = static_cast<int>(items.size());
        }
        items.push_back(std::wstring(res) + (std::wstring(res) == L"1920x1080" ? ui::T(L"   (padr\u00e3o)", L"   (default)") : L""));
    }
    const int picked = PopupChoice(g_resolution, items, current);
    if (picked >= 0) {
        g_res_value = kResolutions[picked];
        UpdateChoiceLabels();
        SaveSettings();
        AppendLog(ui::Pt() ? (L"Resolução alterada para " + g_res_value + L" (salva! Não precisa Preparar novamente).")
                           : (L"Resolution changed to " + g_res_value + L" (saved! No need to click Prepare again)."));
    }
}

void PickIcons() { // Fase 62
    std::vector<std::wstring> items;
    int current = 0;
    for (const IconChoice& choice : kIconChoices) {
        if (g_icons_value == choice.code) {
            current = static_cast<int>(items.size());
        }
        items.push_back(std::wstring(ui::T(choice.pt, choice.en)) +
                        (std::wstring(choice.code) == L"auto" ? ui::T(L"   (pelo controle ligado)", L"   (from the controller)") : L""));
    }
    const int picked = PopupChoice(g_icons, items, current);
    if (picked >= 0) {
        g_icons_value = kIconChoices[picked].code;
        UpdateChoiceLabels();
        SaveSettings();
        const std::wstring icon_name = ui::T(CurrentIcons().pt, CurrentIcons().en);
        AppendLog(ui::Pt() ? (L"Ícones alterados para " + icon_name + L" (salvo! Não precisa Preparar novamente).")
                           : (L"Icons changed to " + icon_name + L" (saved! No need to click Prepare again)."));
    }
}

void PickGpl() { // Fase 74
    std::vector<std::wstring> items;
    int current = 0;
    for (const IconChoice& choice : kGplChoices) {
        if (g_gpl_value == choice.code) {
            current = static_cast<int>(items.size());
        }
        items.push_back(std::wstring(ui::T(choice.pt, choice.en)) +
                        (std::wstring(choice.code) == L"auto" ? ui::T(L"   (NVIDIA / Intel ligado, AMD desligado)", L"   (NVIDIA / Intel on, AMD off)")
                                                              : L""));
    }
    const int picked = PopupChoice(g_gpl, items, current);
    if (picked >= 0) {
        g_gpl_value = kGplChoices[picked].code;
        UpdateChoiceLabels();
        SaveSettings();
    }
}

void PickLanguage() {
    std::vector<std::wstring> items;
    int current = 0;
    for (const Language& language : kLanguages) {
        if (g_lang_value == language.code) {
            current = static_cast<int>(items.size());
        }
        items.push_back(ui::T(language.pt, language.en));
    }
    const int picked = PopupChoice(g_language, items, current);
    if (picked < 0 || g_lang_value == kLanguages[picked].code) {
        return;
    }
    g_lang_value = kLanguages[picked].code;
    SaveSettings();
    // Textos do launcher: portugues so com "pt" (ou automatico num Windows em portugues). Se mudar, reabre.
    const bool pt = g_lang_value == L"pt" || (g_lang_value == L"auto" && PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE);
    if (pt == ui::Pt() || g_busy) {
        UpdateChoiceLabels();
        return;
    }
    SetEnvironmentVariableW(L"LD_LANG", pt ? L"pt" : L"en");
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    std::wstring command = L"\"" + std::wstring(exe) + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, g_root.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DestroyWindow(g_main);
    } else {
        UpdateChoiceLabels();
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
    std::wstring args = L" --app0 " + Quote(GetText(g_game));
    const std::wstring dlc = GetText(g_dlc);
    if (!dlc.empty()) {
        args += L" --addcont " + Quote(dlc);
    }
    if (const std::wstring patch = GetText(g_patch); !patch.empty()) {
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
            SetWindowTextW(g_game, line.substr(5).c_str());
            LoadArt(line.substr(5));
            if (GetText(g_patch).empty()) {
                SetWindowTextW(g_patch, FindPatch(line.substr(5)).wstring().c_str());
            }
            return;
        }
        if (line.rfind(L"ATUALIZACAO ", 0) == 0) {
            SetWindowTextW(g_patch, line.substr(12).c_str());
            return;
        }
        if (line.rfind(L"DLCS ", 0) == 0) {
            SetWindowTextW(g_dlc, line.substr(5).c_str());
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
        InvalidateCard();
        return;
    }
    if (line.rfind(L"ERRO ", 0) == 0) {
        AppendLog(std::wstring(ui::T(L"ERRO: ", L"ERROR: ")) + line.substr(5));
        g_step = std::wstring(ui::T(L"Erro: ", L"Error: ")) + line.substr(5);
        InvalidateCard();
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
    std::wstring args = L" --root " + Quote(g_root.wstring()) + L" --app0 " + Quote(GetText(g_game));
    if (const std::wstring patch = GetText(g_patch); !patch.empty()) {
        args += L" --patch " + Quote(patch);
    }
    if (const std::wstring dlc = GetText(g_dlc); !dlc.empty()) {
        args += L" --addcont " + Quote(dlc);
    }
    // Fase 67: pacotes de mod com nome proprio pelo Anomaly (connorh315) ou pelo carregador antigo do port
    args += ReadIni(L"anomaly", L"1") == L"1" ? L" --carregador anomaly" : L" --carregador port";
    g_step = ui::T(L"Preparando os mods...", L"Preparing the mods...");
    InvalidateCard();
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
    const std::wstring game = GetText(g_game);
    const std::wstring start = !game.empty() ? fs::path(game).parent_path().wstring() : fs::path(pkgs[0]).parent_path().wstring();
    const std::wstring destination = BrowseFolder(ui::T(L"Pasta onde extrair (o jogo fica em <pasta>\\CUSA01176)", L"Folder to extract into (uses the package title ID)"), start);
    if (destination.empty()) {
        return;
    }
    std::wstring args = L" --destino " + Quote(destination);
    // DLCs: na pasta de DLCs ja escolhida quando ela e a pasta do jogo dentro do addcont (CUSA01176), senao <pasta>\addcont\CUSA01176
    const std::wstring dlc = GetText(g_dlc);
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
void ModsMenu(HWND button) {
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
    const fs::path game = GetText(g_game);
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
    RECT r;
    GetWindowRect(button, &r);
    const int picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, r.left, r.bottom + S(2), 0, g_main, nullptr);
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
                            Quote((g_root / L"tools" / L"setup" / L"botoes.py").wstring()) + L" --app0 " + Quote(GetText(g_game));
        if (const std::wstring patch = GetText(g_patch); !patch.empty()) {
            make += L" --patch " + Quote(patch);
        }
        STARTUPINFOW make_si{sizeof(make_si)};
        PROCESS_INFORMATION make_pi{};
        g_step = ui::T(L"Montando os \u00edcones dos bot\u00f5es...", L"Building the button icons...");
        InvalidateCard();
        UpdateWindow(g_main);
        if (CreateProcessW(nullptr, make.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_root.c_str(), &make_si, &make_pi)) {
            WaitForSingleObject(make_pi.hProcess, 120000);
            CloseHandle(make_pi.hThread);
            CloseHandle(make_pi.hProcess);
        }
        g_step.clear();
        UpdateStatus();
    }
    if (ui::Checked(g_toypad)) {
        LaunchLegoToypad();
    }
    PrepareMods(); // Fase 64
    std::wstring command = Quote((g_root / L"build" / L"lego_dimensions.exe").wstring()) + L" --app0 " + Quote(GetText(g_game));
    const std::wstring dlc = GetText(g_dlc);
    command += dlc.empty() ? L" --no-dlc" : L" --addcont " + Quote(dlc);
    if (const std::wstring patch = GetText(g_patch); !patch.empty()) {
        command += L" --patch " + Quote(patch); // Fase 58
    }
    if (ui::Checked(g_fps)) {
        command += L" --fps60";
    }
    if (ui::Checked(g_fullscreen)) {
        command += L" --fullscreen"; // Fase 60: tela cheia sem bordas (F11 alterna dentro do jogo)
    }
    command += L" --resolution " + g_res_value; // Fase 61: tamanho da janela
    command += L" --button-icons " + g_icons_value; // Fase 62
    if (g_gpl_value != L"auto") {
        command += g_gpl_value == L"ligado" ? L" --gpl" : L" --no-gpl"; // Fase 74
    }
    AppendLog(ui::Pt() ? (L"Iniciando em " + g_res_value + (ui::Checked(g_fullscreen) ? L" (Tela Cheia)..." : L" (Janela)..."))
                       : (L"Starting at " + g_res_value + (ui::Checked(g_fullscreen) ? L" (Fullscreen)..." : L" (Windowed)...")));
    if (g_lang_value != L"auto") {
        command += L" --lang " + g_lang_value; // Fase 61: idioma do jogo (e do port)
    }
    if (ui::Checked(g_skip)) {
        command += L" --skip-cutscenes"; // Fase 61: qualquer cutscene pode ser pulada, mesmo sem 100%
    }
    if (ui::Checked(g_overlay)) {
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
    const bool console = ui::Checked(g_console);
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
        InvalidateCard();
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

void ToolsMenu(HWND button) {
    HMENU menu=CreatePopupMenu();
    for(const auto& entry:std::vector<std::pair<int,const wchar_t*>>{{kOpenToypad,L"Open Toypad"},{kShortcut,L"Create desktop shortcut"},{kPrepare,L"Prepare game"},{kPkg,L"Extract PKG..."},{kTransfer,L"Transfer a save..."},{kUpdate,L"Check for updates"}})
        AppendMenuW(menu,MF_STRING|(IsWindowEnabled(GetDlgItem(g_main,entry.first))?0:MF_GRAYED),entry.first,entry.second);
    RECT rect;GetWindowRect(button,&rect);
    int picked=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN|TPM_TOPALIGN|TPM_NONOTIFY,rect.left,rect.bottom+S(2),0,g_main,nullptr);
    DestroyMenu(menu);if(picked)SendMessageW(g_main,WM_COMMAND,MAKEWPARAM(picked,BN_CLICKED),0);
}

void CreateControls() {
    g_game=ui::Edit(g_main,S(38),S(205),S(kW-222),S(24),kGameEdit,g_font);
    g_patch=ui::Edit(g_main,S(38),S(280),S(kW-222),S(24),kPatchEdit,g_font);
    g_dlc=ui::Edit(g_main,S(38),S(355),S(kW-222),S(24),kDlcEdit,g_font);
    for(const auto& row:std::vector<std::pair<int,int>>{{kGameBrowse,194},{kPatchBrowse,269},{kDlcBrowse,344}})
        ui::Button(g_main,L"Browse...",ui::Style::kSecondary,S(kW-160),S(row.second),S(136),S(44),row.first,g_button);
    g_log=ui::Edit(g_main,S(38),S(500),S(kW-76),S(158),kLog,g_small,ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL);
    SetWindowTheme(g_log,L"DarkMode_Explorer",nullptr);
    g_resolution=ui::Button(g_main,L"",ui::Style::kSecondary,S(24),S(442),S(228),S(44),kResolution,g_small);
    g_language=ui::Button(g_main,L"",ui::Style::kSecondary,S(266),S(442),S(228),S(44),kLanguage,g_small);
    g_icons=ui::Button(g_main,L"",ui::Style::kSecondary,S(508),S(442),S(228),S(44),kIcons,g_small);
    g_fps=ui::Button(g_main,L"60 FPS",ui::Style::kToggle,S(24),S(502),S(228),S(42),kFps,g_font);
    g_console=ui::Button(g_main,L"Show console",ui::Style::kToggle,S(266),S(502),S(228),S(42),kConsole,g_font);
    g_fullscreen=ui::Button(g_main,L"Fullscreen",ui::Style::kToggle,S(508),S(502),S(228),S(42),kFullscreen,g_font);
    g_skip=ui::Button(g_main,L"Skip cutscenes",ui::Style::kToggle,S(24),S(558),S(228),S(42),kSkipCutscenes,g_small);
    g_overlay=ui::Button(g_main,L"Performance (F10)",ui::Style::kToggle,S(266),S(558),S(228),S(42),kOverlay,g_small);
    g_names_button=ui::Button(g_main,L"Player nicknames...",ui::Style::kSecondary,S(508),S(558),S(228),S(42),kNames,g_small);
    g_toypad=ui::Button(g_main,L"Start Toypad with game",ui::Style::kToggle,S(24),S(614),S(228),S(42),kToypad,g_small);
    g_gpl=ui::Button(g_main,L"",ui::Style::kSecondary,S(266),S(614),S(228),S(42),kGpl,g_small); // Fase 74
    ui::Button(g_main,L"Open Toypad",ui::Style::kSecondary,0,0,1,1,kOpenToypad,g_font);
    ui::Button(g_main,L"Create shortcut",ui::Style::kSecondary,0,0,1,1,kShortcut,g_font);
    g_version=ReadVersion();
    g_update=ui::Button(g_main,L"Check for updates",ui::Style::kSecondary,0,0,1,1,kUpdate,g_font);
    g_pkg=ui::Button(g_main,L"Extract PKG...",ui::Style::kSecondary,0,0,1,1,kPkg,g_font);
    ui::Button(g_main,L"Transfer save",ui::Style::kSecondary,0,0,1,1,kTransfer,g_font);
    g_prepare=ui::Button(g_main,L"Prepare game",ui::Style::kSecondary,S(kW-192),S(391),S(168),S(40),kPrepare,g_button);
    g_play=ui::Button(g_main,L"PLAY",ui::Style::kPrimary,S(24),S(246),S(kW-48),S(60),kPlay,g_play_font);
    g_mods=ui::Button(g_main,L"Mods",ui::Style::kSecondary,S(24),S(326),S(228),S(44),kMods,g_button);
    g_settings=ui::Button(g_main,L"Settings",ui::Style::kSecondary,S(266),S(326),S(228),S(44),kSettings,g_button);
    g_tools=ui::Button(g_main,L"Tools",ui::Style::kSecondary,S(508),S(326),S(228),S(44),kTools,g_button);
    g_details=ui::Button(g_main,L"Show details",ui::Style::kSecondary,S(24),S(391),S(168),S(40),kDetails,g_small);
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
    SetWindowTextW(g_game, game.c_str());
    std::wstring patch = ReadIni(L"atualizacao");
    if (patch.empty()) {
        patch = FindPatch(game).wstring(); // Fase 58
    }
    SetWindowTextW(g_patch, patch.c_str());
    if (dlc.empty() || CountDlcs(dlc) == 0) {
        const fs::path auto_dlc = FindDlc(game);
        if (!auto_dlc.empty()) {
            dlc = auto_dlc.wstring();
        }
    }
    SetWindowTextW(g_dlc, dlc.c_str());
    ui::SetChecked(g_fps, ReadIni(L"fps60", L"1") == L"1");
    ui::SetChecked(g_console, ReadIni(L"console", L"0") == L"1");
    ui::SetChecked(g_fullscreen, ReadIni(L"telacheia", L"0") == L"1");
    ui::SetChecked(g_skip, ReadIni(L"pularcenas", L"0") == L"1");
    ui::SetChecked(g_overlay, ReadIni(L"contador", L"0") == L"1"); // Fase 65
    ui::SetChecked(g_toypad, ReadIni(L"toypad_auto", L"0") == L"1");
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
    UpdateChoiceLabels();
    LoadArt(game);
    AppendLog(ui::T(L"Bem-vindo! Escolha as pastas, clique em Preparar na primeira vez e depois em JOGAR.", L"Welcome! Choose the folders, click Prepare the first time and then PLAY."));
    UpdateStatus();
}

// Hidden, asset-free layout regression. Never loads/saves user settings, starts
// tools or a game, displays a window, or touches the installed game folder.
int SelfTest(const fs::path& report) {
    std::vector<std::string> failures;
    auto check=[&](bool ok,const std::string& message){if(!ok)failures.push_back(message);};
    check(ui::LogText(L"INSTALADO Example")==L"INSTALLED Example","mod protocol display translation");
    check(ui::LogText(L"C:\\Mods\\ERRO folder")==L"C:\\Mods\\ERRO folder","path changed by log translation");
    check(Utf8ToWide("Example \xC3\xA9")==L"Example \u00e9","UTF-8 disabled-mod name round trip");
    for(int id=kGameEdit;id<=kOpenToypad;++id)
        check(GetDlgItem(g_main,id)!=nullptr,"missing control "+std::to_string(id));
    auto visible=[](HWND h){return (GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)!=0;};
    auto layout=[&](bool settings,bool details) {
        g_settings_page=settings;g_show_details=details;LayoutPage();
        RECT client{};GetClientRect(g_main,&client);
        std::vector<std::pair<int,RECT>> controls;
        for(int id=kGameEdit;id<=kOpenToypad;++id) {
            HWND h=GetDlgItem(g_main,id);if(!h||!visible(h))continue;
            RECT r{};GetWindowRect(h,&r);MapWindowPoints(nullptr,g_main,reinterpret_cast<POINT*>(&r),2);
            check(r.left>=0&&r.top>=0&&r.right<=client.right&&r.bottom<=client.bottom,"control outside page "+std::to_string(id));
            for(const auto& previous:controls) {
                RECT overlap{};
                check(!IntersectRect(&overlap,&r,&previous.second),"overlapping controls "+std::to_string(previous.first)+" and "+std::to_string(id));
            }
            controls.push_back({id,r});
        }
        check(visible(g_play)!=settings,"Play visibility");
        check(visible(g_game)==settings,"game folder visibility");
        check(visible(g_log)==(!settings&&details),"log visibility");
        for(int id:{kUpdate,kPkg,kTransfer,kOpenToypad,kShortcut})check(!visible(GetDlgItem(g_main,id)),"tool command leaked onto page");
    };
    layout(false,false);layout(false,true);layout(true,false);
    g_busy=true;LayoutPage();check(!IsWindowEnabled(g_settings)&&!IsWindowEnabled(g_tools)&&!IsWindowEnabled(g_mods),"busy controls enabled");
    g_busy=false;LayoutPage();
    HDC dc=GetDC(g_main);HGDIOBJ previous=SelectObject(dc,g_font);wchar_t face[LF_FACESIZE]{};
    GetTextFaceW(dc,LF_FACESIZE,face);SelectObject(dc,previous);ReleaseDC(g_main,dc);
    check(_wcsicmp(face,L"Segoe UI")==0,"unexpected UI font");
    std::ofstream out(report,std::ios::trunc);
    if(!out)return 2;
    out<<"{\"passed\":"<<(failures.empty()?"true":"false")<<",\"control_count\":"<<(kOpenToypad-kGameEdit+1)<<",\"pages\":3,\"scale\":"<<ui::g_scale<<",\"failures\":[";
    for(size_t i=0;i<failures.size();++i)out<<(i?",":"")<<'"'<<failures[i]<<'"';
    out<<"]}\n";out.close();
    return failures.empty()?0:1;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM:
        ui::DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, ui::kField);
        SetTextColor(dc, reinterpret_cast<HWND>(lp) == g_log ? ui::kMuted : ui::kText);
        return reinterpret_cast<LRESULT>(g_field_brush);
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kSettings:
            SaveSettings();g_settings_page=!g_settings_page;LayoutPage();break;
        case kDetails:
            g_show_details=!g_show_details;LayoutPage();break;
        case kTools:
            ToolsMenu(g_tools);break;
        case kGameBrowse: {
            const std::wstring folder = BrowseFolder(ui::T(L"Pasta do jogo (dump CUSA01176)", L"Game folder (CUSA01176 / CUSA00935 dump)"), GetText(g_game));
            if (!folder.empty()) {
                SetWindowTextW(g_game, folder.c_str());
                LoadArt(folder);
                if (GetText(g_patch).empty()) { // Fase 58: acha a pasta da atualizacao sozinha
                    SetWindowTextW(g_patch, FindPatch(folder).wstring().c_str());
                }
                if (GetText(g_dlc).empty() || CountDlcs(GetText(g_dlc)) == 0) {
                    SetWindowTextW(g_dlc, FindDlc(folder).wstring().c_str());
                }
                SaveSettings();
            }
            break;
        }
        case kPatchBrowse: {
            const std::wstring folder = BrowseFolder(ui::T(L"Pasta da atualiza\u00e7\u00e3o (CUSA01176-patch)", L"Update folder (CUSA01176-patch / CUSA00935-patch)"), GetText(g_patch).empty() ? GetText(g_game) : GetText(g_patch));
            if (!folder.empty()) {
                SetWindowTextW(g_patch, folder.c_str());
                SaveSettings();
            }
            break;
        }
        case kDlcBrowse: {
            const std::wstring folder = BrowseFolder(ui::T(L"Pasta das DLCs (uma subpasta por DLC)", L"DLC folder (one subfolder per DLC)"), GetText(g_dlc));
            if (!folder.empty()) {
                SetWindowTextW(g_dlc, folder.c_str());
                SaveSettings();
            }
            break;
        }
        case kGameEdit:
        case kPatchEdit:
        case kDlcEdit:
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                g_focus = HIWORD(wp) == EN_SETFOCUS ? reinterpret_cast<HWND>(lp) : nullptr;
                if (HIWORD(wp) == EN_KILLFOCUS) {
                    if (LOWORD(wp) == kGameEdit) {
                        LoadArt(GetText(g_game));
                        if (GetText(g_patch).empty()) {
                            SetWindowTextW(g_patch, FindPatch(GetText(g_game)).wstring().c_str());
                        }
                    }
                    SaveSettings();
                }
                InvalidateCard();
            }
            if (HIWORD(wp) == EN_CHANGE) {
                UpdateStatus();
            }
            break;
        case kPrepare:
            if (!Prepared() || MessageBoxW(hwnd, ui::T(L"Ja esta preparado. Preparar de novo (refaz os shaders AOT)?", L"Already prepared. Prepare again (rebuilds the AOT shaders)?"), L"LEGO Dimensions PC",
                                           MB_YESNO | MB_ICONQUESTION) == IDYES) {
                RunPrepare();
            }
            break;
        case kPlay:
            RunGame();
            break;
        case kResolution:
            PickResolution();
            break;
        case kGpl:
            PickGpl();
            break;
        case kIcons:
            PickIcons();
            break;
        case kUpdate:
            CheckUpdate();
            break;
        case kLanguage:
            PickLanguage();
            break;
        case kTransfer:
            OpenTransfer();
            break;
        case kMods:
            ModsMenu(reinterpret_cast<HWND>(lp));
            break;
        case kPkg:
            ExtractPkgs();
            break;
        case kNames:
            OpenNamesDialog();
            break;
        case kFps:
        case kConsole:
        case kFullscreen:
        case kSkipCutscenes:
        case kOverlay:
        case kToypad:
            ui::Toggle(reinterpret_cast<HWND>(lp));
            SaveSettings();
            break;
        case kShortcut:
            CreateDesktopShortcut();
            break;
        case kOpenToypad:
            LaunchLegoToypad(true);
            break;
        }
        return 0;
    case WM_APP_LINE: {
        std::wstring* line = reinterpret_cast<std::wstring*>(lp);
        OnLine(*line);
        delete line;
        return 0;
    }
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
                MessageBoxW(hwnd, (std::wstring(ui::T(L"Extração concluída! As pastas já estão nos campos do launcher.",
                                                      L"Extraction finished! The folders are already in the launcher fields.")) +
                                   (Prepared() ? L"" : ui::T(L"\n\nAgora clique em Preparar.", L"\n\nNow click Prepare.")))
                                      .c_str(),
                            L"LEGO Dimensions PC", MB_ICONINFORMATION);
            } else {
                AppendLog(ui::T(L"A extração dos PKGs terminou com erro (veja acima).", L"The PKG extraction failed (see above)."));
            }
            return 0;
        }
        if (wp == 0) {
            g_pct = 100;
            g_step = ui::T(L"Pronto! Clique em JOGAR.", L"Done! Click PLAY.");
            AppendLog(ui::T(L"Preparacao concluida.", L"Preparation finished."));
        } else {
            AppendLog(ui::T(L"A preparacao terminou com erro (veja acima).", L"The preparation failed (see above)."));
        }
        UpdateStatus();
        return 0;
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
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const RECT place = ui::FitWindow(kW, kH, style); // Fase 61: cabe em telas pequenas
    g_font = ui::Font(15);
    g_small = ui::Font(13);
    g_label = ui::Font(11.5f, FW_SEMIBOLD);
    g_title = ui::Font(30, FW_SEMIBOLD);
    g_subtitle = ui::Font(15, FW_SEMIBOLD);
    g_button = ui::Font(15, FW_SEMIBOLD);
    g_play_font = ui::Font(21, FW_BLACK);
    g_field_brush = CreateSolidBrush(ui::kField);
    g_bg_brush = CreateSolidBrush(ui::kBg);
    g_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, S(56), S(56), 0));
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bg_brush;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, S(16), S(16), 0));
    wc.lpszClassName = L"LegoDimensionsPcLauncher";
    RegisterClassExW(&wc);
    g_main = CreateWindowExW(0, wc.lpszClassName, L"LEGO Dimensions PC - Beta", style, place.left, place.top, place.right, place.bottom,
                             nullptr, nullptr, instance, nullptr);
    ui::DarkTitle(g_main);
    CreateControls();
    if(g_self_test) {
        const int result=SelfTest(test_report);DestroyWindow(g_main);CoUninitialize();return result;
    }
    LoadSettings();
    LayoutPage();
    ShowWindow(g_main, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return 0;
}
