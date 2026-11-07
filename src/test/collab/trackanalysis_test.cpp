#include "collab/trackanalysis.h"

#include <gtest/gtest.h>

#include "collab/trackhash.h"
#include "track/beats.h"
#include "waveform/waveform.h"

namespace mixxx {
namespace collab {

namespace {

CollabTrackAnalysis makeSampleAnalysis() {
    CollabTrackAnalysis analysis;
    analysis.sampleRate = 44100;

    CollabTrackAnalysisEntry waveform;
    waveform.type = CollabTrackAnalysisEntry::Type::Waveform;
    waveform.version = QStringLiteral("Waveform-6.1");
    waveform.subVersion = QStringLiteral("Waveform 6.1");
    waveform.data = QByteArrayLiteral("\x01\x02\x03\x04\x05\x06\x07\x08");
    analysis.entries.append(waveform);

    CollabTrackAnalysisEntry summary;
    summary.type = CollabTrackAnalysisEntry::Type::WaveformSummary;
    summary.version = QStringLiteral("WaveformSummary-6.1");
    summary.data = QByteArrayLiteral("\xaa\xbb\xcc\xdd");
    analysis.entries.append(summary);

    const mixxx::BeatsPointer pBeats = mixxx::Beats::fromConstTempo(
            mixxx::audio::SampleRate(44100),
            mixxx::audio::FramePos(0.0),
            mixxx::Bpm(128.0));
    CollabTrackAnalysisEntry beats;
    beats.type = CollabTrackAnalysisEntry::Type::Beats;
    beats.version = pBeats->getVersion();
    beats.subVersion = pBeats->getSubVersion();
    beats.data = pBeats->toByteArray();
    analysis.entries.append(beats);

    return analysis;
}

} // namespace

TEST(CollabTrackAnalysisTest, RoundTripsABundle) {
    const CollabTrackAnalysis analysis = makeSampleAnalysis();
    const QByteArray blob = encodeCollabTrackAnalysisBundle(analysis);
    ASSERT_FALSE(blob.isEmpty());

    CollabTrackAnalysis decoded;
    ASSERT_TRUE(decodeCollabTrackAnalysisBundle(blob, &decoded));
    EXPECT_EQ(decoded.sampleRate, analysis.sampleRate);
    ASSERT_EQ(decoded.entries.size(), analysis.entries.size());
    for (int i = 0; i < analysis.entries.size(); ++i) {
        EXPECT_EQ(decoded.entries[i].type, analysis.entries[i].type);
        EXPECT_EQ(decoded.entries[i].version, analysis.entries[i].version);
        EXPECT_EQ(decoded.entries[i].subVersion, analysis.entries[i].subVersion);
        EXPECT_EQ(decoded.entries[i].data, analysis.entries[i].data);
    }
}

TEST(CollabTrackAnalysisTest, EncodesAnEmptyBundle) {
    CollabTrackAnalysis analysis;
    analysis.sampleRate = 48000;
    const QByteArray blob = encodeCollabTrackAnalysisBundle(analysis);
    ASSERT_FALSE(blob.isEmpty());
    CollabTrackAnalysis decoded;
    ASSERT_TRUE(decodeCollabTrackAnalysisBundle(blob, &decoded));
    EXPECT_EQ(decoded.sampleRate, 48000u);
    EXPECT_TRUE(decoded.entries.isEmpty());
}

TEST(CollabTrackAnalysisTest, RejectsBrokenBlobs) {
    const QByteArray blob = encodeCollabTrackAnalysisBundle(makeSampleAnalysis());
    CollabTrackAnalysis decoded;

    EXPECT_FALSE(decodeCollabTrackAnalysisBundle(QByteArray(), &decoded));
    EXPECT_FALSE(decodeCollabTrackAnalysisBundle(
            blob.left(blob.size() - 1), &decoded));

    QByteArray badVersion = blob;
    badVersion[0] = static_cast<char>(99);
    EXPECT_FALSE(decodeCollabTrackAnalysisBundle(badVersion, &decoded));
}

TEST(CollabTrackAnalysisTest, ZeroHashSentinelWorks) {
    CollabContentHash zero;
    zero.bytes = QByteArray(CollabContentHash::kSize, '\0');
    EXPECT_TRUE(zero.isValid());
    EXPECT_TRUE(zero.isZero());

    CollabContentHash nonzero;
    nonzero.bytes = QByteArray(CollabContentHash::kSize, '\0');
    nonzero.bytes[31] = '\1';
    EXPECT_TRUE(nonzero.isValid());
    EXPECT_FALSE(nonzero.isZero());

    EXPECT_FALSE(CollabContentHash().isZero());
}

} // namespace collab
} // namespace mixxx
