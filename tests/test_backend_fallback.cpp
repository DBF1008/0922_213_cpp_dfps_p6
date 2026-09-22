// Unit tests for backend value validity and the try -> verify -> fallback chain.
#include "check.h"
#include "utils/switch_verify.h"

int main() {
    // IsRateValidForBackend: settings takes Hz (>= 20), backdoor takes config index (< 20)
    CHECK(IsRateValidForBackend(60, false));
    CHECK(IsRateValidForBackend(120, false));
    CHECK(IsRateValidForBackend(2, false) == false);
    CHECK(IsRateValidForBackend(0, true));
    CHECK(IsRateValidForBackend(2, true));
    CHECK(IsRateValidForBackend(60, true) == false);
    CHECK(IsRateValidForBackend(-1, false)); // system default fits both
    CHECK(IsRateValidForBackend(-1, true));

    // Primary backend succeeds: fallback must not be attempted
    {
        int calls = 0;
        auto apply = [&calls](int, bool useSfBackdoor) {
            ++calls;
            return useSfBackdoor == false; // settings backend verifies ok
        };
        auto r = TrySwitchWithFallback(60, false, apply);
        CHECK(r == SwitchBackendResult::kPrimary);
        CHECK(calls == 1);
    }

    // Primary fails, value fits the alternate backend: degrade and succeed
    {
        int calls = 0;
        auto apply = [&calls](int, bool useSfBackdoor) {
            ++calls;
            return useSfBackdoor == true; // only backdoor verifies ok
        };
        auto r = TrySwitchWithFallback(-1, false, apply);
        CHECK(r == SwitchBackendResult::kFallback);
        CHECK(calls == 2);
    }

    // Primary fails and the value is not representable by the alternate
    // backend: no bogus attempt (e.g. never send Hz=120 as a config index)
    {
        int calls = 0;
        auto apply = [&calls](int, bool) {
            ++calls;
            return false;
        };
        auto r = TrySwitchWithFallback(120, false, apply);
        CHECK(r == SwitchBackendResult::kFailed);
        CHECK(calls == 1); // backdoor fallback correctly skipped for Hz values

        calls = 0;
        r = TrySwitchWithFallback(2, true, apply);
        CHECK(r == SwitchBackendResult::kFailed);
        CHECK(calls == 1); // settings fallback correctly skipped for index values
    }

    // Both backends fail: report failure so the caller can roll back
    {
        auto apply = [](int, bool) { return false; };
        auto r = TrySwitchWithFallback(-1, true, apply);
        CHECK(r == SwitchBackendResult::kFailed);
    }

    TEST_SUMMARY();
}
