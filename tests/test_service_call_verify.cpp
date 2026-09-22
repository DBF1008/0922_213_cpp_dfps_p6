// Unit tests for `service call SurfaceFlinger` result verification.
#include "check.h"
#include "utils/switch_verify.h"

int main() {
    // Success: exit 0 and a Parcel result
    CHECK(IsServiceCallResultOk(0, "Result: Parcel(00000000 00000000   '........')"));
    CHECK(IsServiceCallResultOk(0, "Result: Parcel(NULL)"));

    // Failure: non-zero exit status is never ok, even with Parcel-looking output
    CHECK(IsServiceCallResultOk(1, "Result: Parcel(00000000)") == false);
    CHECK(IsServiceCallResultOk(-1, "") == false);

    // Failure: exit 0 but the transaction did not produce a valid parcel
    CHECK(IsServiceCallResultOk(0, "") == false);
    CHECK(IsServiceCallResultOk(0, "service: Service SurfaceFlinger does not exist") == false);
    CHECK(IsServiceCallResultOk(0, "Result: Parcel(Error: unknown transaction code)") == false);
    CHECK(IsServiceCallResultOk(0, "java.lang.SecurityException: Permission Denial") == false);
    CHECK(IsServiceCallResultOk(0, "Result: Parcel(android.os.DeadObjectException)") == false);

    TEST_SUMMARY();
}
