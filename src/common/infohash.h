#ifndef RATS_COMMON_INFOHASH_H
#define RATS_COMMON_INFOHASH_H

#include <QString>

namespace rats {

// A BitTorrent v1 info-hash is a 40-character hex SHA-1 digest. This is the one
// place that validates and normalises it; never hand-roll `hash.length() !=
// 40`.
namespace infohash {

inline constexpr int kLength = 40;

inline bool isValid(const QString& hash)
{
    if (hash.length() != kLength)
        return false;
    for (QChar c : hash) {
        // ASCII hex only. QChar::isDigit() is true for every Unicode decimal
        // digit — Arabic-Indic ٠١٢, Devanagari ०१२, and so on — so it used to
        // accept 40 characters that no hex parser can read. data::rowIdFromHash()
        // then derived 0 from them, which Manticore reads as "assign me an id":
        // a row nothing could ever address by hash again.
        const char16_t u = c.unicode();
        const bool isHex = (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
        if (!isHex)
            return false;
    }
    return true;
}

// Normalised form used everywhere internally: lower-case hex.
inline QString normalize(const QString& hash)
{
    return hash.trimmed().toLower();
}

} // namespace infohash
} // namespace rats

#endif // RATS_COMMON_INFOHASH_H
