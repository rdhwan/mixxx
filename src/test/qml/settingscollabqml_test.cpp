#include <gtest/gtest.h>

#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <memory>

#include "test/mixxxtest.h"

namespace {

/// Loads the Online B2B settings page inside a setting-parameter manager, which
/// is the container the real settings popup provides. This catches QML errors
/// in the page without having to open the whole settings dialog.
class SettingsCollabQmlTest : public MixxxTest {
  protected:
    QQmlEngine m_engine;
};

TEST_F(SettingsCollabQmlTest, PageLoads) {
    m_engine.addImportPath(QStringLiteral(RESOURCE_FOLDER "/qml"));

    const QString settingsDir = QUrl::fromLocalFile(
            QStringLiteral(RESOURCE_FOLDER "/qml/Settings"))
                                        .toString();
    const QString qml = QStringLiteral(R"(
import QtQuick
import Mixxx 1.0 as Mixxx
import "%1" as Settings

Mixxx.SettingParameterManager {
    Settings.Collab {
    }
}
)")
                                .arg(settingsDir);

    QQmlComponent component(&m_engine);
    component.setData(qml.toUtf8(),
            QUrl::fromLocalFile(QStringLiteral(
                    RESOURCE_FOLDER "/qml/settingscollabqml_test.qml")));

    std::unique_ptr<QObject> pRoot(component.create());
    ASSERT_FALSE(component.isError()) << qPrintable(component.errorString());
    ASSERT_TRUE(pRoot) << qPrintable(component.errorString());
}

} // namespace
