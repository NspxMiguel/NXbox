// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {

// An on-screen keyboard drawn by us, driven by the controller: a grid of keys for letters, digits
// and the symbols of a URL, a Shift key, shortcut keys for "http://", "https://" and ".com", a
// backspace key and OK / Cancel. Nothing here touches the system keyboard.
//
//   D-pad / stick  move over the keys     A  press the key      B  backspace (cancels when empty)
//   X or Menu      confirm                Y  Shift              LB / RB  move the caret
//
// The caller owns the loop: call Update() once per frame after Input::Update(), then draw the
// frame with Draw() between BeginFrame() and EndFrame() (it paints its own veil, sheet and hints).
class TextEntry {
public:
    enum class Result { Editing, Confirmed, Cancelled };

    TextEntry(std::wstring title, std::wstring initial);

    Result Update(const Input& input);
    void Draw(Renderer& renderer, Clock::time_point now) const;

    const std::wstring& Text() const { return text_; }

private:
    enum class Action { Char, Shift, Backspace, Insert, Ok, Cancel };

    struct Key {
        std::wstring label;
        Action action;
        float width; // in key units; a plain key is 1
        std::wstring insert; // the text a Char or Insert key types
    };

    void Press(const Key& key);
    void Type(const std::wstring& text);
    void Backspace();
    void Move(int dx, int dy);
    float CenterOf(int row, int column) const;

    std::wstring title_;
    std::wstring text_;
    std::size_t caret_ = 0;
    bool shift_ = false;
    int row_ = 1;
    int column_ = 0;
    std::vector<std::vector<Key>> rows_;
};

} // namespace EdenXbox::Ui
