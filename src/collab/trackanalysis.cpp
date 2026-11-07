#include "collab/trackanalysis.h"

#include "track/beats.h"
#include "waveform/waveform.h"

namespace mixxx {
namespace collab {

namespace {

constexpr uint8_t kBundleVersion = 1;
// A waveform bundle for a several-minute track is well under 1 MiB; the cap
// only exists to reject absurd or corrupt payloads before allocating.
constexpr int kMaxBlobBytes = 8 * 1024 * 1024;
constexpr int kMaxEntryBytes = 8 * 1024 * 1024;
constexpr int kMaxEntries = 8;

void putUint16(QByteArray* out, uint16_t value) {
    out->append(static_cast<char>(value & 0xFF));
    out->append(static_cast<char>((value >> 8) & 0xFF));
}

void putUint32(QByteArray* out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out->append(static_cast<char>((value >> shift) & 0xFF));
    }
}

void putString(QByteArray* out, const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    putUint16(out, static_cast<uint16_t>(utf8.size()));
    out->append(utf8);
}

bool getUint8(const QByteArray& blob, int* pos, uint8_t* value) {
    if (*pos >= blob.size()) {
        return false;
    }
    *value = static_cast<uint8_t>(blob.at(*pos));
    ++*pos;
    return true;
}

bool getUint16(const QByteArray& blob, int* pos, uint16_t* value) {
    if (*pos + 2 > blob.size()) {
        return false;
    }
    uint16_t v = 0;
    v |= static_cast<uint8_t>(blob.at(*pos));
    v |= static_cast<uint16_t>(static_cast<uint8_t>(blob.at(*pos + 1))) << 8;
    *value = v;
    *pos += 2;
    return true;
}

bool getUint32(const QByteArray& blob, int* pos, uint32_t* value) {
    if (*pos + 4 > blob.size()) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(static_cast<uint8_t>(blob.at(*pos + i)))
                << (8 * i);
    }
    *value = v;
    *pos += 4;
    return true;
}

bool getString(const QByteArray& blob, int* pos, QString* value) {
    uint16_t len = 0;
    if (!getUint16(blob, pos, &len) || *pos + len > blob.size()) {
        return false;
    }
    *value = QString::fromUtf8(blob.constData() + *pos, len);
    *pos += len;
    return true;
}

} // namespace

QByteArray encodeCollabTrackAnalysisBundle(const CollabTrackAnalysis& analysis) {
    QByteArray blob;
    if (analysis.entries.size() > kMaxEntries) {
        return blob;
    }
    blob.reserve(16);
    blob.append(static_cast<char>(kBundleVersion));
    putUint32(&blob, analysis.sampleRate);
    blob.append(static_cast<char>(analysis.entries.size()));
    for (const CollabTrackAnalysisEntry& entry : analysis.entries) {
        blob.append(static_cast<char>(static_cast<uint8_t>(entry.type)));
        putString(&blob, entry.version);
        putString(&blob, entry.subVersion);
        putUint32(&blob, static_cast<uint32_t>(entry.data.size()));
        blob.append(entry.data);
    }
    if (blob.size() > kMaxBlobBytes) {
        return QByteArray();
    }
    return blob;
}

bool decodeCollabTrackAnalysisBundle(
        const QByteArray& blob, CollabTrackAnalysis* pAnalysis) {
    if (pAnalysis == nullptr || blob.isEmpty() || blob.size() > kMaxBlobBytes) {
        return false;
    }
    int pos = 0;
    uint8_t version = 0;
    if (!getUint8(blob, &pos, &version) || version != kBundleVersion) {
        return false;
    }
    CollabTrackAnalysis analysis;
    if (!getUint32(blob, &pos, &analysis.sampleRate)) {
        return false;
    }
    uint8_t count = 0;
    if (!getUint8(blob, &pos, &count) || count > kMaxEntries) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        CollabTrackAnalysisEntry entry;
        uint8_t type = 0;
        if (!getUint8(blob, &pos, &type) || type > 2) {
            return false;
        }
        entry.type = static_cast<CollabTrackAnalysisEntry::Type>(type);
        if (!getString(blob, &pos, &entry.version) ||
                !getString(blob, &pos, &entry.subVersion)) {
            return false;
        }
        uint32_t len = 0;
        if (!getUint32(blob, &pos, &len) ||
                len > static_cast<uint32_t>(kMaxEntryBytes) ||
                pos + static_cast<int>(len) > blob.size()) {
            return false;
        }
        entry.data = blob.mid(pos, static_cast<int>(len));
        pos += static_cast<int>(len);
        analysis.entries.append(entry);
    }
    *pAnalysis = analysis;
    return true;
}

CollabTrackAnalysis buildCollabTrackAnalysis(const TrackPointer& pTrack) {
    CollabTrackAnalysis analysis;
    if (!pTrack) {
        return analysis;
    }
    analysis.sampleRate = pTrack->getSampleRate().value();

    const ConstWaveformPointer pWaveform = pTrack->getWaveform();
    if (pWaveform && pWaveform->getDataSize() > 0) {
        CollabTrackAnalysisEntry entry;
        entry.type = CollabTrackAnalysisEntry::Type::Waveform;
        entry.version = pWaveform->getVersion();
        entry.subVersion = pWaveform->getDescription();
        entry.data = pWaveform->toByteArray();
        analysis.entries.append(entry);
    }
    const ConstWaveformPointer pSummary = pTrack->getWaveformSummary();
    if (pSummary && pSummary->getDataSize() > 0) {
        CollabTrackAnalysisEntry entry;
        entry.type = CollabTrackAnalysisEntry::Type::WaveformSummary;
        entry.version = pSummary->getVersion();
        entry.subVersion = pSummary->getDescription();
        entry.data = pSummary->toByteArray();
        analysis.entries.append(entry);
    }
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    if (pBeats) {
        CollabTrackAnalysisEntry entry;
        entry.type = CollabTrackAnalysisEntry::Type::Beats;
        entry.version = pBeats->getVersion();
        entry.subVersion = pBeats->getSubVersion();
        entry.data = pBeats->toByteArray();
        analysis.entries.append(entry);
    }
    return analysis;
}

void applyCollabTrackAnalysis(
        const TrackPointer& pTrack, const CollabTrackAnalysis& analysis) {
    if (!pTrack) {
        return;
    }
    const mixxx::audio::SampleRate sampleRate(
            static_cast<int>(analysis.sampleRate));
    for (const CollabTrackAnalysisEntry& entry : analysis.entries) {
        switch (entry.type) {
        case CollabTrackAnalysisEntry::Type::Waveform: {
            auto pWaveform = WaveformPointer(new Waveform(entry.data));
            pWaveform->setVersion(entry.version);
            pWaveform->setDescription(entry.subVersion);
            pTrack->setWaveform(pWaveform);
            break;
        }
        case CollabTrackAnalysisEntry::Type::WaveformSummary: {
            auto pWaveform = WaveformPointer(new Waveform(entry.data));
            pWaveform->setVersion(entry.version);
            pWaveform->setDescription(entry.subVersion);
            pTrack->setWaveformSummary(pWaveform);
            break;
        }
        case CollabTrackAnalysisEntry::Type::Beats: {
            if (!sampleRate.isValid() || entry.version.isEmpty()) {
                break;
            }
            const mixxx::BeatsPointer pBeats = mixxx::Beats::fromByteArray(
                    sampleRate, entry.version, entry.subVersion, entry.data);
            if (pBeats) {
                pTrack->trySetBeats(pBeats);
            }
            break;
        }
        }
    }
}

} // namespace collab
} // namespace mixxx
