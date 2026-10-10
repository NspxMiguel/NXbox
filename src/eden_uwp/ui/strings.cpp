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
    {L"Resolução", L"Resolution"},            // ResolutionTitle
    {L"1x (720p/1080p)", L"1x (720p/1080p)"}, // Resolution1X
    {L"1.5x", L"1.5x"},                       // Resolution1_5X
    {L"2x", L"2x"},                           // Resolution2X
    {L"3x (4K)", L"3x (4K)"},                 // Resolution3X
    {L"Usa mais memória: pode ficar mais lento ou fechar em jogos pesados.",
     L"Uses more memory: may be slower or close on heavy games."}, // ResolutionMemoryHint
    {L"Não foi possível salvar. Pressione A para tentar de novo.",
     L"Could not save. Press A to try again."}, // ResolutionSaveFailed
    {L"Selecionar", L"Select"},                     // HintSelect
    {L"Procurar", L"Scan"},                         // HintScan
    {L"Sair", L"Exit"},                             // HintQuit
    {L"Voltar", L"Back"},                           // HintBack
    {L"Sua biblioteca", L"Your library"},           // RailTitle
    {L"Adicionar\njogos", L"Add\ngames"},           // AddGamesTile, two lines inside the tile
    {L"Adicionar jogos", L"Add games"},             // AddGamesTitle
    {L"Pressione A para importar de um pendrive, ou copie arquivos .nsp ou .xci para a pasta "
     L"games do console pelo Device Portal.",
     L"Press A to import from a USB drive, or copy .nsp or .xci files into the console's "
     L"games folder with Device Portal."},                           // AddGamesBody
    {L"Nenhum jogo ainda", L"No games yet"},               // EmptyTitle
    {L"Faltam as chaves", L"Keys are missing"},            // MissingKeysTitle
    {L"Os jogos estão na pasta games, mas faltam as chaves para abri-los. Copie o prod.keys "
     L"para a pasta eden\\keys do app pelo Device Portal, ou pressione A para importar de um pendrive.",
     L"The games are in the games folder, but the keys to open them are missing. Copy "
     L"prod.keys into the app's eden\\keys folder with Device Portal, or press A to import "
     L"from a USB drive."}, // MissingKeysBody
    {L"Procurando jogos", L"Looking for games"},           // ScanningTitle
    {L"Convertendo o jogo para o formato do emulador",
     L"Converting the game to the emulator's format"}, // ConvertingTitle
    {L"Configurações", L"Settings"},                       // SettingsTitle
    {L"Fontes de jogos, mods e arte entram aqui nas próximas versões.",
     L"Game sources, mods and art will live here in upcoming versions."}, // SettingsBody
    {L"Os mods chegam na próxima versão.", L"Mods arrive in the next version."}, // ModsSoon
    {L"ID do título", L"Title ID"}, // DetailsTitleId
    {L"Formato", L"Format"},        // DetailsFormat
    {L"Tamanho", L"Size"},          // DetailsSize
    {L"Arquivo", L"File"},          // DetailsFile
    {L"Mods", L"Mods"}, // ModsTitle
    {L"mods da versão de Switch no GameBanana. Instala com um botão, desliga quando quiser.", L"Switch mods on GameBanana. Install with one button, turn off whenever you like."}, // ModsSourceLine
    {L"Mais baixados", L"Most downloaded"}, // ChipTop
    {L"Gráficos", L"Graphics"}, // ChipGraphics
    {L"Interface", L"Interface"}, // ChipInterface
    {L"Jogabilidade", L"Gameplay"}, // ChipGameplay
    {L"Traduções e dublagens", L"Translations and dubs"}, // ChipTranslations
    {L"Idioma", L"Language"}, // HintLanguage
    {L"Instalados", L"Installed"}, // ChipInstalled
    {L"Instalar", L"Install"}, // ModInstall
    {L"Instalado", L"Installed"}, // ModInstalled
    {L"Desligado", L"Off"}, // ModDisabled
    {L"Baixando", L"Downloading"}, // ModDownloading
    {L"Instalando", L"Installing"}, // ModInstalling
    {L"Formato ainda não suportado", L"Format not supported yet"}, // ModUnsupported
    {L"Falhou, tentar de novo", L"Failed, try again"}, // ModFailed
    {L"downloads", L"downloads"}, // ModDownloadsCaption
    {L"curtidas", L"likes"}, // ModLikesCaption
    {L"Procurando o jogo no GameBanana", L"Looking for the game on GameBanana"}, // ModsSearching
    {L"Carregando os mods", L"Loading the mods"}, // ModsLoading
    {L"Este jogo não está no GameBanana.", L"This game is not on GameBanana."}, // ModsNoGame
    {L"Sem conexão com o GameBanana. Pressione A para tentar de novo.", L"Cannot reach GameBanana. Press A to try again."}, // ModsOffline
    {L"Nada nesta categoria ainda.", L"Nothing in this category yet."}, // ModsNothing
    {L"Você ainda não instalou nenhum mod.", L"You have not installed any mod yet."}, // ModsNoneInstalled
    {L"Instalar", L"Install"}, // HintInstall
    {L"Ver o mod", L"View the mod"}, // HintViewMod
    {L"Ligar/desligar", L"Turn on/off"}, // HintToggle
    {L"Autor", L"Author"}, // ModAuthor
    {L"Categoria", L"Category"}, // ModCategory
    {L"Downloads", L"Downloads"}, // ModDownloadsLabel
    {L"Arquivos", L"Files"}, // ModFiles
    {L"Carregando…", L"Loading…"}, // ModLoadingDetails
    {L"Mod instalado", L"Mod installed"}, // ToastModInstalled
    {L"Mod ligado", L"Mod on"}, // ToastModOn
    {L"Mod desligado", L"Mod off"}, // ToastModOff
    {L"Já instalado. Use X para ligar ou desligar.", L"Already installed. Press X to turn it on or off."}, // ToastModAlready
    {L"Sincronizar saves com o SwitchSaveSync?", L"Sync saves with SwitchSaveSync?"}, // SyncSetupTitle
    {L"Os saves ficam no Google Drive, os mesmos do seu Switch: o app baixa antes de abrir um jogo e envia depois que você fecha. Dá para mudar depois em Configurações.", L"Saves live on Google Drive, the same ones your Switch uses: they are downloaded before a game opens and uploaded after you close it. You can change this later in Settings."}, // SyncSetupBody
    {L"Sim", L"Yes"}, // SyncYes
    {L"Agora não", L"Not now"}, // SyncNotNow
    {L"Entre pelo celular", L"Sign in with your phone"}, // SyncCodeTitle
    {L"Abra este endereço no celular e digite o código.", L"Open this address on your phone and enter the code."}, // SyncCodeBody
    {L"Esperando você entrar…", L"Waiting for you to sign in…"}, // SyncWaiting
    {L"Conectado. Os saves vão sincronizar sozinhos.", L"Signed in. Saves will sync on their own."}, // SyncSignedIn
    {L"O acesso foi recusado.", L"Access was denied."}, // SyncDenied
    {L"O código expirou. Tente de novo em Configurações.", L"The code expired. Try again in Settings."}, // SyncExpired
    {L"Não deu para entrar. O arquivo de diagnóstico tem o motivo.", L"Sign-in failed. The diagnostics file has the reason."}, // SyncLoginFailed
    {L"Falta o arquivo savesync.json, com o cliente OAuth, na pasta do app. Sem ele a sincronização não funciona. Você pode ligá-la depois em Configurações.", L"The savesync.json file with the OAuth client is missing from the app folder. Sync cannot work without it. You can turn it on later in Settings."}, // SyncNotConfiguredBody
    {L"SwitchSaveSync", L"SwitchSaveSync"}, // SyncRowTitle
    {L"Desligado", L"Off"}, // SyncStateOff
    {L"Conectado", L"Signed in"}, // SyncStateOn
    {L"Não configurado", L"Not configured"}, // SyncStateNotConfigured
    {L"Saves na nuvem, iguais aos do Switch. Pressione A para entrar.", L"Cloud saves, the same as on the Switch. Press A to sign in."}, // SyncRowOffHint
    {L"Os saves sincronizam ao abrir e fechar um jogo. Pressione A para sair da conta.", L"Saves sync when a game opens and closes. Press A to sign out."}, // SyncRowOnHint
    {L"Falta o arquivo savesync.json na pasta do app.", L"The savesync.json file is missing from the app folder."}, // SyncRowMissingHint
    {L"Fontes", L"Sources"}, // SourcesRowTitle
    {L"nenhuma", L"none"}, // SourcesRowState
    {L"Suas listas de jogos no formato Tinfoil. Pressione A para gerenciar.", L"Your own game lists in the Tinfoil format. Press A to manage them."}, // SourcesRowHint
    {L"Entrar", L"Sign in"}, // HintSignIn
    {L"Sair da conta", L"Sign out"}, // HintSignOut
    {L"Cancelar", L"Cancel"}, // HintCancel
    {L"Continuar", L"Continue"}, // HintContinue
    {L"Sincronizando os saves", L"Syncing saves"}, // SyncingTitle
    {L"Enviando o save do último jogo", L"Uploading the last game's save"}, // SyncingAfterTitle
    {L"Conectando ao Google Drive", L"Connecting to Google Drive"}, // SyncStageConnecting
    {L"Comparando os saves", L"Comparing the saves"}, // SyncStageComparing
    {L"Baixando da nuvem", L"Downloading from the cloud"}, // SyncStageDownloading
    {L"Aplicando o save", L"Applying the save"}, // SyncStageApplying
    {L"Enviando para a nuvem", L"Uploading to the cloud"}, // SyncStageUploading
    {L"Limpando a nuvem", L"Tidying the cloud"}, // SyncStageCleaning
    {L"Pronto", L"Done"}, // SyncStageFinished
    {L"Os saves são diferentes", L"The saves differ"}, // SyncConflictTitle
    {L"O save do Xbox e o da nuvem mudaram desde a última sincronização. Qual você quer manter? O outro é substituído.", L"The Xbox save and the cloud save both changed since the last sync. Which one do you want to keep? The other is replaced."}, // SyncConflictBody
    {L"Manter o save do Xbox", L"Keep the Xbox save"}, // SyncKeepXbox
    {L"Usar o da nuvem", L"Use the cloud save"}, // SyncKeepCloud
    {L"Xbox", L"Xbox"}, // SyncSideXbox
    {L"Nuvem", L"Cloud"}, // SyncSideCloud
    {L"arquivos", L"files"}, // SyncFilesSuffix
    {L"Save enviado para a nuvem.", L"Save uploaded to the cloud."}, // SyncResultUploaded
    {L"Save da nuvem aplicado.", L"Cloud save applied."}, // SyncResultDownloaded
    {L"Os saves já estão iguais.", L"The saves already match."}, // SyncResultUpToDate
    {L"Ainda não há save deste jogo.", L"There is no save for this game yet."}, // SyncResultNothing
    {L"A conta saiu. Entre de novo em Configurações.", L"The account signed out. Sign in again in Settings."}, // SyncResultNotSignedIn
    {L"A sincronização falhou. O jogo abre mesmo assim.", L"Sync failed. The game starts anyway."}, // SyncResultFailed
    {L"A sincronização não está configurada.", L"Sync is not configured."}, // SyncResultNotConfigured
    {L"Jogar sem sincronizar", L"Play without syncing"}, // SyncSkip
    {L"O código vale por", L"The code is valid for"}, // SyncExpiresIn
    {L"Atualização disponível", L"Update available"},    // UpdateAvailable
        {L"Baixando atualização", L"Downloading update"},    // UpdateDownloading
        {L"Instalando. O app será reiniciado.",
         L"Installing. The app will restart."}, // UpdateInstalling
        {L"Atualização enviada ao console. O app será reiniciado.",
         L"Update sent to the console. The app will restart."}, // UpdateRestarting
        {L"Não foi possível atualizar. Verifique o Device Portal e pressione A para tentar de novo "
         L"ou B para voltar.",
         L"Could not update. Check Device Portal and press A to retry or B to go back."}, // UpdateFailed
        {L"Falta portal.json na pasta LocalState, com host, port, user e pass do Device Portal. "
         L"Adicione o arquivo e pressione A para tentar de novo.",
         L"portal.json is missing from LocalState, with the Device Portal host, port, user and "
         L"pass. Add the file and press A to retry."}, // UpdateMissingPortal
        {L"Atualizar", L"Update"},                     // UpdateAction
        {L"Pendrive detectado", L"USB drive detected"}, // UsbDetected
        {L"Pendrive", L"USB drive"},                    // UsbSetting
        {L"Não escolhido (pergunta no próximo drive)",
         L"Not chosen yet (asks on the next drive)"},                            // UsbModeUnset
        {L"Perguntar sempre", L"Ask every time"},                                // UsbModeAsk
        {L"Copiar jogos para o SSD interno", L"Copy games to the internal SSD"}, // UsbModeCopy
        {L"Usar como SSD externo", L"Use as an external SSD"},                   // UsbModeExternal
        {L"Não fazer nada", L"Do nothing"},                                      // UsbModeOff
        {L"Agora não", L"Not now"},                                              // UsbLater
        {L"A ação escolhida será repetida sempre que este drive for conectado. Mude em "
         L"Configurações > Pendrive, inclusive para perguntar sempre. Agora não mantém a "
         L"configuração atual.",
         L"The chosen action will run every time this drive is connected. Change it in Settings > "
         L"USB drive, including asking every time. Not now keeps the current setting."}, // UsbRepeat
        {L"Perguntar sempre está ativo: esta escolha vale só agora. Você verá esta pergunta a cada "
         L"conexão. Mude em Configurações > Pendrive.",
         L"Ask every time is on: this choice runs once. You will see this question on every "
         L"connection. Change it in Settings > USB drive."}, // UsbRepeatAsk
        {L"A muda a ação ao conectar o drive.",
         L"A changes the action when a drive connects."},                    // UsbSettingHint
        {L"jogos fora de NXbox\\games", L"games outside NXbox\\games"},      // UsbDetectedGames
        {L"chaves fora de NXbox\\games", L"key files outside NXbox\\games"}, // UsbDetectedKeys
        {L"Espaço livre desconhecido", L"Free space unknown"},               // UsbSpaceUnknown
        {L"jogos não couberam: use o drive como SSD externo.",
         L"games did not fit: use the drive as an external SSD."}, // UsbDidNotFit
        {L"Não foi possível salvar a preferência do pendrive.",
         L"Could not save the USB drive preference."},                    // UsbSaveFailed
        {L"Importar do pendrive", L"Import from USB drive"}, // UsbTitle
        {L"Procurando nos drives...", L"Looking through your drives..."}, // UsbScanning
        {L"Nenhum drive removível encontrado. Conecte um pendrive ou HD formatado em exFAT ou NTFS com seus dumps e pressione A para procurar de novo.", L"No removable drive found. Plug in a USB drive formatted exFAT or NTFS with your dumps, then press A to scan again."}, // UsbNoDrive
        {L"Nenhum jogo ou chave nos drives conectados. Coloque arquivos .nsp, .nsz, .xci, .xcz ou prod.keys no drive e pressione A para procurar de novo.", L"No games or keys on the connected drives. Put .nsp, .nsz, .xci, .xcz or prod.keys files on the drive, then press A to scan again."}, // UsbEmpty
        {L"Xbox", L"Xbox"}, // UsbXbox
        {L"livres", L"free"}, // UsbFree
        {L"Copiar tudo", L"Copy all"}, // UsbCopyAll
        {L"Copiar para o Xbox", L"Copy to the Xbox"}, // UsbCopyToXbox
        {L"Jogar do pendrive", L"Play from the drive"}, // UsbPlayFromDrive
        {L"Copiar usa o espaço do Xbox. Jogar do pendrive move o arquivo para NXbox\\games no próprio drive, sem usar espaço do console.", L"Copying uses the Xbox's storage. Playing from the drive moves the file into NXbox\\games on the same drive and uses none of the console's space."}, // UsbChoiceBody
        {L"Chaves", L"Keys"}, // UsbKeysTag
        {L"Sem espaço no Xbox para este arquivo.", L"Not enough space on the Xbox for this file."}, // UsbNoSpace
        {L"Precisa de", L"Needs"}, // UsbNeeds
        {L"Copiando", L"Copying"}, // UsbCopying
        {L"Movendo", L"Moving"}, // UsbMoving
        {L"Cópia cancelada", L"Copy cancelled"}, // UsbCancelled
        {L"ok", L"done"}, // UsbSummaryOk
        {L"falharam", L"failed"}, // UsbSummaryFailed
        {L"sem espaço", L"no space"}, // UsbSummaryNoSpace
        {L"Movido. O jogo já aparece na biblioteca.", L"Moved. The game now shows up in the library."}, // UsbMoved
        {L"Não foi possível mover o arquivo.", L"Could not move the file."}, // UsbMoveFailed
        {L"Nada cabe no Xbox. Use Jogar do pendrive.", L"Nothing fits on the Xbox. Use Play from the drive."}, // UsbNothingFits
        {L"Importar", L"Import"}, // HintImport
        {L"fonte", L"source"}, // SourcesUnitOne
        {L"fontes", L"sources"}, // SourcesUnitMany
        {L"Endereços de listas no formato da loja Tinfoil, escolhidos por você.", L"Addresses of lists in the Tinfoil shop format, chosen by you."}, // SrcSubtitle
        {L"Adicionar fonte", L"Add a source"}, // SrcAdd
        {L"Digite o endereço da lista com o teclado na tela.", L"Type the list address with the on-screen keyboard."}, // SrcAddHint
        {L"Importar sources.txt do USB", L"Import sources.txt from USB"}, // SrcImportUsb
        {L"Um endereço por linha, ou nome|endereço. Na raiz do pendrive ou em switch\\.", L"One address per line, or name|address. At the drive root or in switch\\."}, // SrcImportHint
        {L"Nenhuma fonte ainda. Adicione o endereço de uma fonte sua.", L"No sources yet. Add the address of a source of your own."}, // SrcEmpty
        {L"Endereço da fonte (http ou https)", L"Source address (http or https)"}, // SrcEnterUrl
        {L"O endereço precisa começar com http:// ou https://.", L"The address must start with http:// or https://."}, // SrcInvalidUrl
        {L"Essa fonte já está na lista.", L"That source is already in the list."}, // SrcDuplicate
        {L"Fonte adicionada.", L"Source added."}, // SrcAdded
        {L"importada(s)", L"imported"}, // SrcImported
        {L"Nenhum sources.txt com endereços novos foi achado nos pendrives.", L"No sources.txt with new addresses was found on the drives."}, // SrcImportNone
        {L"Procurando sources.txt nos pendrives…", L"Looking for sources.txt on the drives…"}, // SrcImporting
        {L"Remover esta fonte?", L"Remove this source?"}, // SrcRemoveTitle
        {L"O endereço sai da lista. O que já foi baixado não é apagado.", L"The address leaves the list. Anything already downloaded stays."}, // SrcRemoveBody
        {L"Remover", L"Remove"}, // SrcRemove
        {L"Fonte removida.", L"Source removed."}, // SrcRemoved
        {L"Carregando a lista…", L"Loading the list…"}, // SrcLoading
        {L"Esse formato de fonte não é suportado. A lista precisa ser um JSON da loja Tinfoil, sem criptografia.", L"This source format is not supported. The list must be an unencrypted Tinfoil shop JSON."}, // SrcUnsupported
        {L"Não deu para abrir a fonte. Confira o endereço e a rede.", L"Could not open the source. Check the address and the network."}, // SrcUnreachable
        {L"Essa fonte não lista nenhum arquivo.", L"This source lists no files."}, // SrcNoFiles
        {L"Jogo", L"Game"}, // SrcTypeGame
        {L"Atualização", L"Update"}, // SrcTypeUpdate
        {L"DLC", L"DLC"}, // SrcTypeDlc
        {L"itens", L"items"}, // SrcItems
        {L"Baixando", L"Downloading"}, // SrcDownloading
        {L"Baixado. Ele aparece na biblioteca.", L"Downloaded. It shows up in the library."}, // SrcDownloadDone
        {L"O download falhou. Sem espaço ou sem conexão; tente de novo.", L"The download failed. No space or no connection; try again."}, // SrcDownloadFailed
        {L"Download cancelado. O que já veio fica para continuar depois.", L"Download cancelled. What arrived is kept to resume later."}, // SrcDownloadCancelled
        {L"Baixar", L"Download"}, // HintDownload
        {L"Remover", L"Remove"}, // HintRemove
        {L"Digitar", L"Type"}, // HintType
        {L"Maiús", L"Shift"}, // KeyShift
        {L"Apagar", L"Delete"}, // KeyBackspace
        {L"OK", L"OK"}, // KeyOk
        {L"Recarregar", L"Reload"}, // HintReload
        {L"Cheats",
         L"Cheats"}, // CheatsTitle
        {L"Cheats da comunidade para este jogo. Valem na próxima vez que o jogo abrir.",
         L"Community cheats for this game. They apply the next time the game opens."}, // CheatsSourceLine
        {L"Banco de cheats: nx-cheats-db, de sthetix, via CNX Updater, de CostelaCNX (GPL-3.0)",
         L"Cheats database: nx-cheats-db by sthetix, via CNX Updater by CostelaCNX (GPL-3.0)"}, // CheatsCredit
        {L"Buscando os cheats deste jogo…",
         L"Looking for this game's cheats…"}, // CheatsLoading
        {L"O banco de cheats não tem nada para este jogo.",
         L"The cheats database has nothing for this game."}, // CheatsNone
        {L"Sem conexão com o banco de cheats. Pressione A para tentar de novo.",
         L"Cannot reach the cheats database. Press A to try again."}, // CheatsOffline
        {L"Não deu para ler o banco de cheats. Pressione A para tentar de novo.",
         L"The cheats database could not be read. Press A to try again."}, // CheatsFailed
        {L"Cada versão do jogo tem um build ID próprio. Ligue os cheats na versão que você joga; na dúvida, ligue em todas.",
         L"Each game version has its own build ID. Turn cheats on for the version you play; if unsure, turn them on in every one."}, // CheatsBuildHint
        {L"Ligado",
         L"On"}, // CheatOn
        {L"Desligado",
         L"Off"}, // CheatOff
        {L"Cheats",
         L"Cheats"}, // HintCheats
        {L"Ligar todos",
         L"All on"}, // HintAllOn
        {L"Desligar todos",
         L"All off"}, // HintAllOff
        {L"Cheat ligado",
         L"Cheat on"}, // ToastCheatOn
        {L"Cheat desligado",
         L"Cheat off"}, // ToastCheatOff
        {L"Créditos",
         L"Credits"}, // CreditsRowTitle
        {L"Ver",
         L"View"}, // CreditsRowState
        {L"Quem fez o que o NXbox usa.",
         L"Who made what NXbox builds on."}, // CreditsRowHint
        {L"Créditos",
         L"Credits"}, // CreditsTitle
        {L"O NXbox é um fork do Eden, um emulador de Switch de código aberto (GPL-3.0), levado para o Xbox Series X.\n"
         L"\n"
         L"Cheats: o banco nx-cheats-db, de sthetix, baixado quando você abre a tela e que pertence aos seus autores. Chegamos a ele pelo mesmo caminho do CNX Updater, de CostelaCNX (GPL-3.0, github.com/CostelaCNX/CNX-Updater).\n"
         L"\n"
         L"O CNX Updater é um fork do AIO-Switch-Updater, de HamletDuFromage (GPL-3.0), feito com o Borealis, de natinusala. O conjunto de recursos deles inspirou os downloads e os cheats do NXbox.\n"
         L"\n"
         L"Os avisos completos estão no arquivo NOTICE.md do repositório.",
         L"NXbox is a fork of Eden, an open-source Nintendo Switch emulator (GPL-3.0), brought to the Xbox Series X.\n"
         L"\n"
         L"Cheats: the nx-cheats-db database by sthetix, downloaded when you open the screen and owned by its authors. We reach it the same way CNX Updater by CostelaCNX does (GPL-3.0, github.com/CostelaCNX/CNX-Updater).\n"
         L"\n"
         L"CNX Updater is a fork of AIO-Switch-Updater by HamletDuFromage (GPL-3.0), built on Borealis by natinusala. Their feature set inspired the downloads and cheats in NXbox.\n"
         L"\n"
         L"The full notices are in NOTICE.md in the repository."}, // CreditsBody
        {L"Procurando o jogo", L"Looking up the game"},            // LaunchLookup
        {L"Carregando chaves", L"Loading keys"},                   // LaunchKeys
        {L"Preparando shaders", L"Building shaders"},              // LaunchShaders
        {L"Iniciando", L"Starting"},                               // LaunchStarting
        {L"Jogo indisponível. Conecte a unidade ou abra o NXbox para conferir o jogo e as chaves. "
         L"Pressione B para sair.",
         L"Game unavailable. Connect the drive or open NXbox to check the game and keys. Press B "
         L"to exit."}, // LaunchMissing
        {L"Não foi possível iniciar. Abra o NXbox para conferir o jogo e as chaves. Pressione B "
         L"para sair.",
         L"Could not start. Open NXbox to check the game and keys. Press B to exit."}, // LaunchFailed
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
