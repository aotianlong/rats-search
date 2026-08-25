#include "data/row_id.h"

namespace rats::data {

qint64 rowIdFromHash(const QString& hash)
{
    // A BitTorrent v1 infohash is 40 hex characters. Anything shorter cannot
    // supply the 16 nibbles this reads, and anything non-hex is not an infohash.
    if (hash.size() < 16)
        return 0;

    bool ok = false;
    // Case-insensitive on purpose: the same torrent must land on the same row id
    // whether its hash arrived upper- or lower-cased.
    const quint64 top = QStringView(hash).left(16).toULongLong(&ok, 16);
    if (!ok)
        return 0;

    const qint64 id = static_cast<qint64>(top >> 1);
    // 0 means "auto-assign" to Manticore, so the one hash that would produce it
    // is nudged to 1. That is a collision like any other and is caught by the
    // hash verification every caller performs.
    return id == 0 ? 1 : id;
}

} // namespace rats::data
