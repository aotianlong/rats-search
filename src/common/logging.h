#ifndef RATS_COMMON_LOGGING_H
#define RATS_COMMON_LOGGING_H

namespace rats {
namespace common {

// How much disk the log is allowed to occupy in total, in megabytes: the active
// rats-search.log plus every rotated archive next to it. This is the number the
// user sets ("logs must not eat more than N MB"); the per-file threshold the
// logger actually wants is derived from it in applyLogSizeBudget().
inline constexpr int kDefaultLogMaxSizeMb = 100;
inline constexpr int kMinLogMaxSizeMb = 5;
inline constexpr int kMaxLogMaxSizeMb = 10000;

// Rotated archives kept beside the active file (rats-search.log.1 .. .N), so the
// budget is split across kLogRetentionCount + 1 files. Four archives is a
// compromise: rotation on startup consumes one slot per launch, so fewer slots
// would let a couple of restarts wipe the log of the run that actually crashed,
// while more slots make each file too small to hold a whole session.
inline constexpr int kLogRetentionCount = 4;

// Split totalMb across the active log and its archives and push the result into
// the librats logger. Called once at startup with the default (config is not
// readable yet) and again from Application::applyConfig() with the stored
// logMaxSizeMb, so changing the setting takes effect without a restart.
// totalMb is clamped to [kMinLogMaxSizeMb, kMaxLogMaxSizeMb].
void applyLogSizeBudget(int totalMb);

} // namespace common
} // namespace rats

#endif // RATS_COMMON_LOGGING_H
