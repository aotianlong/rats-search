#include "common/logging.h"

// The one place above net/ that talks to librats directly: the logger is the
// process-wide sink Qt logging is routed into, not part of the P2P surface. No
// librats type crosses this boundary - the header above is plain ints.
#include "librats/util/logger.h"

#include <algorithm>

namespace rats {
namespace common {

void applyLogSizeBudget(int totalMb)
{
    const int budget = std::clamp(totalMb, kMinLogMaxSizeMb, kMaxLogMaxSizeMb);

    // Per-file threshold: retention archives + the active file must fit the
    // budget. The logger checks the threshold before writing a line, so a file
    // overshoots by at most one line - negligible against a megabyte-sized cap.
    const size_t perFile = static_cast<size_t>(budget) * 1024 * 1024 / (kLogRetentionCount + 1);

    auto& logger = librats::Logger::getInstance();
    logger.set_log_rotation_size(perFile);
    logger.set_log_retention_count(kLogRetentionCount);
}

} // namespace common
} // namespace rats
