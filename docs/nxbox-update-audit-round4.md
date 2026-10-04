# NXbox update-path audit — round 4

The working-tree patch fixes a demonstrated **updated ExeFS + base RomFS** selection defect. It does not establish that the games now boot: no build, test execution, emulator run, commit, or push was performed. The new checks are intended to validate the actual patched mount on the next console run.

## Ranked findings

1. **The Program RomFS update was never mounted.** Before this patch, `src/core/file_sys/romfs_factory.cpp:42` recovered the base NCA exclusively through `content_provider.GetEntry(current_process_title_id, Program)`. UWP creates a fresh provider union (`src/eden_uwp/game_session.cpp:424`) without registering the loaded base in a frontend game-list provider. External scanning deliberately passes `only_content=true` (`src/core/file_sys/registered_cache.cpp:294`); the filter at `:238` admits only Update and AOC, and `ExternalContentProvider::GetEntryRaw` at `:1466` only serves those categories. Thus a base loaded directly from NSP/XCI is absent from this union. `AppLoader_NCA::ReadRomFS` supplies the original file regardless. With `base_nca == nullptr`, both update branches and the base-verification branch in the old `PatchRomFS` were skipped, leaving only `FINAL`. ExeFS selection independently opens the external update without requiring a base. This explains the version mismatch and verifier silence together. **Fix:** `src/core/loader/nca.cpp:108` passes its shared NCA to the factory; `src/core/file_sys/romfs_factory.cpp:50` prefers that exact loaded NCA, retaining its lifetime and context. NSP and XCI both delegate their NCA loading to this loader.

2. **The four “RomFS: Update” successes were not evidence of a Program update.** In `sm3dw-eden.txt:144–145` and `botw-eden.txt:155–156`, the only `type=01` call goes straight to `FINAL`, with no update success between them. The other successes surround Control/NACP loading, whose call path is `src/core/file_sys/patch_manager.cpp:1199`. Their missing “Patching RomFS” prefixes are consistent with Control's debug-level logging. The patch adds `type` to success messages and logs `ROMFS_BASE`, `ROMFS_SELECT`, and `ROMFS_DATA`. The old output therefore does **not** show an update being applied four times to the program, nor demonstrate faulty BKTR reads. The observed zero-read BKTR instances include discovery/ExeFS-only mounts destroyed without serving guest RomFS.

3. **Verifier lifetime/policy could independently lose context.** The old NCA constructor only created its verifier when `getenv` was enabled at construction. This is a real latent issue for pre-scanned NCAs, but does not explain the supplied absence of even `SKIP`: a call to `VerifyRomFS` would have logged `no_mount_context`. `src/core/file_sys/content_archive.cpp:130` now retains context unconditionally. `src/core/file_sys/patch_manager.cpp:744` keeps the selected update owner until after LayeredFS and passes the exact returned file to the check. Missing mounts and changed/unsupported files produce a failure/incomplete diagnostic, never a silent pass.

4. **BKTR corruption remains unproven; the next run can separate it from selection.** `src/core/file_sys/romfs_verify.cpp:43` decodes entry sets sequentially without production `Find`, `GetEntryList`, binary searches, or coalescing. It checks ordered offsets, set continuity and the exclusive end sentinel against production storage sizes. Reference reads resolve each relocation to the retained original section or patch section, then independently resolve patch subsections. `src/core/file_sys/fssystem/fssystem_aes_ctr_counter_extended_storage.cpp:228` constructs big-endian secure-value/generation/physical-counter bytes and generates the keystream with AES-ECB, bypassing `MakeIv`, the CTR decryptor and alignment wrappers. Probes cover edges, 16 deterministic random locations, both relocation sources, relocation/subsection boundaries, and section-relative 4 GiB boundaries. Mismatched IVFC blocks additionally compare raw, unmerged and reference reads. Original-storage decryption and the already-decrypted metadata buffers are shared with production; this is an independent relocation/subsection/patch-CTR oracle, not an independent dump/key validator.

5. **ExeFS and DLC are separate paths.** `src/core/file_sys/patch_manager.cpp:325` replaces the whole ExeFS directory with one update's ExeFS. The loader reloads `main.npdm` after that replacement (`src/core/loader/deconstructed_rom_directory.cpp:174`), then loads its modules from that directory. The supplied logs have `nso_patched=false`, all segment hashes passing, and no `EXEFS_LAYER` replacements. `src/core/file_sys/patch_manager.cpp:382` now logs every selected NPDM/NSO's NCA, numeric version, file identity versus the selected directory, and NSO build ID. Compare these NCA names with `ROMFS_SELECT selected=` and IDs with `FAULT MODULE`. DLC calls use the distinct title `01007EF00011F001` and `ContentRecordType::Data` (`02`), while the current-process path requests `Program` (`01`); provider lookup is keyed by both title and record type. DLC cannot replace the Program RomFS through this path. `ROMFS_DATA` makes those separate mounts explicit.

## Assessment of the proposed causes

| Candidate | Result |
| --- | --- |
| (a) Relocation/subsection lookup, >4 GiB, sentinel | No new proven defect. Reference lookup and boundary probes now test the live mount. |
| (b) CTR-Ex counter/generation | No console evidence yet. Independent ECB-based reference uses physical NCA offsets and subsection generation. |
| (c) IVFC/RomFS offset | Driver creates the data slice from the last IVFC level after relocation; it does not use a legacy `ivfc_offset`/`BuildRomFS` path. Reference comparison reads `raw[leaf_offset + romfs_offset]` against the exact received RomFS. `raw_same=false` isolates that slice/wrapper discrepancy. |
| (d) Base association | **Proven missing base association in UWP**, rather than a demonstrated Control/old-base pairing. The loader's original NCA is now retained. Constructor checks at `src/core/file_sys/content_archive.cpp:90` reject mismatched content type, title/program index, content index, or an already patched base. |
| (e) Provider order/double application | External version lists sort descending (`registered_cache.cpp:1515`); both active external branches choose the first enabled numeric version and retrieve its Program record. No evidence of an old version or double application in these logs. Non-versioned provider maps can reflect scan order, but the selected-version external path bypasses them. Same-version duplicates and NAND fallback policies are not redesigned here. |

## Automatic check and limits

For a selected Program update, `ROMFS_CHECK` runs without any environment switch. It authenticates the FS-header digest, every hash level above the data level, the first and last 2 MiB of data, and 512 distinct deterministic pseudo-random 4 KiB locations. Locations expand to complete IVFC blocks if the declared block size differs; overlapping blocks are deduplicated. Section-relative 4 GiB boundary blocks are added. Data is also reread unaligned. `blocks` includes the complete upper levels, so it can exceed the data sample count substantially.

The working buffers are about 3 MiB plus bounded retry buffers and decoded BKTR tables. Read volume includes **all upper hash levels**, not merely the roughly 6 MiB data sample; for a large game this can be tens or hundreds of MiB. No whole-game allocation is made. No-update launches keep the prior no-check behavior unless full verification is requested.

A check is reported once per selected NCA/base/header identity per UWP launch, including failures. A repeated mount reuses that attempt; it is not a claim that every later runtime read is authenticated. `ResetRomfsVerification` already runs before each launch. LayeredFS rebuilding changes offsets and invalidates this tree's relationship to the returned file, so it reports `FAIL reason=received_file_changed scope=unverified_received`. Sparse/compressed/unsupported layouts also report incomplete verification. These are diagnostic failures, not proof of corrupt game data; the diagnostic does not block launching.

`NXBOX_VERIFY_ROMFS=1` first produces the same sampled summary, then runs the full data-tree walk. The full walk is distinguished by `scan=full`; its `END` must have zero mismatches/read errors/shape mismatches and a valid header hash. A sampled `ok` alone is not a full-tree result. Leave `NXBOX_UPDATES=1` enabled for the diagnostic run; the default-off update policy was not changed.

## Reading the next log

| Line/field | Meaning |
| --- | --- |
| `ROMFS_BASE origin=loader present=true file_matches=true` | Exact NCA/file from the loaded base, not a missing or unrelated provider entry. |
| `ROMFS_SELECT type=01 provider=external applied=true` | Program update actually mounted. Require the intended `version`, a named base and selected NCA, `base_is_update=false`, and `layeredfs_changed=false`. |
| `EXEFS_SOURCE same_selected_file=true` | This final NPDM/NSO came from the selected ExeFS directory. Its `nca` should equal `ROMFS_SELECT selected`. A false value identifies a LayeredExeFS replacement. |
| `ROMFS_CHECK ok ... source=relocated scan=sample` | Header, all upper levels, sampled received data, unaligned rereads, and reference probes passed. `bad=0`; `first_bad_offset=none`. `source=update` means a replacement without relocation; `source=base` means a base check. |
| `ROMFS_CHECK FAIL` | Inspect `reason`, `first_bad_level`, `read_errors`, `shape_bad`, and `reference_bad`. `first_bad_offset` is a patched-section byte offset, not a physical NCA offset. `bad` counts failed checks, including layout/header failures, and is not a count of unique corrupt data blocks. |
| `ROMFS_REFERENCE ok ... base_spans=... update_spans=...` | Both lookup/read paths agreed for the reported probes. Span counts reveal which physical sources were exercised. |
| `ROMFS_REFERENCE DIFF raw_same=false` | Received RomFS differs from its raw-section slice: investigate offset/wrapper/read-shape handling. |
| `ROMFS_REFERENCE DIFF raw_same=true manual_same=false` | Production raw path differs from flat relocation/subsection/ECB resolution: investigate production lookup, coalescing, alignment or CTR. This line alone does not prove the reference is right; correlate IVFC hashes. |
| `MISMATCH ... reference_matches=true` | Independent resolution matches the expected IVFC digest while production does not. `unmerged_matches=true` further points to relocation coalescing. |
| `MISMATCH ... reference_same=true reference_matches=false` | Both paths agree on bad bytes. Check mapping source, expected-hash ancestors and dump/base/key association; do not assume a lookup defect. |
| `NCA_PAIR FAIL`, `ROMFS_SELECT_ERROR`, missing-base `ROMFS_CHECK FAIL` | Selection/mount failed before successful verification. |
| `ROMFS_DATA type=02` | Separate data/DLC mount; not the current process's Program RomFS. |

The existing build IDs are comparison baselines, not independently authenticated release manifests:

- SM3DW main: `635FE25C7CB69847F41B49F19CD9AA8942BF1B50000000000000000000000000`, version `0x40000`.
- BotW main: `CD57B23FA4BBAD65803D9788C01821EE00000000000000000000000000000000`, version `0x110000`.
- BotW subsdk0: `F39F4C431234C72EB520CFDD1001BBCF02B3E885000000000000000000000000`.

Save the next console Eden log as `next-eden.txt`, then run:

```sh
grep -E 'NXBOX (ROMFS_|NCA_PAIR|EXEFS_SOURCE|VERIFY_ROMFS|BKTR|FAULT (EXEFS_UPDATE|EXEFS_LAYER|NPDM|NSO_SEGMENT|MODULE))|Patching RomFS|RomFS: Update' next-eden.txt
```

With no mods, the decisive chain is loader base present → intended Program update applied → matching ExeFS/ROMFS NCA → sampled `ROMFS_CHECK ok source=relocated`. If the game still fails after that, use full-tree output to distinguish unsampled content errors from later runtime/guest-memory behavior.

## Validation

Changed C++ regions were formatted using Xcode's `clang-format` with `src/.clang-format`; `git diff --check` passed. The existing synthetic CTR test now includes unaligned reference reads on both sides of a generation/4 GiB counter boundary and an out-of-range read (`src/tests/core/file_sys/bktr.cpp`). Tests were **not executed**. Compilation, console timings, returned game frames and actual reference/hash results remain unverified, as requested.
