#include "collab/collablibraryimport.h"

#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackset/crate/crate.h"
#include "library/trackset/crate/cratestorage.h"

namespace mixxx {
namespace collab {

bool importTrackIntoCollabCrate(
        Library* pLibrary, const TrackPointer& pTrack) {
    if (pLibrary == nullptr || !pTrack) {
        return false;
    }
    const TrackId trackId = pTrack->getId();
    if (!trackId.isValid()) {
        // Not in the library (yet); the load itself still works.
        return false;
    }
    TrackCollectionManager* pManager = pLibrary->trackCollectionManager();
    if (pManager == nullptr) {
        return false;
    }
    TrackCollection* pCollection = pManager->internalCollection();
    if (pCollection == nullptr) {
        return false;
    }

    const QString crateName = QString::fromLatin1(kCollabCrateName);
    Crate crate;
    CrateId crateId;
    if (pCollection->crates().readCrateByName(crateName, &crate)) {
        crateId = crate.getId();
    } else {
        crate.setName(crateName);
        if (!pCollection->insertCrate(crate, &crateId)) {
            return false;
        }
    }
    if (!crateId.isValid()) {
        return false;
    }
    return pCollection->addCrateTracks(crateId, QList<TrackId>{trackId});
}

} // namespace collab
} // namespace mixxx
