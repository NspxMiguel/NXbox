// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include "eden_uwp/ui/update_version.h"

int main() {
    using EdenXbox::Ui::ParseUpdateVersion;
    using EdenXbox::Ui::UpdateVersion;
    int failures = 0;
    const auto check = [&](bool condition) {
        if (!condition)
            ++failures;
    };
    check(ParseUpdateVersion(L"v1.2.3") == UpdateVersion{1, 2, 3, 0});
    check(ParseUpdateVersion(L"v1.2.3.4") == UpdateVersion{1, 2, 3, 4});
    check(ParseUpdateVersion(L"v65535.65535.65535.65535") ==
          UpdateVersion{65535, 65535, 65535, 65535});
    check(ParseUpdateVersion(L"v1.10.0") > ParseUpdateVersion(L"v1.9.99"));
    check(ParseUpdateVersion(L"v2.0.0") > ParseUpdateVersion(L"v1.65535.65535.65535"));
    check(ParseUpdateVersion(L"v1.2.3.1") > ParseUpdateVersion(L"v1.2.3"));
    for (const auto tag :
         {L"", L"1.2.3", L"v1.2", L"v0.3.999", L"v1.2.3-beta", L"v1.2.3.", L"v1.2.3.4.5",
          L"v65536.0.0", L"v1.-2.3", L"v1.2.3\n", L"v999999999999999999999.0.0"}) {
        check(!ParseUpdateVersion(tag));
    }
    if (failures)
        std::cerr << failures << " update version checks failed\n";
    return failures ? 1 : 0;
}
