// SPDX-FileCopyrightText: Copyright 2026 NXbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

// NSZ is an NSP (a PFS0 container) in which every compressed NCA is stored as an `.ncz` entry
// instead of an `.nca` entry. The format is produced and consumed by the reference tool "nsz"
// (https://github.com/nicoboss/nsz); the conversion below follows its decompressor (Decompressor.py
// and the Fs/Pfs0.py writer), and lays out the PFS0 header like `nsz -D` does without
// `--fix-padding`.
//
// An NCZ holds the first 0x4000 bytes of the original NCA verbatim, a table of the NCA's crypto
// sections, and the rest of the NCA decrypted and compressed with zstd (either as one zstd stream
// or as independent zstd blocks, announced by an NCZBLOCK header).
//
// TODO: `.xcz` -> `.xci` is not supported. An XCZ is an HFS0 root partition with nested HFS0
// partitions, so a conversion also has to rebuild every HFS0 header, the per-file hash regions, the
// per-partition header hashes and the XCI card header (RootPartitionHeaderHash, valid data end,
// card size). That is a separate piece of work and does not share the PFS0 writer below. Only the
// NCZ -> NCA stream conversion in nsz.cpp (NczPayloadReader and Converter::ConvertNcz) carries
// over, once it is moved out of that file.

/// Returns true when `file` is a PFS0 container that holds at least one `.ncz` entry.
/// Only the container's header and string table are read.
[[nodiscard]] bool IsNsz(const VirtualFile& file);

/**
 * Converts an NSZ into a regular NSP, streaming: no NCA is ever held in memory, only a few MiB of
 * buffers plus the zstd window of the stream (at most 128 MiB, which is zstd's default limit; a
 * frame that needs more is rejected) are used, no matter how large the game is.
 *
 * The output is a PFS0 with the same entries in the same order as the input, except that every
 * `name.ncz` entry becomes `name.nca` with its decompressed size. The PFS0 header keeps the input's
 * string table size and data start, like `nsz -D` without `--fix-padding`. Every other entry is
 * copied byte for byte.
 *
 * The decompressed NCAs are verified the way nsz does it: the first 16 bytes of the SHA-256 of an
 * NCA are its NCA ID, which is the file name of the entry. A mismatch is logged as a warning (the
 * source is probably corrupt) and does not fail the conversion.
 *
 * @param nsz       The NSZ to read. Must be readable and must stay valid for the whole call.
 * @param out_nsp   The file the NSP is written to, which must be writable and must not be `nsz`.
 *                  It is truncated first and then written sequentially from the start. After a
 *                  failure its contents are undefined: the caller should delete it.
 * @param progress  Optional. Called with the number of bytes written so far and the size of the
 *                  complete NSP: first with 0, then about every 8 MiB, and last with the size of
 *                  the NSP when the conversion is done. Runs on the calling thread.
 * @param error     Optional. Receives a short description of the failure when false is returned.
 * @param cancel    Optional. Checked between chunks; when it becomes true the conversion stops and
 *                  returns false with the error "cancelled".
 * @return true when `out_nsp` holds the complete NSP.
 *
 * This blocks until the conversion has finished; call it from a worker thread.
 */
[[nodiscard]] bool ConvertNszToNsp(const VirtualFile& nsz, const VirtualFile& out_nsp,
                                   std::function<void(u64 done, u64 total)> progress,
                                   std::string* error, const std::atomic<bool>* cancel = nullptr);

} // namespace FileSys
