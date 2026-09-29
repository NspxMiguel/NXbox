// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace EdenXbox {

/// Shared shader cache: NXBOX_SHADER_SHARE_URL (LocalState\nxbox_env.txt) names a server holding one
/// OpenGL pipeline cache per title. Before the local cache is loaded, the shared one is downloaded
/// and its new pipelines are merged in; at the end of the session the merged cache is uploaded, so
/// every console starts with the shaders any console has already met.
void DownloadSharedShaderCache(std::uint64_t title_id);
void UploadSharedShaderCache(std::uint64_t title_id);

} // namespace EdenXbox
