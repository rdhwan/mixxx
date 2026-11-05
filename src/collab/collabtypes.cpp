#include "collab/collabtypes.h"

#include <QChar>
#include <QString>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace mixxx {
namespace collab {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hexDigitValue(QChar c) {
    const char ch = c.toLatin1();
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

template<std::size_t N>
QString idToHex(const std::array<uint8_t, N>& id) {
    QString result;
    result.reserve(static_cast<qsizetype>(N * 2));
    for (const uint8_t byte : id) {
        result.append(QLatin1Char(kHexDigits[(byte >> 4) & 0x0F]));
        result.append(QLatin1Char(kHexDigits[byte & 0x0F]));
    }
    return result;
}

template<std::size_t N>
bool idFromHex(const QString& hex, std::array<uint8_t, N>* pId) {
    if (pId == nullptr) {
        return false;
    }
    if (hex.size() != static_cast<qsizetype>(N * 2)) {
        return false;
    }
    std::array<uint8_t, N> id{};
    for (std::size_t i = 0; i < N; ++i) {
        const int high = hexDigitValue(hex.at(static_cast<qsizetype>(i * 2)));
        const int low = hexDigitValue(hex.at(static_cast<qsizetype>(i * 2 + 1)));
        if (high < 0 || low < 0) {
            return false;
        }
        id[i] = static_cast<uint8_t>((high << 4) | low);
    }
    *pId = id;
    return true;
}

} // namespace

QString collabRoleToString(CollabRole role) {
    return role == CollabRole::Host ? QStringLiteral("host") : QStringLiteral("guest");
}

QString peerIdToHex(const CollabPeerId& peerId) {
    return idToHex(peerId);
}

QString sessionIdToHex(const CollabSessionId& sessionId) {
    return idToHex(sessionId);
}

bool peerIdFromHex(const QString& hex, CollabPeerId* pPeerId) {
    return idFromHex(hex, pPeerId);
}

bool sessionIdFromHex(const QString& hex, CollabSessionId* pSessionId) {
    return idFromHex(hex, pSessionId);
}

} // namespace collab
} // namespace mixxx
