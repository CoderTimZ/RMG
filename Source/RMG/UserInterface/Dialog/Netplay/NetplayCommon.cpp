/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "NetplayCommon.hpp"

#include <QCryptographicHash>
#include <QByteArray>
#include <QFileInfo>
#include <QDateTime>

#include <RMG-Core/Core.hpp>

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
    json.insert("netplay_version", 16);
}

QList<QString> NetplayCommon::GetPluginNames(QString md5QString)
{
    QList<QString> pluginNames(2);
    const std::string md5 = md5QString.toStdString();
    const std::vector<CorePlugin> pluginList = CoreGetAllPlugins();
    const SettingsID settingsId[] =
    {
        SettingsID::Core_RSP_Plugin, SettingsID::Core_GFX_Plugin, 
    };
    const SettingsID gameSettingsId[] =
    {
        SettingsID::Game_RSP_Plugin, SettingsID::Game_GFX_Plugin, 
    };
    QString pluginFileNames[4];
    QString pluginFileName;

    int index = 0;

    for (int i = 0; i < 2; i++)
    {
        pluginFileName = QString::fromStdString(CoreSettingsGetStringValue(gameSettingsId[i], md5));
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
        index = ((int)p.Type - 1);

        if (pluginFileNames[index] == QString::fromStdString(p.File))
        {
            pluginNames[index] = QString::fromStdString(p.Name);
        }
    }

    return pluginNames;
}