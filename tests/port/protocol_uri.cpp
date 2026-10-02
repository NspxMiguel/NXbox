// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include "eden_uwp/protocol_uri.h"

int main() {
    using EdenXbox::ParsePlayTitleUri;
    int failures = 0;
    const auto check = [&](bool condition) {
        if (!condition)
            ++failures;
    };
    check(ParsePlayTitleUri("nxbox://play?title=01007EF00011E000") == "01007EF00011E000");
    check(ParsePlayTitleUri("nxbox://play?title=01007ef00011e000") == "01007EF00011E000");
    check(ParsePlayTitleUri("NXBOX://Play/?title=01007EF00011E000") == "01007EF00011E000");
    check(ParsePlayTitleUri("nxbox://play?x=1&title=0100225000FEE000&y=2") == "0100225000FEE000");
    check(ParsePlayTitleUri("nxbox://play?title=0100225000FEE000#frag") == "0100225000FEE000");
    for (const char* bad :
         {"", "nxbox://play", "nxbox://play?", "nxbox://play?title=", "nxbox://play?title=0100",
          "nxbox://play?title=01007EF00011E0000", "nxbox://play?title=01007EF00011E00G",
          "nxbox://other?title=01007EF00011E000", "http://play?title=01007EF00011E000",
          "nxbox://playx?title=01007EF00011E000", "nxbox://play?subtitle=01007EF00011E000"}) {
        check(ParsePlayTitleUri(bad).empty());
    }
    if (failures)
        std::cerr << failures << " protocol URI checks failed\n";
    return failures ? 1 : 0;
}
