#pragma once

#include <QQmlApplicationEngine>
#include <QString>

namespace mixxx {
namespace qml {

/// Hosts the Online B2B diagnostics as a standalone QML window. Legacy
/// (QWidget) skins have no QML engine of their own, so this class creates one
/// and loads CollabDiagnosticsWindow.qml from the resource directory.
class CollabDiagnosticsWindow : public QObject {
    Q_OBJECT
  public:
    explicit CollabDiagnosticsWindow(
            const QString& resourcePath,
            QObject* pParent = nullptr);

    /// Shows the window, creating it on first use.
    void show();

    /// True when the window QML has been loaded successfully.
    bool isLoaded() const;

  private:
    QQmlApplicationEngine m_engine;
};

} // namespace qml
} // namespace mixxx
