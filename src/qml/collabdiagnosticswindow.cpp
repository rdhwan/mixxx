#include "qml/collabdiagnosticswindow.h"

#include <QDir>
#include <QQuickWindow>
#include <QUrl>
#include <QtDebug>
#include <memory>

namespace mixxx {
namespace qml {

namespace {
const QString kQmlDirName = QStringLiteral("qml");
const QString kWindowFileName = QStringLiteral("CollabDiagnosticsWindow.qml");
// The compiled QML modules (e.g. Mixxx) live under this resource prefix.
const QString kQmlImportResourcePath = QStringLiteral(":/mixxx.org/imports");
} // namespace

CollabDiagnosticsWindow::CollabDiagnosticsWindow(
        const QString& resourcePath,
        QObject* pParent)
        : QObject(pParent) {
    // Same import paths as the QML skins so that `import Mixxx 1.0` resolves.
    m_engine.addImportPath(kQmlImportResourcePath);
    m_engine.addImportPath(QDir(resourcePath).filePath(kQmlDirName));

    const QString windowPath = QDir(resourcePath).filePath(
            kQmlDirName + QLatin1Char('/') + kWindowFileName);
    m_engine.load(QUrl::fromLocalFile(windowPath));
    if (m_engine.rootObjects().isEmpty()) {
        qWarning() << "Failed to load the Online B2B diagnostics window from"
                   << windowPath;
    }
}

bool CollabDiagnosticsWindow::isLoaded() const {
    return !m_engine.rootObjects().isEmpty();
}

void CollabDiagnosticsWindow::show() {
    const QList<QObject*> rootObjects = m_engine.rootObjects();
    if (rootObjects.isEmpty()) {
        return;
    }
    auto* pWindow = qobject_cast<QQuickWindow*>(rootObjects.first());
    if (pWindow == nullptr) {
        qWarning() << "Online B2B diagnostics root object is not a window";
        return;
    }
    qDebug() << "Showing the Online B2B diagnostics window";
    pWindow->show();
    pWindow->raise();
    pWindow->requestActivate();
}

} // namespace qml
} // namespace mixxx
