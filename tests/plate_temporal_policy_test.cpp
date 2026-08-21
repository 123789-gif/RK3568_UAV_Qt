#include "npu/plate_temporal_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "plate_temporal_policy_test: FAIL: "
                  << message << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main()
{
    using namespace plate_temporal_policy;

    require(!shouldDisplay(1, 0, 0, true),
            "one hit must not expose a new plate");
    require(shouldDisplay(2, 0, 0, true),
            "two consistent hits must expose a new plate");
    require(shouldDisplay(3, 1, 120, true),
            "one missed inference must keep the OSD visible");
    require(!shouldDisplay(3, 2, 120, true),
            "two missed inferences must hide the OSD");
    require(!shouldDisplay(3, 0, 601, true),
            "expired internal track must not be displayed");
    require(!shouldDisplay(3, 0, 20, false),
            "empty stable text must not be displayed");

    require(identityChanged(true, "A12345", true, "B67890", 21.0),
            "strong ROI and text change must reset identity");
    require(!identityChanged(true, "A12345", true, "B67890", 20.0),
            "threshold boundary must retain identity");
    require(!identityChanged(true, "A12345", true, "A12345", 50.0),
            "same text must not reset identity");
    require(!identityChanged(false, "", true, "B67890", 50.0),
            "unconfirmed track must not trigger identity reset");

    std::cout << "plate_temporal_policy_test: PASS" << std::endl;
    return 0;
}
