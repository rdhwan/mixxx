#include <gtest/gtest.h>

#include <QFile>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QTest>
#include <gsl/pointers>

#include "collab/collabconnection.h"
#include "collab/collabprotocol.h"
#include "collab/collabsession.h"
#include "collab/collabtrackshare.h"
#include "collab/trackcache.h"
#include "collab/trackhash.h"
#include "collab/tracktransfer.h"
#include "control/controlindicatortimer.h"
#include "database/mixxxdb.h"
#include "effects/effectsmanager.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "engine/enginemixer.h"
#include "library/coverartcache.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/deck.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxdbtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"
#include "util/cmdlineargs.h"
#ifdef __RUBBERBAND__
#include "engine/bufferscalers/rubberbandworkerpool.h"
#endif

namespace {

const QString kTrackLocationTest1 = QStringLiteral("id3-test-data/cover-test-øé~ł€˚-png.mp3");
const QString kTrackLocationTest2 = QStringLiteral("id3-test-data/cover-test-øé~ł€˚-vbr.mp3");

void deleteTrack(Track* pTrack) {
    // Delete track objects directly in unit tests with
    // no main event loop
    delete pTrack;
};

void waitForTrackToBeLoaded(Deck* pDeck) {
    while (!pDeck->getEngineDeck()->getEngineBuffer()->isTrackLoaded()) {
        QTest::qSleep(100); // millis
    }
}

} // namespace

// We can't inherit from LibraryTest because that creates a key_notation control object that is also
// created by the Library object itself. The duplicated CO creation causes a debug assert.
class PlayerManagerTest : public MixxxDbTest, SoundSourceProviderRegistration {
  public:
    PlayerManagerTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        // This setup mirrors coreservices -- it would be nice if we could use coreservices instead
        // but it does a lot of local disk / settings setup.
        auto pChannelHandleFactory = std::make_shared<ChannelHandleFactory>();
        m_pEffectsManager = std::make_shared<EffectsManager>(m_pConfig, pChannelHandleFactory);
        m_pEngine = std::make_shared<EngineMixer>(
                m_pConfig,
                "[Master]",
                m_pEffectsManager.get(),
                pChannelHandleFactory,
                true);
        m_pSoundManager = std::make_shared<SoundManager>(m_pConfig, m_pEngine.get());
        m_pControlIndicatorTimer = std::make_shared<mixxx::ControlIndicatorTimer>(nullptr);
        m_pEngine->registerNonEngineChannelSoundIO(gsl::make_not_null(m_pSoundManager.get()));

        CoverArtCache::createInstance();

        m_pPlayerManager = std::make_shared<PlayerManager>(m_pConfig,
                m_pSoundManager.get(),
                m_pEffectsManager.get(),
                m_pEngine.get());

        m_pPlayerManager->addConfiguredDecks();
        m_pPlayerManager->addSampler();
        PlayerInfo::create();
        m_pEffectsManager->setup();

        const auto dbConnection = mixxx::DbConnectionPooled(dbConnectionPooler());
        if (!MixxxDb::initDatabaseSchema(dbConnection)) {
            exit(1);
        }
        m_pTrackCollectionManager = std::make_unique<TrackCollectionManager>(
                nullptr,
                m_pConfig,
                dbConnectionPooler(),
                deleteTrack);

        m_pRecordingManager = std::make_shared<RecordingManager>(m_pConfig, m_pEngine.get());
        m_pLibrary = std::make_shared<Library>(
                nullptr,
                m_pConfig,
                dbConnectionPooler(),
                m_pTrackCollectionManager.get(),
                m_pPlayerManager.get(),
                m_pRecordingManager.get());

        m_pPlayerManager->bindToLibrary(m_pLibrary.get());
#ifdef __RUBBERBAND__
        RubberBandWorkerPool::createInstance();
#endif
    }

    void TearDown() override {
        CoverArtCache::destroy();
#ifdef __RUBBERBAND__
        RubberBandWorkerPool::destroy();
#endif
    }

    ~PlayerManagerTest() {
        m_pSoundManager.reset();
        m_pPlayerManager.reset();
        PlayerInfo::destroy();
        m_pLibrary.reset();
        m_pRecordingManager.reset();
        m_pEngine.reset();
        m_pEffectsManager.reset();
        m_pTrackCollectionManager.reset();
        m_pControlIndicatorTimer.reset();
    }

  protected:
    TrackPointer getOrAddTrackByLocation(
            const QString& trackLocation) const {
        return m_pTrackCollectionManager->getOrAddTrack(
                TrackRef::fromFilePath(trackLocation));
    }

    std::shared_ptr<EffectsManager> m_pEffectsManager;
    std::shared_ptr<mixxx::ControlIndicatorTimer> m_pControlIndicatorTimer;
    std::shared_ptr<EngineMixer> m_pEngine;
    std::shared_ptr<SoundManager> m_pSoundManager;
    std::shared_ptr<PlayerManager> m_pPlayerManager;
    std::unique_ptr<TrackCollectionManager> m_pTrackCollectionManager;
    std::shared_ptr<RecordingManager> m_pRecordingManager;
    std::shared_ptr<Library> m_pLibrary;
};

TEST_F(PlayerManagerTest, UnEjectTest) {
    // Ejecting an empty deck with no previously-recorded ejected track has no effect.
    auto deck1 = m_pPlayerManager->getDeck(0);
    deck1->slotEjectTrack(1.0);
    ASSERT_EQ(nullptr, deck1->getLoadedTrack());

    // Load a track and eject it
    TrackPointer pTrack1 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest1));
    ASSERT_NE(nullptr, pTrack1);
    TrackId testId1 = pTrack1->getId();
    ASSERT_TRUE(testId1.isValid());
    deck1->slotLoadTrack(pTrack1,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            false);
    ASSERT_NE(nullptr, deck1->getLoadedTrack());

    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);
    // make sure eject does not trigger 'unreplace':
    // sleep for longer than 500 ms 'unreplace' period so this is not registered as double-click
    QTest::qSleep(kUnreplaceDelay); // millis
    deck1->slotEjectTrack(1.0);

    // Load another track.
    TrackPointer pTrack2 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest2));
    ASSERT_NE(nullptr, pTrack2);
    deck1->slotLoadTrack(pTrack2,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            false);

    // Ejecting in an empty deck loads the last-ejected track.
    auto deck2 = m_pPlayerManager->getDeck(1);
    ASSERT_EQ(nullptr, deck2->getLoadedTrack());
    // make sure eject does not trigger 'unreplace'
    QTest::qSleep(kUnreplaceDelay); // millis
    deck2->slotEjectTrack(2.0);
    ASSERT_NE(nullptr, deck2->getLoadedTrack());
    ASSERT_EQ(testId1, deck2->getLoadedTrack()->getId());
}

// Loading a new track in a deck causes the old one to be ejected.
// That old track can be unejected into a different deck.
TEST_F(PlayerManagerTest, UnEjectReplaceTrackTest) {
    auto deck1 = m_pPlayerManager->getDeck(0);
    // Load a track and the load another one
    TrackPointer pTrack1 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest1));
    ASSERT_NE(nullptr, pTrack1);
    TrackId testId1 = pTrack1->getId();
    ASSERT_TRUE(testId1.isValid());
    deck1->slotLoadTrack(pTrack1,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            false);
    ASSERT_NE(nullptr, deck1->getLoadedTrack());

    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);

    // Load another track, replacing the first, causing it to be unloaded.
    TrackPointer pTrack2 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest2));
    ASSERT_NE(nullptr, pTrack2);
    deck1->slotLoadTrack(pTrack2,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            false);
    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);

    // Ejecting in an empty deck loads the last-ejected track.
    auto deck2 = m_pPlayerManager->getDeck(1);
    ASSERT_EQ(nullptr, deck2->getLoadedTrack());
    // make sure eject does not trigger 'unreplace'
    QTest::qSleep(kUnreplaceDelay);
    deck2->slotEjectTrack(1.0);
    ASSERT_NE(nullptr, deck2->getLoadedTrack());
    ASSERT_EQ(testId1, deck2->getLoadedTrack()->getId());
}

TEST_F(PlayerManagerTest, UnEjectInvalidTrackIdTest) {
    // Save an invalid trackid in playermanager.
    auto pTrack = Track::newDummy(
            getTestDir().filePath(kTrackLocationTest1), TrackId(QVariant(10)));
    ASSERT_NE(nullptr, pTrack);
    m_pPlayerManager->slotSaveEjectedTrack(pTrack);
    auto deck1 = m_pPlayerManager->getDeck(0);
    // Does nothing -- no crash.
    // make sure eject does not trigger 'unreplace'
    QTest::qSleep(kUnreplaceDelay);
    deck1->slotEjectTrack(1.0);
    ASSERT_EQ(nullptr, deck1->getLoadedTrack());
}

TEST_F(PlayerManagerTest, UnReplaceTest) {
    // Trigger eject twice within 500 ms to undo track replacement
    auto deck1 = m_pPlayerManager->getDeck(0);
    // Load a track
    TrackPointer pTrack1 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest1));
    ASSERT_NE(nullptr, pTrack1);
    TrackId testId1 = pTrack1->getId();
    ASSERT_TRUE(testId1.isValid());
    deck1->slotLoadTrack(pTrack1,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            false);
    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);
    ASSERT_NE(nullptr, deck1->getLoadedTrack());

    // Load another track.
    TrackPointer pTrack2 = getOrAddTrackByLocation(getTestDir().filePath(kTrackLocationTest2));
    ASSERT_NE(nullptr, pTrack2);
    deck1->slotLoadTrack(pTrack2,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            false);
    m_pEngine->process(1024);
    waitForTrackToBeLoaded(deck1);
    ASSERT_NE(nullptr, deck1->getLoadedTrack());

    // Eject. Make sure eject does not trigger 'unreplace':
    // sleep for longer than 500 ms 'unreplace' period so this is not registered as double-click
    QTest::qSleep(kUnreplaceDelay); // millis
    deck1->slotEjectTrack(1.0);
    ASSERT_EQ(nullptr, deck1->getLoadedTrack());

    // Eject again, assume this is reached faster than 500 ms after first eject
    deck1->slotEjectTrack(1.0);
    // First track should be reloaded
    ASSERT_NE(nullptr, deck1->getLoadedTrack());
    ASSERT_EQ(testId1, deck1->getLoadedTrack()->getId());
}

// Loading a track on an owned deck asks the collaboration peer to load the
// same file. The peer here is a minimal CollabTrackShare that already has the
// file in its cache, so it can answer LOAD_READY without a transfer.
TEST_F(PlayerManagerTest, CollabOwnerLoadAsksPeerToLoad) {
    QTemporaryDir peerDir;
    ASSERT_TRUE(peerDir.isValid());

    mixxx::collab::CollabTrackCache peerCache(peerDir.filePath(QStringLiteral("cache")));
    QString error;
    ASSERT_TRUE(peerCache.ensureRoot(&error)) << error.toStdString();
    mixxx::collab::CollabTrackHashIndex peerIndex;
    mixxx::collab::CollabTransferService peerTransfer(&peerCache);
    ASSERT_TRUE(peerTransfer.listen(0, &error)) << error.toStdString();
    peerTransfer.setAllowedPeer(QHostAddress::LocalHost);
    mixxx::collab::CollabSessionTimeouts timeouts;
    timeouts.handshakeMs = 2000;
    timeouts.handshakeRetryMs = 50;
    timeouts.keepaliveMs = 100;
    timeouts.peerTimeoutMs = 500;
    mixxx::collab::CollabSession peerSession;
    peerSession.setTimeouts(timeouts);
    mixxx::collab::CollabTrackShare peerShare(
            &peerSession, &peerTransfer, &peerIndex, &peerCache);
    QObject::connect(&peerSession,
            &mixxx::collab::CollabSession::packetReceived,
            &peerShare,
            [&peerShare](const mixxx::collab::CollabPacket& packet) {
                peerShare.handlePacket(packet);
            });

    // The file the owner will load, already installed in the peer cache. It
    // must be decodable audio, because the engine has to load it to emit
    // newTrackLoaded.
    const QString source =
            getTestDir().filePath(QStringLiteral("id3-test-data/all.mp3"));
    mixxx::collab::CollabContentHash hash;
    ASSERT_TRUE(mixxx::collab::collabHashFile(source, &hash));
    const QString partial = peerCache.partialPathForHash(hash);
    {
        QFile sourceFile(source);
        ASSERT_TRUE(sourceFile.open(QIODevice::ReadOnly));
        QFile partialFile(partial);
        ASSERT_TRUE(partialFile.open(QIODevice::WriteOnly));
        ASSERT_EQ(partialFile.write(sourceFile.readAll()),
                sourceFile.size());
    }
    ASSERT_TRUE(peerCache.install(hash, partial, QStringLiteral("mp3")));

    mixxx::collab::CollabConnection* pHost =
            mixxx::collab::CollabConnection::shared();
    pHost->setControlReplicationEnabled(false);
    mixxx::collab::CollabTrackSharingConfig sharing;
    sharing.cacheRoot = QDir(m_pConfig->getSettingsPath())
                                .filePath(QStringLiteral("collab-test-cache"));
    pHost->setTrackSharingConfig(sharing);

    const mixxx::collab::CollabSessionToken token =
            mixxx::collab::makeCollabSessionToken();
    ASSERT_TRUE(pHost->host(0, token, QString()));
    ASSERT_TRUE(peerSession.join(
            QHostAddress::LocalHost, pHost->localPort(), 0, token));
    const auto bothConnected = [&]() {
        return pHost->isConnected() &&
                peerSession.state() ==
                mixxx::collab::CollabSession::State::Connected;
    };
    for (int i = 0; i < 250 && !bothConnected() && !HasFatalFailure(); ++i) {
        QTest::qWait(20);
    }
    ASSERT_TRUE(bothConnected());

    bool peerAsked = false;
    bool ownerCompleted = false;
    QObject::connect(&peerShare,
            &mixxx::collab::CollabTrackShare::loadRequested,
            [&](int deck, const mixxx::collab::CollabContentHash& requestedHash, const QString&) {
                peerAsked = true;
                EXPECT_EQ(deck, 1);
                EXPECT_EQ(requestedHash, hash);
                peerShare.reportLoadResult(deck, requestedHash, true); });
    QObject::connect(pHost,
            &mixxx::collab::CollabConnection::trackLoadCompleted,
            [&](int deck, const mixxx::collab::CollabContentHash&) {
                ownerCompleted = true;
                EXPECT_EQ(deck, 1);
            });

    // Load the track on deck 1, which the host owns.
    TrackPointer pTrack = getOrAddTrackByLocation(source);
    ASSERT_TRUE(pTrack);
    m_pPlayerManager->getDeck(0)
            ->slotLoadTrack(pTrack,
#ifdef __STEM__
                    mixxx::StemChannelSelection(),
#endif
                    false);

    // newTrackLoaded is emitted from EngineBuffer::trackLoaded, which the
    // engine delivers while processing buffers, so drive the engine here.
    for (int i = 0; i < 250 && !ownerCompleted && !HasFatalFailure(); ++i) {
        m_pEngine->process(1024);
        QTest::qWait(20);
    }
    EXPECT_TRUE(peerAsked);
    EXPECT_TRUE(ownerCompleted);
    EXPECT_FALSE(pHost->hasPendingTrackLoad(1));

    pHost->leave();
    peerSession.close();
}
