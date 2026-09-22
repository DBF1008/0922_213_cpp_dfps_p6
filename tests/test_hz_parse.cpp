// Unit tests for refresh-rate value parsing and settings readback verification.
#include "check.h"
#include "utils/switch_verify.h"

int main() {
    int hz = 0;

    // ParseHzValue: accepts plain values, signs, whitespace and float-ish output
    CHECK(ParseHzValue("60", &hz) && hz == 60);
    CHECK(ParseHzValue("-1", &hz) && hz == -1);
    CHECK(ParseHzValue(" 120\n", &hz) && hz == 120);
    CHECK(ParseHzValue("60.0", &hz) && hz == 60); // some ROMs report float rates
    CHECK(ParseHzValue("0", &hz) && hz == 0);

    // ParseHzValue: rejects settings' "null" and other non-numeric output
    CHECK(ParseHzValue("null", &hz) == false);
    CHECK(ParseHzValue("", &hz) == false);
    CHECK(ParseHzValue("abc", &hz) == false);
    CHECK(ParseHzValue(" ", &hz) == false);
    CHECK(ParseHzValue("60", nullptr) == false);

    // VerifySettingsReadback: readback must match the value we wrote
    CHECK(VerifySettingsReadback("60", "60"));
    CHECK(VerifySettingsReadback("120", " 120\n"));
    CHECK(VerifySettingsReadback("60", "60.0"));
    CHECK(VerifySettingsReadback("-1", "-1"));
    CHECK(VerifySettingsReadback("60", "90") == false);
    CHECK(VerifySettingsReadback("60", "120") == false);
    CHECK(VerifySettingsReadback("60", "null") == false); // key missing on this ROM
    CHECK(VerifySettingsReadback("60", "") == false);

    TEST_SUMMARY();
}
