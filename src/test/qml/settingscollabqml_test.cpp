#include <gtest/gtest.h>

#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <memory>

#include "test/mixxxtest.h"

namespace {

/// Loads the Online B2B diagnostics UI in the containers the real application
/// provides, so QML errors are caught without opening the whole skin.
class SettingsCollabQmlTest : public MixxxTest {
  protected:
    std::unique_ptr<QObject> load(const QString& source) {
        m_engine.addImportPath(QStringLiteral(RESOURCE_FOLDER "/qml"));

        QQmlComponent component(&m_engine);
        component.setData(source.toUtf8(),
                QUrl::fromLocalFile(QStringLiteral(
                        RESOURCE_FOLDER "/qml/settingscollabqml_test.qml")));

        std::unique_ptr<QObject> pRoot(component.create());
        EXPECT_FALSE(component.isError()) << qPrintable(component.errorString());
        EXPECT_TRUE(pRoot) << qPrintable(component.errorString());
        return pRoot;
    }

    static QString qmlDir() {
        return QUrl::fromLocalFile(QStringLiteral(RESOURCE_FOLDER "/qml"))
                .toString();
    }

    static QString settingsDir() {
        return QUrl::fromLocalFile(
                QStringLiteral(RESOURCE_FOLDER "/qml/Settings"))
                .toString();
    }

    QQmlEngine m_engine;
};

TEST_F(SettingsCollabQmlTest, PageLoadsInSettingsPopup) {
    load(QStringLiteral(R"(
import QtQuick
import Mixxx 1.0 as Mixxx
import "%1" as Settings

Mixxx.SettingParameterManager {
    Settings.Collab {
    }
}
)")
                    .arg(settingsDir()));
}

TEST_F(SettingsCollabQmlTest, DiagnosticsPopupLoads) {
    load(QStringLiteral(R"(
import QtQuick
import "%1" as App

App.CollabDiagnosticsPopup {
}
)")
                    .arg(qmlDir()));
}

TEST_F(SettingsCollabQmlTest, DiagnosticsPopupLoadsThroughLoader) {
    load(QStringLiteral(R"(
import QtQuick

Loader {
    active: true
    source: "%1/CollabDiagnosticsPopup.qml"
}
)")
                    .arg(qmlDir()));
}

} // namespace
