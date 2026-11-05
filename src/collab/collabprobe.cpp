#include "collab/collabprobe.h"

#include <QDataStream>
#include <QIODevice>
#include <QTimer>
#include <QUdpSocket>

#include "moc_collabprobe.cpp"

namespace mixxx {
namespace collab {

namespace {

constexpr char kProbeMagic[] = "MXXC";
constexpr int kProbeMagicSize = 4;
constexpr quint8 kProbeVersion = 1;
constexpr int kProbePacketSize = kProbeMagicSize + 1 + 1 + 2 + 3 * 8;

} // namespace

QByteArray encodeCollabProbePacket(const CollabProbePacket& packet) {
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData(kProbeMagic, kProbeMagicSize);
    stream << kProbeVersion;
    stream << static_cast<quint8>(packet.type);
    stream << static_cast<quint16>(0);
    stream << packet.t1Us;
    stream << packet.t2Us;
    stream << packet.t3Us;
    return data;
}

bool decodeCollabProbePacket(const QByteArray& data, CollabProbePacket* pPacket) {
    if (pPacket == nullptr || data.size() != kProbePacketSize) {
        return false;
    }
    QDataStream stream(data);
    stream.setByteOrder(QDataStream::LittleEndian);

    char magic[kProbeMagicSize] = {};
    if (stream.readRawData(magic, kProbeMagicSize) != kProbeMagicSize) {
        return false;
    }
    if (qstrncmp(magic, kProbeMagic, kProbeMagicSize) != 0) {
        return false;
    }

    quint8 version = 0;
    quint8 type = 0;
    quint16 reserved = 0;
    quint64 t1Us = 0;
    quint64 t2Us = 0;
    quint64 t3Us = 0;
    stream >> version >> type >> reserved >> t1Us >> t2Us >> t3Us;
    if (stream.status() != QDataStream::Ok || version != kProbeVersion) {
        return false;
    }
    if (type != static_cast<quint8>(CollabProbePacket::Type::Ping) &&
            type != static_cast<quint8>(CollabProbePacket::Type::Pong)) {
        return false;
    }

    pPacket->type = static_cast<CollabProbePacket::Type>(type);
    pPacket->t1Us = t1Us;
    pPacket->t2Us = t2Us;
    pPacket->t3Us = t3Us;
    return true;
}

CollabProbe::CollabProbe(QObject* pParent)
        : QObject(pParent),
          m_pClock(std::make_unique<SteadyCollabClock>()),
          m_pSocket(new QUdpSocket(this)),
          m_pTimer(new QTimer(this)) {
    m_pTimer->setTimerType(Qt::PreciseTimer);
    connect(m_pTimer, &QTimer::timeout, this, &CollabProbe::sendPing);
    connect(m_pSocket,
            &QUdpSocket::readyRead,
            this,
            [this]() {
                while (m_pSocket->hasPendingDatagrams()) {
                    QByteArray data;
                    data.resize(static_cast<qsizetype>(
                            m_pSocket->pendingDatagramSize()));
                    QHostAddress sender;
                    quint16 senderPort = 0;
                    const qint64 read = m_pSocket->readDatagram(
                            data.data(), data.size(), &sender, &senderPort);
                    if (read < 0) {
                        continue;
                    }
                    data.resize(static_cast<qsizetype>(read));
                    handleDatagram(data, sender, senderPort);
                }
            });
}

CollabProbe::~CollabProbe() {
    stop();
}

bool CollabProbe::start(const QString& peerAddress,
        quint16 peerPort,
        quint16 localPort,
        int intervalMs) {
    stop();
    m_errorString.clear();

    QHostAddress address(peerAddress);
    if (address.isNull()) {
        m_errorString = tr("Invalid peer address: %1").arg(peerAddress);
        emit probeError(m_errorString);
        return false;
    }
    if (peerPort == 0) {
        m_errorString = tr("Peer port must not be zero");
        emit probeError(m_errorString);
        return false;
    }

    if (!m_pSocket->bind(QHostAddress::Any, localPort)) {
        m_errorString = tr("Cannot bind UDP port %1: %2")
                                .arg(localPort)
                                .arg(m_pSocket->errorString());
        emit probeError(m_errorString);
        return false;
    }
    m_peerAddress = address;
    m_peerPort = peerPort;
    m_clockSync.reset();
    m_running = true;

    const int interval = intervalMs > 0 ? intervalMs : 200;
    m_pTimer->start(interval);
    sendPing();
    return true;
}

void CollabProbe::stop() {
    if (m_pTimer != nullptr) {
        m_pTimer->stop();
    }
    if (m_pSocket != nullptr) {
        m_pSocket->disconnectFromHost();
        m_pSocket->close();
    }
    m_running = false;
}

bool CollabProbe::isRunning() const {
    return m_running;
}

void CollabProbe::clearStatistics() {
    m_clockSync.reset();
    emit statisticsChanged();
}

QString CollabProbe::localAddress() const {
    return m_pSocket->localAddress().toString();
}

quint16 CollabProbe::localPort() const {
    return m_pSocket->localPort();
}

void CollabProbe::sendPing() {
    if (!m_running) {
        return;
    }
    CollabProbePacket ping;
    ping.type = CollabProbePacket::Type::Ping;
    ping.t1Us = static_cast<quint64>(m_pClock->nowUs());
    const QByteArray data = encodeCollabProbePacket(ping);
    if (m_pSocket->writeDatagram(data, m_peerAddress, m_peerPort) != data.size()) {
        m_errorString = m_pSocket->errorString();
        emit probeError(m_errorString);
    }
}

void CollabProbe::handleDatagram(const QByteArray& data,
        const QHostAddress& sender,
        quint16 senderPort) {
    CollabProbePacket packet;
    if (!decodeCollabProbePacket(data, &packet)) {
        return;
    }
    // Only the configured peer may feed the estimator.
    if (sender != m_peerAddress || senderPort != m_peerPort) {
        return;
    }

    const qint64 nowUs = m_pClock->nowUs();
    if (packet.type == CollabProbePacket::Type::Ping) {
        CollabProbePacket pong;
        pong.type = CollabProbePacket::Type::Pong;
        pong.t1Us = packet.t1Us;
        pong.t2Us = static_cast<quint64>(nowUs);
        pong.t3Us = static_cast<quint64>(m_pClock->nowUs());
        m_pSocket->writeDatagram(encodeCollabProbePacket(pong), sender, senderPort);
        return;
    }

    const qint64 t1Us = static_cast<qint64>(packet.t1Us);
    const qint64 t2Us = static_cast<qint64>(packet.t2Us);
    const qint64 t3Us = static_cast<qint64>(packet.t3Us);
    if (!m_clockSync.addSample(t1Us, t2Us, t3Us, nowUs)) {
        return;
    }

    const CollabClockSample* pSample = m_clockSync.lastSample();
    if (pSample != nullptr) {
        m_diagnostics.logClockSample(nowUs,
                m_clockSync.toRemoteTime(nowUs),
                *pSample,
                m_clockSync.offsetUs(),
                m_clockSync.jitterUs(),
                m_clockSync.skewPpm());
    }

    emit statisticsChanged();
}

} // namespace collab
} // namespace mixxx
