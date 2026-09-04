#include "natural_action_lifecycle_report_v1.h"

int main() {
    using namespace a9tas::natural_action_lifecycle_report_v1;
    Report report{};
    report.flags = kRequiredFlags;
    return report.flags == 0x1FFFFu && sizeof(report) == 664 ? 0 : 1;
}
