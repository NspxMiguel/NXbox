# NXbox BotW update RomFS audit — round 3

The working-tree patch fixes demonstrable read-path defects, but does **not** establish which one caused the console fault. No build, emulator run, commit, or push was performed. The supplied logs confirm update selection and the fault; they contain no IVFC evidence yet.

## Ranked findings and fixes

Paths below are relative to `src/`.

1. **Failed/short reads were reported as successful.** `core/file_sys/fssystem/fssystem_indirect_storage.cpp:115` discarded both the underlying byte count and `OperatePerEntry`'s error. `fssystem_aes_ctr_counter_extended_storage.cpp:157`, `fssystem_aes_ctr_storage.cpp:41`, and `fssystem_alignment_matching_storage_impl.cpp:31` also discarded byte counts. These could expose untouched or undeciphered bytes to the game. The patch propagates failure as zero bytes, validates subsection traversal with normal returns, and checks table reads (`fssystem_bucket_tree.cpp:198`, `fssystem_bucket_tree_template_impl.h:50`, `fssystem_nca_file_system_driver.cpp:858` and `:981`). Partial coalesced reads cannot safely claim a valid prefix. A short read has **not** been observed on the console.
2. **Shared AES contexts could race.** `fssystem_aes_ctr_storage.cpp:60` now locks `SetIV` and `Transcode` together; writes share that lock. `fssystem_aes_xts_storage.cpp:57` now uses its previously unused mutex. File-handle locking ends before decryption and did not protect these contexts. CTR-Ex already creates a local cipher per subsection. Concurrent corruption is possible in the old code, but the deterministic BotW failure does not establish it happened.
3. **A real length narrowing exists, not a demonstrated 4 GiB seek bug.** `core/crypto/aes_util.cpp:115` passed a whole `size_t` request to EVP as `int`. CTR/ECB now use bounded updates without resetting the stream between chunks. XTS retains one update per data unit. Normal small reads were unaffected; no console request above `INT_MAX` was demonstrated.
4. **Out-of-view reads could escape an offset slice.** `core/file_sys/vfs/vfs_offset.cpp:96` subtracted an out-of-range unsigned offset. The patch clamps safely and returns immediately for out-of-range `Read`. This is proven independently of BotW.
5. **IVFC integrity was not checked.** `fssystem_integrity_verification_storage.cpp:56` only reads/pads data. The new opt-in verifier checks the complete SHA-256 hierarchy, including the header's master hash and FS-header digest, without changing normal integrity policy.

## Read-path audit

`nca_patch.cpp` is absent in this upstream. The active chain is NCA driver → indirect relocation storage → base CTR / update CTR-Ex → alignment wrappers → offset VFS → RealVfsFile → IOFile. Relocation/subsection positions are signed 64-bit; VFS sizes/offsets are `size_t` (64-bit on Xbox x64), not universally `u64`. NCA sector conversion widens before shifting. CTR uses big-endian upper generation/secure value and the physical NCA byte position divided by 16; no proven counter truncation or key-selection error was found. The legacy `ctr_encryption_layer.cpp` / `xts_encryption_layer.cpp` are not this NCA driver's decryption layers.

`common/fs/file.cpp:33` maps seek/tell to `_fseeki64`/`_ftelli64` under MSVC, including the `CreateFileFromAppW` → `_open_osfhandle` → `_wfdopen` fallback. `ReadSpan` returns `fread`'s `size_t` item count; RealVfsFile passes bytes and retains its lock across seek/read. UWP size queries use `GetFileSizeEx`. No `long` seek or `SetFilePointer` truncation was found here. Stall scopes only record timing.

Concatenation uses 64-bit positions and propagates short reads. Layered VFS chooses files; it does not decrypt them. RomFS file/data offsets are 64-bit; 32-bit values identify metadata entries. LayeredFS rebuilding is bypassed when there are no mods. Compression/sparse variants are not claimed verified by this diagnostic.

## Next console run

Put `NXBOX_VERIFY_ROMFS=1` in `LocalState/nxbox_env.txt`, restart NXbox, and launch BotW with the update. Allow the full scan to finish: it reads the patched data twice, once aligned and once unaligned. Working buffers total about 3 MiB plus bounded retry storage; the full RomFS is never loaded into RAM. Progress is logged every five seconds. Remove the setting or set it to `0` afterwards.

Read these lines in the Eden log:

| Tag/fields | Meaning |
| --- | --- |
| `NXBOX VERIFY_ROMFS SELECT selected_version=0x110000` | Confirms the chosen update. |
| `BEGIN`, `LEVEL_BEGIN` | NCA/base identities, sizes, FS-header digest result, level offsets, actual block sizes. The data level is normally 6. |
| `MISMATCH` | First 16 bad blocks: level-relative and patched-section offsets, expected/actual hashes, read status and ancestor validity. All mismatches are still counted. |
| `SOURCE role=data/hash` and `CTR` | Maps the bad block and its expected digest to base/update physical section positions; update generation, NCA offset and low counter follow. Mapping details are bounded. |
| `raw_retry_matches=true` | The isolated raw block passes while the batch failed: investigate read shape, upper wrapper or transient corruption. |
| `raw_retry_matches=false unmerged_matches=true` | Disabling relocation coalescing repairs the block: isolates the optimization. |
| Both false, `read_ok=true ancestors_ok=true` | Persistent content discrepancy below the IVFC data layer. Source mapping narrows base versus update; this alone cannot distinguish a bad dump/base pairing from wrong decryption. |
| `IO_SHORT`, `BKTR READ_ERROR`, `MOUNT_ERROR` | Lower-level I/O, relocation/subsection traversal, or mounting failed. Includes offsets/counts or result codes. |
| `SHAPE_MISMATCH` | Aligned and unaligned reads of the same bytes disagree. |
| `END complete=true` | Exact totals for hash mismatches, block read errors and read-shape mismatches. Require all zero and `fs_header_hash_ok=true`. |
| `FINAL layeredfs_changed=true` | Mods rebuilt/replaced RomFS after IVFC verification; the original tree cannot authenticate those modified bytes. |
| `ABORT` / `SKIP` | Incomplete/unsupported verification, never a pass. |
| `CACHED` | An earlier successful scan of the same NCA/base/header in this launch is reused. The UWP launch hook resets this cache. |

`NXBOX BKTR kind=relocation/subsection` logs entry counts, reads, `crossing_reads` (calls visiting more than one entry), and failed reads, keyed by instance `id`. `verify_begin`/`verify_end` delimit the scan; progress and destruction report later totals. Counts include metadata, verification and retry traffic, not just guest requests. With the switch absent/zero, there is no scan or diagnostic atomic-counter traffic.

A clean complete scan with `layeredfs_changed=false` establishes correct startup bytes through this mount, across both read shapes. If the fault persists, investigate runtime reads, guest-memory transfer/decompression and DLC separately; this is not proof that every later guest buffer is correct.

## Validation and limits

Changed C++ regions/new files were formatted with clang-format; `git diff --check` passed. Five unexecuted synthetic tests in `tests/core/file_sys/bktr.cpp` cover relocation across 4 GiB, CTR generation/counter boundaries using an independently constructed ECB keystream, short-read propagation, EVP chunk continuity, and truncated bucket nodes. Target compilation and console validation remain outstanding.

The verifier uses ordinary read-only IVFC SHA-256 with zero-padding for a final partial block, consistent with [Atmosphère's integrity storage implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/source/fssystem/fssystem_integrity_verification_storage.cpp). It does not verify NCA RSA signatures, DLC, or mod-authored content. No keys or decrypted file contents are logged. Normal-mode changes affect error handling and cipher synchronization; their runtime cost/compatibility still needs the target run.
