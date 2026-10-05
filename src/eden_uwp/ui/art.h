// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace EdenXbox::Ui {

enum class BannerState {
    Pending, // not known yet: queued or being downloaded
    Ready,   // the JPEG is in LocalState\library
    None,    // offline, or the title has no banner: the screen keeps the icon gradient
};

// The official eShop banners of the hero (docs/nxbox-ui.md, Increment 3 art). The index maps a
// title ID to its banner and icon ids on Nintendo's CDN. It lives in the repository
// (dist/art/eshop-art.json) and is downloaded once into LocalState\library\eshop-art.json, then
// refreshed weekly (a failed refresh keeps the old copy). Each banner (1920x1080 JPEG) is cached in
// LocalState\library\<TITLEID>.banner.jpg, so a banner seen once also works offline.
//
// Everything runs on one worker thread with a WinRT HttpClient; the render thread only asks and
// polls. The newest request is served first, so scrolling through the shelf fetches what is on
// screen now before what was passed.
class BannerSource {
public:
    explicit BannerSource(std::filesystem::path local_state, bool fetch_icon = false);
    ~BannerSource(); // the worker finishes its current download in the background

    BannerSource(const BannerSource&) = delete;
    BannerSource& operator=(const BannerSource&) = delete;

    // Asks for a title's banner. Idempotent.
    void Request(const std::string& title_id);
    // Where the title's banner stands. When Ready, `file` is the cached JPEG.
    BannerState Get(const std::string& title_id, std::filesystem::path& file) const;
    // The file turned out not to be an image: forget it, so the screen falls back to the icon.
    void Discard(const std::string& title_id);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace EdenXbox::Ui
