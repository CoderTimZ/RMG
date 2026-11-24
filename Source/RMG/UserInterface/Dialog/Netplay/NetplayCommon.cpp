/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "NetplayCommon.hpp"

#include <QCryptographicHash>
#include <QByteArray>
#include <QJsonArray>
#include <QFileInfo>
#include <QDateTime>
#include <QHostInfo>
#include <QUrl>

#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Plugins.hpp>
#include <RMG-Core/RomSettings.hpp>
#include <RMG-Core/CachedRomHeaderAndSettings.hpp>
#include <RMG-Core/Version.hpp>

using namespace NetplayCommon;

void NetplayCommon::AddCommonJson(QJsonObject& json)
{
    QCryptographicHash hash = QCryptographicHash(QCryptographicHash::Sha256);
    QByteArray currentTime = QByteArray::number(QDateTime::currentMSecsSinceEpoch());
    hash.addData(currentTime);
    hash.addData(QStringLiteral("RMG").toUtf8());

    json.insert("client_sha", QString::fromStdString(CoreGetVersion()));
    json.insert("emulator", "RMG");
    json.insert("auth", QString(hash.result().toHex()));
    json.insert("authTime", QString(currentTime));
    json.insert("netplay_version", 17);
}

QString NetplayCommon::GetGameSettingsSection(QString md5, QString file)
{
    CoreRomSettings settings;

    if (!file.isEmpty() &&
        CoreGetCachedRomHeaderAndSettings(file.toStdU32String(), nullptr, nullptr, nullptr, &settings))
    {
        return QString::fromStdString(CoreGetRomSettingsSection(settings));
    }

    return md5;
}

QList<QString> NetplayCommon::GetPluginNames(QString sectionQString)
{
    const std::string section = sectionQString.toStdString();
    const std::vector<CorePlugin> pluginList = CoreGetAllPlugins();
    const CorePluginType pluginTypes[] =
    {
        CorePluginType::Rsp, CorePluginType::Gfx, CorePluginType::Execution
    };
    const SettingsID settingsId[] =
    {
        SettingsID::Core_RSP_Plugin, SettingsID::Core_GFX_Plugin, SettingsID::Core_EXECUTION_Plugin
    };
    const SettingsID gameSettingsId[] =
    {
        SettingsID::Game_RSP_Plugin, SettingsID::Game_GFX_Plugin, SettingsID::Game_EXECUTION_Plugin
    };
    constexpr int count = sizeof(pluginTypes) / sizeof(pluginTypes[0]);
    QList<QString> pluginNames(count);
    QString pluginFileNames[count];
    QString pluginFileName;

    for (int i = 0; i < count; i++)
    {
        pluginFileName = QString::fromStdString(CoreSettingsGetStringValue(gameSettingsId[i], section));
        if (pluginFileName.isEmpty())
        { // fallback to main plugin settings
            pluginFileName = QString::fromStdString(CoreSettingsGetStringValue(settingsId[i]));
        }

        if (!pluginFileName.isEmpty())
        {
            // account for full path (<v0.3.5 we used the full path)
            pluginFileName = QFileInfo(pluginFileName).fileName();
            pluginFileNames[i] = pluginFileName;
        }
    }

    for (const auto &p : pluginList)
    {
        for (int i = 0; i < count; i++)
        {
            if (p.Type == pluginTypes[i] &&
                pluginFileNames[i] == QString::fromStdString(p.File))
            {
                pluginNames[i] = QString::fromStdString(p.Name);
            }
        }
    }

    // no execution plugin (sessions from RMG versions without
    // execution plugins don't say, which means the same)
    if (pluginNames[2].isEmpty())
    {
        pluginNames[2] = "(None)";
    }

    return pluginNames;
}

void NetplayCommon::AddServers(QComboBox* comboBox, const QJsonDocument& document)
{
    QJsonObject jsonObject  = document.object();
    QStringList jsonServers = jsonObject.keys();

    for (int i = 0; i < jsonServers.size(); i++)
    {
        comboBox->addItem(jsonServers.at(i), jsonObject.value(jsonServers.at(i)).toString());
    }

    NetplayCommon::RestoreSelectedServer(comboBox);
}

void NetplayCommon::RestoreSelectedServer(QComboBox* comboBox)
{
    QString server = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_SelectedServer));
    if (server.isEmpty())
    {
        return;
    }

    int index = comboBox->findText(server);
    if (index >= 0)
    {
        comboBox->setCurrentIndex(index);
    }
}

QString NetplayCommon::GetServerData(QComboBox* comboBox, int index)
{
    if (index == -1)
    {
        index = comboBox->currentIndex();
    }

    return comboBox->itemData(index).toString();
}

QNetworkRequest NetplayCommon::GetNetworkRequest(QUrl url)
{
    QNetworkRequest networkRequest(url);
    networkRequest.setRawHeader("netplay-id", "RMG");
    return networkRequest;
}
