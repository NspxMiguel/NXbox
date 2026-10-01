// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/strings.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <windows.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.UserProfile.h>

#include "eden_uwp/diagnostic.h"

namespace EdenXbox::Ui {
namespace {

struct Entry {
    const wchar_t* pt;
    const wchar_t* en;
};

// One row per Text value, in the same order as the enum.
constexpr Entry kTable[] = {
    {L"Biblioteca", L"Library"},                    // NavLibrary
    {L"Configurações", L"Settings"},                // NavSettings
    {L"Jogar", L"Play"},                            // ActionPlay
    {L"Mods", L"Mods"},                             // ActionMods
    {L"Detalhes", L"Details"},                      // ActionDetails
    {L"Selecionar", L"Select"},                     // HintSelect
    {L"Procurar", L"Scan"},                         // HintScan
    {L"Sair", L"Exit"},                             // HintQuit
    {L"Voltar", L"Back"},                           // HintBack
    {L"Sua biblioteca", L"Your library"},           // RailTitle
    {L"Adicionar\njogos", L"Add\ngames"},           // AddGamesTile, two lines inside the tile
    {L"Adicionar jogos", L"Add games"},             // AddGamesTitle
    {L"Copie arquivos .nsp ou .xci para a pasta games do console pelo Device Portal e "
     L"pressione A para procurar de novo.",
     L"Copy .nsp or .xci files into the console's games folder with Device Portal, then "
     L"press A to scan again."},                           // AddGamesBody
    {L"Nenhum jogo ainda", L"No games yet"},               // EmptyTitle
    {L"Faltam as chaves", L"Keys are missing"},            // MissingKeysTitle
    {L"Os jogos estão na pasta games, mas faltam as chaves para abri-los. Copie o prod.keys "
     L"para a pasta eden\\keys do app pelo Device Portal e pressione A para procurar de novo.",
     L"The games are in the games folder, but the keys to open them are missing. Copy "
     L"prod.keys into the app's eden\\keys folder with Device Portal, then press A to scan "
     L"again."}, // MissingKeysBody
    {L"Procurando jogos", L"Looking for games"},           // ScanningTitle
    {L"Convertendo NSZ para NSP", L"Converting NSZ to NSP"}, // ConvertingTitle
    {L"Configurações", L"Settings"},                       // SettingsTitle
    {L"Fontes de jogos, mods e arte entram aqui nas próximas versões.",
     L"Game sources, mods and art will live here in upcoming versions."}, // SettingsBody
    {L"Os mods chegam na próxima versão.", L"Mods arrive in the next version."}, // ModsSoon
    {L"ID do título", L"Title ID"}, // DetailsTitleId
    {L"Formato", L"Format"},        // DetailsFormat
    {L"Tamanho", L"Size"},          // DetailsSize
    {L"Arquivo", L"File"},          // DetailsFile
};
static_assert(std::size(kTable) == static_cast<std::size_t>(Text::Count),
              "every Text needs a row in kTable");

// nxbox_env.txt is only applied to the environment once Mesa starts, which is after this screen,
// so the setting is also read straight from the file.
std::string ForcedLanguage() {
    if (const char* value = std::getenv("NXBOX_LANG")) {
        return value;
    }
    try {
        const std::filesystem::path file =
            std::filesystem::path(std::wstring_view(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
            "nxbox_env.txt";
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            constexpr std::string_view key = "NXBOX_LANG=";
            if (line.starts_with(key)) {
                return line.substr(key.size());
            }
        }
    } catch (const winrt::hresult_error&) {
        // No LocalFolder to read: fall back to the system language.
    }
    return {};
}

Language ResolveLanguage() {
    std::string forced = ForcedLanguage();
    for (char& letter : forced) {
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    if (forced.starts_with("pt")) {
        Diagnostic("UI language pt (NXBOX_LANG)");
        return Language::Portuguese;
    }
    if (forced.starts_with("en")) {
        Diagnostic("UI language en (NXBOX_LANG)");
        return Language::English;
    }
    try {
        const auto languages =
            winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages();
        if (languages.Size() > 0) {
            const winrt::hstring first = languages.GetAt(0);
            const std::wstring_view tag = first;
            const bool portuguese = tag.starts_with(L"pt");
            Diagnostic("UI language " + std::string(portuguese ? "pt" : "en") + " (system " +
                       winrt::to_string(first) + ")");
            return portuguese ? Language::Portuguese : Language::English;
        }
    } catch (const winrt::hresult_error&) {
        // Preference list unavailable: English below.
    }
    Diagnostic("UI language en (default)");
    return Language::English;
}

} // namespace

Language CurrentLanguage() {
    static const Language language = ResolveLanguage();
    return language;
}

const wchar_t* Tr(Text text) {
    const Entry& entry = kTable[static_cast<std::size_t>(text)];
    return CurrentLanguage() == Language::Portuguese ? entry.pt : entry.en;
}

} // namespace EdenXbox::Ui
