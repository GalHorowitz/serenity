/*
 * Copyright (c) 2021, the SerenityOS developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <wchar.h>

TEST_CASE(wcscoll)
{
    // Check if wcscoll is sorting correctly. At the moment we are doing raw char comparisons,
    // so it's digits, then uppercase letters, then lowercase letters.

    // Equalness between equal strings.
    EXPECT(wcscoll(L"", L"") == 0);
    EXPECT(wcscoll(L"0", L"0") == 0);

    // Shorter strings before longer strings.
    EXPECT(wcscoll(L"", L"0") < 0);
    EXPECT(wcscoll(L"0", L"") > 0);
    EXPECT(wcscoll(L"123", L"1234") < 0);
    EXPECT(wcscoll(L"1234", L"123") > 0);

    // Order within digits.
    EXPECT(wcscoll(L"0", L"9") < 0);
    EXPECT(wcscoll(L"9", L"0") > 0);

    // Digits before uppercase letters.
    EXPECT(wcscoll(L"9", L"A") < 0);
    EXPECT(wcscoll(L"A", L"9") > 0);

    // Order within uppercase letters.
    EXPECT(wcscoll(L"A", L"Z") < 0);
    EXPECT(wcscoll(L"Z", L"A") > 0);

    // Uppercase letters before lowercase letters.
    EXPECT(wcscoll(L"Z", L"a") < 0);
    EXPECT(wcscoll(L"a", L"Z") > 0);

    // Uppercase letters before lowercase letters.
    EXPECT(wcscoll(L"a", L"z") < 0);
    EXPECT(wcscoll(L"z", L"a") > 0);
}
