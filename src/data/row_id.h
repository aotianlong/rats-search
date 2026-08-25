#ifndef RATS_DATA_ROW_ID_H
#define RATS_DATA_ROW_ID_H

#include <QString>

namespace rats::data {

// The Manticore row id of a torrent (and of its `files` row) is derived from
// the infohash instead of a local counter.
//
// Why: Manticore has no secondary indexes here — `lib_manticore_columnar` is not
// shipped in imports/ — so a filter on the `hash` string attribute is a full
// table scan. Every lookup Rats performs is by hash, and every one of them was
// therefore O(rows): on a million-torrent index a single exists() cost ~3 ms and
// a 500-hash batch ~230 ms, which made export/import quadratic. Filtering on
// `id` instead goes through Manticore's built-in docid lookup: the same batch
// costs ~9 ms, a single lookup ~0.4 ms.
//
// Two consequences worth knowing:
//   - the mapping is global, so the same torrent has the same row id on every
//     peer. REPLACE INTO is then insert-or-overwrite by torrent identity, which
//     is what makes a dump merge idempotent.
//   - ids are no longer chronological. Nothing depends on that: `recent()` sorts
//     by `added`, and the two keyset sweeps (ApiRouter's cleanup, the database
//     export) only need *a* total order.
//
// 63 bits, not 64: Manticore ids are unsigned but the codebase carries them as
// qint64, so the top bit is dropped rather than risking a negative id. The
// birthday bound at 10M torrents is ~5e-6, and a collision is never silent —
// every caller verifies the stored hash before trusting a row (see
// TorrentRepository).
//
// Returns 0 for anything that is not a usable infohash; 0 is never a valid row
// id (Manticore reads it as "auto-assign"), so callers can test for it.
qint64 rowIdFromHash(const QString& hash);

} // namespace rats::data

#endif // RATS_DATA_ROW_ID_H
