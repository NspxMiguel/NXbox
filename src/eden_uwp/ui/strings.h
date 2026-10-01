// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace EdenXbox::Ui {

enum class Language { Portuguese, English };

// Every text the UI shows. Add the pt-BR and en strings to the table in strings.cpp, in this order.
enum class Text {
    NavLibrary,
    NavSettings,
    ActionPlay,
    ActionMods,
    ActionDetails,
    HintSelect,
    HintScan,
    HintQuit,
    HintBack,
    RailTitle,
    AddGamesTile,
    AddGamesTitle,
    AddGamesBody,
    EmptyTitle,
    MissingKeysTitle,
    MissingKeysBody,
    ScanningTitle,
    ConvertingTitle,
    SettingsTitle,
    SettingsBody,
    ModsSoon,
    DetailsTitleId,
    DetailsFormat,
    DetailsSize,
    DetailsFile,
    Count,
};

// The UI language: NXBOX_LANG=pt|en when set (process environment or LocalState\nxbox_env.txt),
// otherwise the first language of the system's preference list, otherwise English.
Language CurrentLanguage();

const wchar_t* Tr(Text text);

} // namespace EdenXbox::Ui
