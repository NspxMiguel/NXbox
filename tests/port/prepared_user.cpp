// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <random>
#include "common/fs/path_util.h"
#include "core/hle/service/acc/offline_account.h"
#include "core/hle/service/mii/mii_manager.h"
#include "core/hle/service/mii/types/char_info.h"
#include "core/hle/service/mii/types/store_data.h"

// Only host UUID/random providers and file/log adapters are substituted. Mii data, validation,
// checksums, manager and serialization execute the production code.
namespace Common {
UUID UUID::MakeRandomRFC4122V4() {
    static u8 sequence = 0;
    UUID id = MakeDefault();
    id.uuid[0] = ++sequence;
    return id;
}
namespace Random {
std::mt19937 GetMT19937() noexcept {
    return std::mt19937{42};
}
} // namespace Random
} // namespace Common

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace Service::Account::Offline;
    assert(IsFakeLinkEnabled() == FakeLinkEnabled(std::getenv("NXBOX_FAKE_NA_LINK"), true));
    assert(FakeLinkEnabled(nullptr, true));
    assert(!FakeLinkEnabled(nullptr, false));
    assert(!FakeLinkEnabled("0", true));
    assert(FakeLinkEnabled("1", false));
    assert(FakeLinkEnabled("", true));
    assert(AccountId(0) == 1);
    assert(AccountId(1234) == 1234);
    assert(SelectProfileIndex(-1, 3) == 0);
    assert(SelectProfileIndex(2, 3) == 2);
    assert(SelectProfileIndex(3, 3) == 0);
    assert(SelectProfileIndex(99, 8) == 7);
    assert(SelectProfileIndex(99, 1) == 0);

    using namespace Service::Mii;
    Common::FS::nand_path = argv[1];
    const auto db_path = Common::FS::nand_path / "system/save/8000000000000030/MiiDatabase.dat";
    MiiManager manager;
    DatabaseSessionMetadata first{}, second{};
    assert(manager.Initialize(first).IsSuccess());
    assert(manager.GetCount(first, SourceFlag::Database) == 1);
    assert(manager.GetCount(first, SourceFlag::Default) == 6);
    assert(manager.GetCount(first, SourceFlag::Database | SourceFlag::Default) == 7);
    assert(!manager.IsUpdated(first, SourceFlag::Database));
    std::array<CharInfoElement, 1> entries{};
    u32 count = 0;
    assert(manager.Get(first, entries, count, SourceFlag::Database).IsSuccess());
    assert(count == 1);
    const auto original = entries[0].char_info;
    assert(original.Verify() == ValidationResult::NoErrors);
    assert(original.GetCreateId().IsValid());
    assert((original.GetNickname().data == Nickname{u'P', u'l', u'a', u'y', u'e', u'r'}.data));
    assert(std::filesystem::file_size(db_path) == sizeof(NintendoFigurineDatabase));

    assert(manager.Initialize(second).IsSuccess());
    assert(!manager.IsUpdated(first, SourceFlag::Database));
    StoreData custom{};
    custom.BuildDefault(1);
    custom.SetNickname({u'C', u'u', u's', u't', u'o', u'm'});
    custom.SetChecksum();
    assert(custom.IsValid() == ValidationResult::NoErrors);
    assert(manager.AddOrReplace(second, custom).IsSuccess());
    assert(!manager.IsUpdated(first, SourceFlag::Default));
    assert(manager.IsUpdated(first, SourceFlag::Database));
    assert(!manager.IsUpdated(first, SourceFlag::Database));
    assert(manager.GetCount(first, SourceFlag::Database) == 2);

    MiiManager reloaded;
    DatabaseSessionMetadata reload{};
    assert(reloaded.Initialize(reload).IsSuccess());
    assert(reloaded.GetCount(reload, SourceFlag::Database) == 2);
    count = 0;
    assert(reloaded.Get(reload, entries, count, SourceFlag::Database).IsError()); // Too small.
    std::array<CharInfoElement, 2> all{};
    count = 0;
    assert(reloaded.Get(reload, all, count, SourceFlag::Database).IsSuccess());
    assert(all[0].char_info.GetCreateId() == original.GetCreateId());
    assert(all[1].char_info.GetCreateId() == custom.GetCreateId());
    assert(all[1].char_info.GetNickname().data == custom.GetNickname().data);

    // A valid persisted empty database also seeds once, keeping the same UUID on reopening.
    assert(reloaded.Format(reload).IsSuccess());
    MiiManager empty;
    DatabaseSessionMetadata empty_metadata{};
    assert(empty.Initialize(empty_metadata).IsSuccess());
    assert(empty.GetCount(empty_metadata, SourceFlag::Database) == 1);
    assert(empty.Initialize(empty_metadata).IsSuccess());
    assert(empty.GetCount(empty_metadata, SourceFlag::Database) == 1);

    // Preserve a database with just an existing custom Mii; no default should be appended.
    assert(empty.AddOrReplace(empty_metadata, custom).IsSuccess());
    count = 0;
    assert(empty.Get(empty_metadata, all, count, SourceFlag::Database).IsSuccess());
    assert(empty.Delete(empty_metadata, all[0].char_info.GetCreateId()).IsSuccess());
    MiiManager custom_only;
    DatabaseSessionMetadata custom_metadata{};
    assert(custom_only.Initialize(custom_metadata).IsSuccess());
    assert(custom_only.GetCount(custom_metadata, SourceFlag::Database) == 1);
    count = 0;
    assert(custom_only.Get(custom_metadata, entries, count, SourceFlag::Database).IsSuccess());
    assert(entries[0].char_info.GetCreateId() == custom.GetCreateId());

    // Corrupt files must not be replaced by default data before the guest requests recovery.
    Common::FS::nand_path = std::filesystem::path(argv[1]) / "corrupt";
    const auto corrupt_db = Common::FS::nand_path / "system/save/8000000000000030/MiiDatabase.dat";
    std::filesystem::create_directories(corrupt_db.parent_path());
    std::ofstream(corrupt_db, std::ios::binary) << "broken";
    MiiManager corrupt;
    DatabaseSessionMetadata corrupt_metadata{};
    assert(corrupt.Initialize(corrupt_metadata).IsError());
    assert(std::filesystem::file_size(corrupt_db) == 6);
    assert(corrupt.GetCount(corrupt_metadata, SourceFlag::Database) == 0);
    assert(corrupt.IsBrokenWithClearFlag(corrupt_metadata));
    assert(corrupt.Initialize(corrupt_metadata).IsSuccess());
    assert(corrupt.GetCount(corrupt_metadata, SourceFlag::Database) == 1);

    // Failure to mount NAND must propagate, rather than reporting a prepared empty database.
    Common::FS::nand_path = std::filesystem::path(argv[1]) / "blocked";
    std::ofstream(Common::FS::nand_path) << "not a directory";
    MiiManager blocked;
    DatabaseSessionMetadata blocked_metadata{};
    assert(blocked.Initialize(blocked_metadata).IsError());
}
