#include "collab/collabprotocol.h"

#include <QDataStream>
#include <QIODevice>
#include <QRandomGenerator>
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace mixxx {
namespace collab {

namespace {

constexpr char kProtocolMagic[] = "MXXC";
constexpr int kProtocolMagicSize = 4;
constexpr int kMaxReasonStringBytes = 512;

void initStream(QDataStream& stream) {
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}

void writeId(QDataStream& stream, const uint8_t* pData, int size) {
    stream.writeRawData(reinterpret_cast<const char*>(pData), size);
}

void writeString(QDataStream& stream, const QString& text) {
    QByteArray utf8 = text.toUtf8();
    if (utf8.size() > kMaxReasonStringBytes) {
        utf8.truncate(kMaxReasonStringBytes);
    }
    stream << static_cast<uint16_t>(utf8.size());
    stream.writeRawData(utf8.constData(), utf8.size());
}

bool readString(QDataStream& stream, QString* pText) {
    uint16_t size = 0;
    stream >> size;
    if (stream.status() != QDataStream::Ok || size > kMaxReasonStringBytes) {
        return false;
    }
    if (stream.device()->bytesAvailable() < size) {
        return false;
    }
    QByteArray utf8(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (stream.readRawData(utf8.data(), size) != size) {
        return false;
    }
    *pText = QString::fromUtf8(utf8);
    return true;
}

void writeU32(QDataStream& stream, uint32_t value) {
    stream << static_cast<quint32>(value);
}

void writeU64(QDataStream& stream, uint64_t value) {
    stream << static_cast<quint64>(value);
}

bool readU32(QDataStream& stream, uint32_t* pValue) {
    quint32 value = 0;
    stream >> value;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    *pValue = static_cast<uint32_t>(value);
    return true;
}

bool readU64(QDataStream& stream, uint64_t* pValue) {
    quint64 value = 0;
    stream >> value;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    *pValue = static_cast<uint64_t>(value);
    return true;
}

template<typename T>
void fillRandom(T* pValue) {
    auto* pBytes = reinterpret_cast<uint8_t*>(pValue);
    for (std::size_t i = 0; i < sizeof(T); i += 4) {
        const quint32 value = QRandomGenerator::system()->generate();
        const std::size_t remaining = std::min<std::size_t>(4, sizeof(T) - i);
        std::memcpy(pBytes + i, &value, remaining);
    }
}

} // namespace

bool CollabCommand::addArg(uint16_t id, double value) {
    if (argCount >= kCollabMaxCommandArgs) {
        return false;
    }
    args[argCount] = CollabCommandArg{id, value};
    ++argCount;
    return true;
}

QByteArray encodeCollabPacket(const CollabHeader& header, const QByteArray& payload) {
    QByteArray data;
    data.reserve(kCollabHeaderSize + payload.size());
    QDataStream stream(&data, QIODevice::WriteOnly);
    initStream(stream);

    stream.writeRawData(kProtocolMagic, kProtocolMagicSize);
    stream << static_cast<uint8_t>(kCollabProtocolVersion);
    stream << static_cast<uint8_t>(header.type);
    stream << header.flags;
    writeId(stream, header.sessionId.data(), static_cast<int>(header.sessionId.size()));
    writeId(stream,
            header.senderPeerId.data(),
            static_cast<int>(header.senderPeerId.size()));
    writeU32(stream, header.seq);
    writeU32(stream, header.ack);
    writeU64(stream, header.ackBits);
    writeU64(stream, header.sendTimeUs);
    stream.writeRawData(payload.constData(), payload.size());
    return data;
}

bool decodeCollabPacket(const QByteArray& data, CollabPacket* pPacket) {
    if (pPacket == nullptr || data.size() < kCollabHeaderSize ||
            data.size() > kCollabMaxDatagramSize) {
        return false;
    }

    QDataStream stream(data);
    initStream(stream);

    char magic[kProtocolMagicSize] = {};
    if (stream.readRawData(magic, kProtocolMagicSize) != kProtocolMagicSize) {
        return false;
    }
    if (qstrncmp(magic, kProtocolMagic, kProtocolMagicSize) != 0) {
        return false;
    }

    uint8_t version = 0;
    uint8_t type = 0;
    uint16_t flags = 0;
    CollabHeader header;
    stream >> version;
    stream >> type;
    stream >> flags;
    if (stream.status() != QDataStream::Ok || version != kCollabProtocolVersion) {
        return false;
    }
    header.type = static_cast<CollabMessageType>(type);
    header.flags = flags;
    if (stream.readRawData(reinterpret_cast<char*>(header.sessionId.data()),
                static_cast<int>(header.sessionId.size())) !=
                    static_cast<int>(header.sessionId.size()) ||
            stream.readRawData(reinterpret_cast<char*>(header.senderPeerId.data()),
                    static_cast<int>(header.senderPeerId.size())) !=
                    static_cast<int>(header.senderPeerId.size())) {
        return false;
    }
    if (!readU32(stream, &header.seq) || !readU32(stream, &header.ack) ||
            !readU64(stream, &header.ackBits) ||
            !readU64(stream, &header.sendTimeUs)) {
        return false;
    }

    pPacket->header = header;
    pPacket->payload = data.mid(kCollabHeaderSize);
    return true;
}

QString collabMessageTypeToString(CollabMessageType type) {
    switch (type) {
    case CollabMessageType::Hello:
        return QStringLiteral("HELLO");
    case CollabMessageType::Welcome:
        return QStringLiteral("WELCOME");
    case CollabMessageType::Reject:
        return QStringLiteral("REJECT");
    case CollabMessageType::Bye:
        return QStringLiteral("BYE");
    case CollabMessageType::ClockPing:
        return QStringLiteral("CLOCK_PING");
    case CollabMessageType::ClockPong:
        return QStringLiteral("CLOCK_PONG");
    case CollabMessageType::ControlDelta:
        return QStringLiteral("CONTROL_DELTA");
    case CollabMessageType::ControlKeyframe:
        return QStringLiteral("CONTROL_KEYFRAME");
    case CollabMessageType::DeckAnchor:
        return QStringLiteral("DECK_ANCHOR");
    case CollabMessageType::DeckState:
        return QStringLiteral("DECK_STATE");
    case CollabMessageType::Command:
        return QStringLiteral("COMMAND");
    case CollabMessageType::CommandAck:
        return QStringLiteral("COMMAND_ACK");
    case CollabMessageType::CommandNack:
        return QStringLiteral("COMMAND_NACK");
    case CollabMessageType::Keepalive:
        return QStringLiteral("KEEPALIVE");
    case CollabMessageType::KeepaliveAck:
        return QStringLiteral("KEEPALIVE_ACK");
    case CollabMessageType::Error:
        return QStringLiteral("ERROR");
    }
    return QStringLiteral("UNKNOWN");
}

QString collabCommandActionToString(CollabCommandAction action) {
    switch (action) {
    case CollabCommandAction::Play:
        return QStringLiteral("PLAY");
    case CollabCommandAction::Pause:
        return QStringLiteral("PAUSE");
    case CollabCommandAction::Seek:
        return QStringLiteral("SEEK");
    case CollabCommandAction::CueStart:
        return QStringLiteral("CUE_START");
    case CollabCommandAction::HotcueActivate:
        return QStringLiteral("HOTCUE_ACTIVATE");
    case CollabCommandAction::LoopSet:
        return QStringLiteral("LOOP_SET");
    case CollabCommandAction::LoopEnable:
        return QStringLiteral("LOOP_ENABLE");
    case CollabCommandAction::Beatloop:
        return QStringLiteral("BEATLOOP");
    case CollabCommandAction::RateSet:
        return QStringLiteral("RATE_SET");
    case CollabCommandAction::KeylockSet:
        return QStringLiteral("KEYLOCK_SET");
    }
    return QStringLiteral("UNKNOWN");
}

QByteArray encodeCollabHello(const CollabHello& hello) {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    initStream(stream);
    stream << static_cast<uint8_t>(hello.role);
    stream << static_cast<uint8_t>(0);
    stream << static_cast<uint16_t>(0);
    stream << hello.featureFlags;
    writeId(stream, hello.token.data(), static_cast<int>(hello.token.size()));
    return payload;
}

bool decodeCollabHello(const QByteArray& payload, CollabHello* pHello) {
    if (pHello == nullptr) {
        return false;
    }
    QDataStream stream(payload);
    initStream(stream);
    uint8_t role = 0;
    uint8_t reserved8 = 0;
    uint16_t reserved16 = 0;
    CollabHello hello;
    stream >> role >> reserved8 >> reserved16 >> hello.featureFlags;
    if (stream.status() != QDataStream::Ok || role > 1) {
        return false;
    }
    hello.role = role == 0 ? CollabRole::Host : CollabRole::Guest;
    if (stream.readRawData(reinterpret_cast<char*>(hello.token.data()),
                static_cast<int>(hello.token.size())) !=
            static_cast<int>(hello.token.size())) {
        return false;
    }
    *pHello = hello;
    return true;
}

QByteArray encodeCollabWelcome(const CollabWelcome& welcome) {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    initStream(stream);
    writeU32(stream, welcome.sessionEpoch);
    stream << static_cast<uint8_t>(welcome.role);
    stream << static_cast<uint8_t>(0);
    stream << static_cast<uint16_t>(0);
    writeU64(stream, welcome.hostSessionTimeUs);
    return payload;
}

bool decodeCollabWelcome(const QByteArray& payload, CollabWelcome* pWelcome) {
    if (pWelcome == nullptr) {
        return false;
    }
    QDataStream stream(payload);
    initStream(stream);
    CollabWelcome welcome;
    uint8_t role = 0;
    uint8_t reserved8 = 0;
    uint16_t reserved16 = 0;
    if (!readU32(stream, &welcome.sessionEpoch)) {
        return false;
    }
    stream >> role >> reserved8 >> reserved16;
    if (stream.status() != QDataStream::Ok || role > 1) {
        return false;
    }
    if (!readU64(stream, &welcome.hostSessionTimeUs)) {
        return false;
    }
    welcome.role = role == 0 ? CollabRole::Host : CollabRole::Guest;
    *pWelcome = welcome;
    return true;
}

QByteArray encodeCollabBye(const CollabBye& bye) {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    initStream(stream);
    stream << bye.reasonCode;
    writeString(stream, bye.message);
    return payload;
}

bool decodeCollabBye(const QByteArray& payload, CollabBye* pBye) {
    if (pBye == nullptr) {
        return false;
    }
    QDataStream stream(payload);
    initStream(stream);
    CollabBye bye;
    stream >> bye.reasonCode;
    if (stream.status() != QDataStream::Ok || !readString(stream, &bye.message)) {
        return false;
    }
    *pBye = bye;
    return true;
}

QByteArray encodeCollabClockPing(const CollabClockPing& ping) {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    initStream(stream);
    writeU64(stream, ping.t1Us);
    return payload;
}

bool decodeCollabClockPing(const QByteArray& payload, CollabClockPing* pPing) {
    if (pPing == nullptr) {
        return false;
    }
    QDataStream stream(payload);
    initStream(stream);
    CollabClockPing ping;
    if (!readU64(stream, &ping.t1Us)) {
        return false;
    }
    *pPing = ping;
    return true;
}

QByteArray encodeCollabClockPong(const CollabClockPong& pong) {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    initStream(stream);
    writeU64(stream, pong.t1Us);
    writeU64(stream, pong.t2Us);
    writeU64(stream, pong.t3Us);
    return payload;
}

bool decodeCollabClockPong(const QByteArray& payload, CollabClockPong* pPong) {
    if (pPong == nullptr) {
        return false;
    }
    QDataStream stream(payload);
    initStream(stream);
    CollabClockPong pong;
    if (!readU64(stream, &pong.t1Us) || !readU64(stream, &pong.t2Us) ||
            !readU64(stream, &pong.t3Us)) {
        return false;
    }
    *pPong = pong;
    return true;
}

QByteArray encodeCollabCommand(const CollabCommand& command) {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    initStream(stream);
    writeU64(stream, command.commandId);
    stream << static_cast<uint8_t>(command.action);
    stream << command.flags;
    stream << command.deckId;
    writeU64(stream, command.targetSessionTimeUs);
    const uint8_t count =
            std::min<uint8_t>(command.argCount, kCollabMaxCommandArgs);
    writeU32(stream, count);
    for (uint8_t i = 0; i < count; ++i) {
        stream << command.args[i].id;
        stream << command.args[i].value;
    }
    return payload;
}

bool decodeCollabCommand(const QByteArray& payload, CollabCommand* pCommand) {
    if (pCommand == nullptr) {
        return false;
    }
    QDataStream stream(payload);
    initStream(stream);
    CollabCommand command;
    uint8_t action = 0;
    uint32_t argCount = 0;
    if (!readU64(stream, &command.commandId)) {
        return false;
    }
    stream >> action >> command.flags >> command.deckId;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    if (!readU64(stream, &command.targetSessionTimeUs) ||
            !readU32(stream, &argCount) ||
            argCount > kCollabMaxCommandArgs) {
        return false;
    }
    if (!isValidCollabDeckId(command.deckId)) {
        return false;
    }
    command.action = static_cast<CollabCommandAction>(action);
    for (uint32_t i = 0; i < argCount; ++i) {
        CollabCommandArg arg;
        stream >> arg.id >> arg.value;
        if (stream.status() != QDataStream::Ok) {
            return false;
        }
        command.args[command.argCount++] = arg;
    }
    *pCommand = command;
    return true;
}

QByteArray encodeCollabCommandAck(const CollabCommandAck& ack) {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    initStream(stream);
    writeU64(stream, ack.commandId);
    stream << static_cast<uint8_t>(ack.result);
    writeU64(stream, ack.appliedSessionTimeUs);
    stream << ack.reasonCode;
    writeString(stream, ack.reason);
    return payload;
}

bool decodeCollabCommandAck(const QByteArray& payload, CollabCommandAck* pAck) {
    if (pAck == nullptr) {
        return false;
    }
    QDataStream stream(payload);
    initStream(stream);
    CollabCommandAck ack;
    uint8_t result = 0;
    if (!readU64(stream, &ack.commandId)) {
        return false;
    }
    stream >> result;
    if (stream.status() != QDataStream::Ok || result > 4) {
        return false;
    }
    if (!readU64(stream, &ack.appliedSessionTimeUs)) {
        return false;
    }
    stream >> ack.reasonCode;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    ack.result = static_cast<CollabCommandResult>(result);
    if (!readString(stream, &ack.reason)) {
        return false;
    }
    *pAck = ack;
    return true;
}

bool collabTokensEqual(const std::array<uint8_t, 16>& lhs,
        const std::array<uint8_t, 16>& rhs) {
    volatile uint8_t difference = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        difference = difference | static_cast<uint8_t>(lhs[i] ^ rhs[i]);
    }
    return difference == 0;
}

CollabSessionId makeCollabSessionId() {
    CollabSessionId id{};
    fillRandom(&id);
    return id;
}

CollabPeerId makeCollabPeerId() {
    CollabPeerId id{};
    fillRandom(&id);
    return id;
}

CollabSessionToken makeCollabSessionToken() {
    CollabSessionToken token{};
    fillRandom(&token);
    return token;
}

} // namespace collab
} // namespace mixxx
