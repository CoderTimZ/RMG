/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "NetplaySessionWindow.hpp"

#include "Utilities/QtMessageBox.hpp"

#include <RMG-Core/CachedRomHeaderAndSettings.hpp>
#include <RMG-Core/Cheats.hpp>
#include <RMG-Core/NetplayNew.hpp>
#include <RMG-Core/Plugins.hpp>
#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Version.hpp>
#include <RMG-Netplay-New/Packet.hpp>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPointer>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>

using namespace UserInterface::Dialog;
using namespace Utilities;

namespace
{
enum Column
{
    ColumnPlayer = 0,
    ColumnPort1,
    ColumnPort2,
    ColumnPort3,
    ColumnPort4,
    ColumnLag,
    ColumnPing,
    ColumnInputBy,
    ColumnStatus,
    ColumnCount
};

//
// Cheats travel as the host's enabled cheats, each with its chosen option
//

std::vector<CoreCheat> get_enabled_cheats(QString romFile)
{
    std::vector<CoreCheat> cheats;
    std::vector<CoreCheat> enabled;
    std::filesystem::path file = romFile.toStdU32String();

    if (!CoreGetCurrentCheats(file, cheats))
    {
        return enabled;
    }

    for (CoreCheat cheat : cheats)
    {
        if (!CoreIsCheatEnabled(file, cheat))
        {
            continue;
        }

        if (cheat.HasOptions)
        {
            CoreCheatOption option;
            if (!CoreGetCheatOption(file, cheat, option))
            {
                continue;
            }
            cheat.CheatOptions = {option};
        }
        else
        {
            cheat.CheatOptions.clear();
        }

        enabled.push_back(cheat);
    }

    return enabled;
}

std::vector<uint8_t> serialize_cheats(const std::vector<CoreCheat>& cheats)
{
    NetplayNew::PacketWriter writer;
    writer.U32(static_cast<uint32_t>(cheats.size()));
    for (const CoreCheat& cheat : cheats)
    {
        writer.String(cheat.Name).U8(cheat.HasOptions ? 1 : 0);
        writer.U32(static_cast<uint32_t>(cheat.CheatOptions.size()));
        for (const CoreCheatOption& option : cheat.CheatOptions)
        {
            writer.String(option.Name).U32(option.Value).U32(static_cast<uint32_t>(option.Size));
        }
        writer.U32(static_cast<uint32_t>(cheat.CheatCodes.size()));
        for (const CoreCheatCode& code : cheat.CheatCodes)
        {
            writer.U32(code.Address)
                .U32(static_cast<uint32_t>(code.Value))
                .U8(code.UseOptions ? 1 : 0)
                .U32(static_cast<uint32_t>(code.OptionIndex))
                .U32(static_cast<uint32_t>(code.OptionSize));
        }
    }
    return writer.Data();
}

bool deserialize_cheats(const std::vector<uint8_t>& data, std::vector<CoreCheat>& cheats)
{
    try
    {
        NetplayNew::PacketReader reader(data.data(), data.size());
        uint32_t count = reader.U32();
        for (uint32_t i = 0; i < count; i++)
        {
            CoreCheat cheat;
            cheat.Name       = reader.String();
            cheat.HasOptions = reader.U8() != 0;
            uint32_t options = reader.U32();
            for (uint32_t j = 0; j < options; j++)
            {
                CoreCheatOption option;
                option.Name  = reader.String();
                option.Value = reader.U32();
                option.Size  = static_cast<int32_t>(reader.U32());
                cheat.CheatOptions.push_back(option);
            }
            uint32_t codes = reader.U32();
            for (uint32_t j = 0; j < codes; j++)
            {
                CoreCheatCode code;
                code.Address     = reader.U32();
                code.Value       = static_cast<int32_t>(reader.U32());
                code.UseOptions  = reader.U8() != 0;
                code.OptionIndex = static_cast<int>(reader.U32());
                code.OptionSize  = static_cast<int>(reader.U32());
                cheat.CheatCodes.push_back(code);
            }
            cheats.push_back(cheat);
        }
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}
} // namespace

NetplaySessionWindow::NetplaySessionWindow(QWidget* parent, const QMap<QString, CoreRomSettings>& roms)
    : QDialog(parent), roms(roms)
{
    this->setWindowTitle("Netplay Session");
    this->setWindowFlag(Qt::WindowMinimizeButtonHint, true);
    this->resize(760, 460);

    QPointer<NetplaySessionWindow> self(this);
    this->session = std::make_unique<NetplayNew::Session>([self](const NetplayNew::Event& event) {
        // session events arrive on the network and emulation threads
        QMetaObject::invokeMethod(self.data(), [self, event]() {
            if (self)
            {
                self->handleEvent(event);
            }
        }, Qt::QueuedConnection);
    });

    this->gameLabel = new QLabel(this);
    this->gameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    this->addressLabel = new QLabel(this);
    this->addressLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->addressLabel->setWordWrap(true);

    this->playerTable = new QTableWidget(0, ColumnCount, this);
    this->playerTable->setHorizontalHeaderLabels({"Player", "P1", "P2", "P3", "P4", "Lag", "Ping", "Input by", "Status"});
    this->playerTable->verticalHeader()->setVisible(false);
    this->playerTable->setSelectionMode(QAbstractItemView::NoSelection);
    this->playerTable->horizontalHeader()->setSectionResizeMode(ColumnPlayer, QHeaderView::Stretch);
    this->playerTable->horizontalHeader()->setSectionResizeMode(ColumnStatus, QHeaderView::Stretch);
    for (int column = ColumnPort1; column <= ColumnInputBy; column++)
    {
        this->playerTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    this->playerTable->horizontalHeaderItem(ColumnLag)->setToolTip("Input delay in frames, set automatically from the measured pings (1 frame is about 17 ms)");
    this->playerTable->horizontalHeaderItem(ColumnPing)->setToolTip("Round trip time to the host");
    this->playerTable->horizontalHeaderItem(ColumnInputBy)->setToolTip("Whose computer writes this player's input during the game");

    this->modeComboBox = new QComboBox(this);
    this->modeComboBox->setToolTip("Lockstep: everyone sends their own input, and one player lagging lags everyone.\n"
                                   "Rollback: like Lockstep, but other players' input is guessed instead of waited for, and the\n"
                                   "game goes back to correct a wrong guess, so there's less input delay. With an execution plugin,\n"
                                   "only when the plugin can save its state.\n"
                                   "Authority: one player sends everyone's input; only players who lag fall behind.\n"
                                   "Golf: like Authority, but whoever starts playing becomes the authority.");
    this->pauseButton = new QPushButton("Pause", this);
    this->pauseButton->setToolTip("Pauses the game for everyone");

    this->startReasonLabel = new QLabel(this);
    this->startReasonLabel->setWordWrap(true);

    this->startButton = new QPushButton("Start", this);
    this->leaveButton = new QPushButton("Leave", this);

    this->logList = new QListWidget(this);
    this->logList->setMaximumHeight(110);

    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addWidget(new QLabel("Mode:", this));
    buttons->addWidget(this->modeComboBox);
    buttons->addWidget(this->startReasonLabel, 1);
    buttons->addWidget(this->pauseButton);
    buttons->addWidget(this->startButton);
    buttons->addWidget(this->leaveButton);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(this->gameLabel);
    layout->addWidget(this->addressLabel);
    layout->addWidget(this->playerTable, 1);
    layout->addLayout(buttons);
    layout->addWidget(this->logList);

    connect(this->modeComboBox, &QComboBox::activated, this, &NetplaySessionWindow::on_modeComboBox_activated);
    connect(this->pauseButton, &QPushButton::clicked, this, &NetplaySessionWindow::on_pauseButton_clicked);
    connect(this->startButton, &QPushButton::clicked, this, &NetplaySessionWindow::on_startButton_clicked);
    connect(this->leaveButton, &QPushButton::clicked, this, &NetplaySessionWindow::on_leaveButton_clicked);

    connect(this->playerTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (this->updatingTable || item->column() < ColumnPort1 || item->column() > ColumnPort4)
        {
            return;
        }
        int row = item->row();
        uint8_t player = static_cast<uint8_t>(this->playerTable->item(row, ColumnPlayer)->data(Qt::UserRole).toUInt());
        uint8_t ports  = 0;
        for (int i = 0; i < 4; i++)
        {
            QTableWidgetItem* portItem = this->playerTable->item(row, ColumnPort1 + i);
            if (portItem != nullptr && portItem->checkState() == Qt::Checked)
            {
                ports |= static_cast<uint8_t>(1 << i);
            }
        }
        this->session->SetPorts(player, ports);
    });

    // pings change all the time
    QTimer* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &NetplaySessionWindow::refresh);
    timer->start(1000);
}

NetplaySessionWindow::~NetplaySessionWindow(void)
{
    // leaving joins the network thread, so no event arrives after this
    this->session->Leave();
}

NetplayNew::LocalInfo NetplaySessionWindow::localInfo(QString nickname, QString romFile)
{
    NetplayNew::LocalInfo info;
    info.name            = nickname.toStdString();
    info.version         = CoreGetVersion();
    info.rspPlugin       = CoreSettingsGetStringValue(SettingsID::Core_RSP_Plugin);
    info.gfxPlugin       = CoreSettingsGetStringValue(SettingsID::Core_GFX_Plugin);
    info.executionPlugin = CoreSettingsGetStringValue(SettingsID::Core_EXECUTION_Plugin);

    // a game can override the plugins
    if (!romFile.isEmpty() && this->roms.contains(romFile))
    {
        const std::string section = CoreGetRomSettingsSection(this->roms.value(romFile));
        auto override_with = [&](SettingsID id, std::string& plugin) {
            std::string value = CoreSettingsGetStringValue(id, section);
            if (!value.empty())
            {
                plugin = value;
            }
        };
        override_with(SettingsID::Game_RSP_Plugin, info.rspPlugin);
        override_with(SettingsID::Game_GFX_Plugin, info.gfxPlugin);
        override_with(SettingsID::Game_EXECUTION_Plugin, info.executionPlugin);
    }

    if (info.executionPlugin == "(None)")
    {
        info.executionPlugin.clear();
    }

    info.rollbackProblem = CoreGetNetplayNewRollbackProblem(info.rspPlugin, info.gfxPlugin);

    // an execution plugin keeps state of its own, which rollback can only
    // restore when the plugin saves it
    if (info.rollbackProblem.empty() && !info.executionPlugin.empty() &&
        !CoreNetplayNewExecutionPluginSavesState(info.executionPlugin))
    {
        info.rollbackProblem = "execution plugin can't save its state for rollback";
    }
    return info;
}

bool NetplaySessionWindow::Host(QString nickname, QString romFile, QString gameName, QString gameMD5, QString gameInternalName,
                                uint16_t port, bool mapPort, QString& error)
{
    std::string errorString;
    this->romFile = romFile;

    // (the core refuses them too; the guests find the same file by its MD5)
    CoreRomType type = CoreRomType::Cartridge;
    if (CoreGetCachedRomHeaderAndSettings(romFile.toStdU32String(), &type, nullptr, nullptr, nullptr) &&
        type == CoreRomType::Disk)
    {
        error = "64DD disks aren't supported in netplay";
        return false;
    }

    if (!this->session->Host(port, this->localInfo(nickname, romFile), gameName.toStdString(), gameMD5.toStdString(),
                             gameInternalName.toStdString(), errorString))
    {
        error = QString::fromStdString(errorString);
        return false;
    }

    QStringList lanAddresses;
    QStringList ipv6Addresses;
    for (const NetplayNew::LocalAddress& address : NetplayNew::Transport::GetLocalAddresses())
    {
        QString text = QString::fromStdString(address.WithPort(port));
        (address.global ? ipv6Addresses : lanAddresses).append(text);
    }

    QString text = "On your network, players join with " +
                   (lanAddresses.isEmpty() ? QString("your IP address and port %1").arg(port) : lanAddresses.join(" or ")) + ".";
    if (!ipv6Addresses.isEmpty())
    {
        text += "\nOver the internet, players with IPv6 join with " + ipv6Addresses.join(" or ") +
                QString(" (your router's firewall may need to allow UDP port %1).").arg(port);
    }
    this->addressText = text;
    this->addressLabel->setText(text);

    if (mapPort)
    {
        this->session->MapPort();
    }

    this->log("Hosting " + gameName + " on UDP port " + QString::number(port));
    if (this->session->GetInfo().mode == NetplayNew::Mode::Golf)
    {
        this->log("This is a golf game, so the session starts in Golf mode");
    }
    this->refresh();
    return true;
}

bool NetplaySessionWindow::Join(QString nickname, QString address, uint16_t port, QString& error)
{
    std::string errorString;
    this->nickname = nickname;
    if (!this->session->Join(address.toStdString(), port, this->localInfo(nickname, QString()), errorString))
    {
        error = QString::fromStdString(errorString);
        return false;
    }

    this->addressLabel->setText("Host: " + (address.contains(':') ? "[" + address + "]" : address) + ":" + QString::number(port));
    this->log("Connecting to " + (address.contains(':') ? "[" + address + "]" : address) + ":" + QString::number(port) + "...");
    this->refresh();
    return true;
}

bool NetplaySessionWindow::HasEnded(void) const
{
    return this->ended;
}

#ifdef NETPLAY_NEW_TESTS
void NetplaySessionWindow::SetAutoStart(int players, int mode, int authority)
{
    this->autoStartPlayers   = players;
    this->autoStartMode      = mode;
    this->autoStartAuthority = authority;
}
#endif // NETPLAY_NEW_TESTS

void NetplaySessionWindow::OnGameStopped(void)
{
    CoreSetNetplayNewCallbacks(nullptr);
    this->session->GameStopped();
    this->refresh();
}

void NetplaySessionWindow::reject(void)
{
    this->close();
}

void NetplaySessionWindow::closeEvent(QCloseEvent* event)
{
    if (!this->ended)
    {
        NetplayNew::SessionInfo info = this->session->GetInfo();
        if (info.playing)
        {
            const NetplayNew::PlayerInfo* me = info.Find(info.myId);
            QString question = info.isHost ? "Leaving ends the game for everyone. Leave the session?"
                             : (me != nullptr && me->ports != 0)
                                 ? "The others keep playing, with your controller idle. Leave the session?"
                                 : "Stop watching and leave the session?";
            QMessageBox::StandardButton answer = QMessageBox::question(this, "Leave Netplay Session", question);
            if (answer != QMessageBox::Yes)
            {
                event->ignore();
                return;
            }
        }
        this->leave();
    }
    else
    {
        emit this->Left();
    }

    event->accept();
}

void NetplaySessionWindow::handleEvent(const NetplayNew::Event& event)
{
    QString text = QString::fromStdString(event.text);

    switch (event.type)
    {
    case NetplayNew::EventType::Connected:
        this->log("Connected to the host");
        break;

    case NetplayNew::EventType::StateChanged:
        break;

    case NetplayNew::EventType::GameStarting:
    {
        NetplayNew::SessionInfo info = this->session->GetInfo();
        QString file = info.isHost ? this->romFile : this->findRom(QString::fromStdString(info.gameMD5));

        std::vector<CoreCheat> cheats;
        if (file.isEmpty() || !deserialize_cheats(event.cheats, cheats))
        {
            this->log(file.isEmpty() ? "Couldn't find the game on this computer" : "Couldn't read the host's cheats");
            this->session->GameStopped();
            break;
        }

        CoreSetNetplayCheats(cheats);
        if (!CoreSetNetplayNewCallbacks(this->session->GetCallbacks()))
        {
            this->log("Couldn't attach the session to the emulator");
            this->session->GameStopped();
            break;
        }

        this->log("Starting the game" + (cheats.empty() ? QString() : QString(" with %1 cheat(s)").arg(cheats.size())));
        emit this->PlayGame(file);
        break;
    }

    case NetplayNew::EventType::GameEnded:
        this->log("The game ended: " + text);
        emit this->ShowMessage("Netplay: " + text);
        break;

    case NetplayNew::EventType::Waiting:
        emit this->ShowMessage("Waiting for " + text + "...");
        break;

    case NetplayNew::EventType::Paused:
        this->log(text + " paused the game");
        emit this->ShowMessage("Paused by " + text);
        break;

    case NetplayNew::EventType::Resumed:
        this->log(text + " resumed the game");
        break;

    case NetplayNew::EventType::Desync:
        this->log("Desync: " + text);
        emit this->ShowMessage("Netplay desync: " + text);
        break;

    case NetplayNew::EventType::Message:
        this->log(text);
        if (this->session->GetInfo().playing)
        {
            emit this->ShowMessage("Netplay: " + text);
        }
        break;

    case NetplayNew::EventType::Ended:
        this->ended = true;
        this->log("The session ended: " + text);
        emit this->ShowMessage("Netplay: " + text);
        if (!this->isVisible())
        {
            this->show();
        }
        break;
    }

    this->refresh();
}

void NetplaySessionWindow::refresh(void)
{
    NetplayNew::SessionInfo info = this->session->GetInfo();
    bool isHost = info.isHost && !this->ended;

    // the host's addresses, with what the router said
    if (info.isHost && !this->addressText.isEmpty())
    {
        QString ipv4;
        QString port = QString::number(info.port);
        switch (info.portMapping)
        {
        case NetplayNew::SessionInfo::PortMapping::Off:
            ipv4 = "Players elsewhere join with your public IPv4 address and port " + port +
                   ", which needs the UDP port forwarded to this computer.";
            break;
        case NetplayNew::SessionInfo::PortMapping::Pending:
            ipv4 = "Asking your router to forward UDP port " + port + "...";
            break;
        case NetplayNew::SessionInfo::PortMapping::Mapped:
            ipv4 = "Your router forwards UDP port " + port + " (" + QString::fromStdString(info.portMappingMethod) +
                   "): players elsewhere join with " +
                   (info.publicAddress.empty() ? QString("your public IPv4 address") : QString::fromStdString(info.publicAddress)) +
                   ":" + port + ".";
            if (!info.portMappingMessage.empty())
            {
                ipv4 += " Note: " + QString::fromStdString(info.portMappingMessage) + ".";
            }
            break;
        case NetplayNew::SessionInfo::PortMapping::Failed:
            ipv4 = "Couldn't get your router to forward the port (" + QString::fromStdString(info.portMappingMessage) +
                   "). For players elsewhere, forward UDP port " + port + " to this computer in the router's settings.";
            break;
        }
        this->addressLabel->setText(this->addressText + "\n" + ipv4);
    }

    this->gameLabel->setText(info.gameName.empty() ? QString("Game: waiting for the host...")
                                                   : "Game: " + QString::fromStdString(info.gameName) +
                                                         (info.playing ? "  (playing)" : ""));

    // guests tell the host whether they have the game, once they know which it is
    if (!info.isHost && !this->sentHasGame && !info.gameMD5.empty() && info.connected)
    {
        QString file = this->findRom(QString::fromStdString(info.gameMD5));
        bool hasGame = !file.isEmpty();
        this->session->SetHasGame(hasGame, this->localInfo(this->nickname, file));
        this->sentHasGame = true;
        if (!hasGame)
        {
            this->log("You don't have " + QString::fromStdString(info.gameName) +
                      " in your ROM browser, so you can't play this session");
        }
    }

    const NetplayNew::PlayerInfo* host = info.Find(NetplayNew::HostPlayer);

    this->updatingTable = true;

    // rebuild the rows only when players come or go, so editing isn't interrupted
    bool sameRows = this->playerTable->rowCount() == static_cast<int>(info.players.size());
    for (int row = 0; sameRows && row < this->playerTable->rowCount(); row++)
    {
        QTableWidgetItem* item = this->playerTable->item(row, ColumnPlayer);
        sameRows = item != nullptr && item->data(Qt::UserRole).toUInt() == info.players[row].id;
    }
    if (!sameRows)
    {
        this->playerTable->setRowCount(0);
        this->playerTable->setRowCount(static_cast<int>(info.players.size()));
    }

    // ports only change between games
    bool editable = isHost && !info.playing;

    for (int row = 0; row < static_cast<int>(info.players.size()); row++)
    {
        const NetplayNew::PlayerInfo& player = info.players[row];

        QString name = QString::fromStdString(player.name);
        if (player.id == NetplayNew::HostPlayer)
        {
            name += " (host)";
        }
        if (player.id == info.myId)
        {
            name += " (you)";
        }
        if (player.ports == 0)
        {
            name += " - spectating";
        }

        QTableWidgetItem* nameItem = this->playerTable->item(row, ColumnPlayer);
        if (nameItem == nullptr)
        {
            nameItem = new QTableWidgetItem();
            nameItem->setFlags(Qt::ItemIsEnabled);
            this->playerTable->setItem(row, ColumnPlayer, nameItem);
        }
        nameItem->setText(name);
        nameItem->setData(Qt::UserRole, player.id);

        for (int i = 0; i < 4; i++)
        {
            QTableWidgetItem* portItem = this->playerTable->item(row, ColumnPort1 + i);
            if (portItem == nullptr)
            {
                portItem = new QTableWidgetItem();
                this->playerTable->setItem(row, ColumnPort1 + i, portItem);
            }
            Qt::ItemFlags flags = Qt::ItemIsUserCheckable;
            if (editable)
            {
                flags |= Qt::ItemIsEnabled;
            }
            portItem->setFlags(flags);
            portItem->setCheckState((player.ports & (1 << i)) ? Qt::Checked : Qt::Unchecked);
            portItem->setToolTip(QString("N64 controller port %1").arg(i + 1));
        }

        // the lag is set automatically (see Session::AdjustLagLocked); shown for information
        {
            QTableWidgetItem* lagItem = this->playerTable->item(row, ColumnLag);
            if (lagItem == nullptr)
            {
                lagItem = new QTableWidgetItem();
                lagItem->setFlags(Qt::ItemIsEnabled);
                this->playerTable->setItem(row, ColumnLag, lagItem);
            }
            bool lagMatters = player.ports != 0 && NetplayNew::OwnersWrite(info.mode);
            lagItem->setText(lagMatters ? QString::number(player.lag) : QString("-"));
            lagItem->setToolTip(NetplayNew::OwnersWrite(info.mode) ? QString() : "Lag is only used in Lockstep and Rollback modes");
        }

        QTableWidgetItem* pingItem = this->playerTable->item(row, ColumnPing);
        if (pingItem == nullptr)
        {
            pingItem = new QTableWidgetItem();
            pingItem->setFlags(Qt::ItemIsEnabled);
            this->playerTable->setItem(row, ColumnPing, pingItem);
        }
        pingItem->setText(player.id == NetplayNew::HostPlayer ? QString("-") : QString("%1 ms").arg(player.ping));

        QString inputBy = "-";
        auto authority = info.inputBy.find(player.id);
        if (authority != info.inputBy.end())
        {
            const NetplayNew::PlayerInfo* writer = info.Find(authority->second);
            inputBy = authority->second == player.id ? QString("themself")
                    : writer != nullptr ? QString::fromStdString(writer->name)
                                        : QString("?");
        }
        QTableWidgetItem* inputByItem = this->playerTable->item(row, ColumnInputBy);
        if (inputByItem == nullptr)
        {
            inputByItem = new QTableWidgetItem();
            inputByItem->setFlags(Qt::ItemIsEnabled);
            this->playerTable->setItem(row, ColumnInputBy, inputByItem);
        }
        inputByItem->setText(inputBy);

        // compared with the host
        QString status = "✓ Ready";
        QString statusTip;
        auto desync = info.desyncs.find(player.id);
        if (desync != info.desyncs.end())
        {
            status = QString("⚠ Desynced at frame %1").arg(desync->second);
            statusTip = "This player's game drifted apart from the host's: what happens on their screen no longer "
                        "matches. Different plugins or settings are the usual cause.";
        }
        else if (!player.hasGame)
        {
            status = "✗ Doesn't have the game";
            statusTip = "The game has to be in this player's ROM browser, with the same MD5 as the host's.";
        }
        else if (host != nullptr && player.version != host->version)
        {
            status = "✗ Different RMG version";
            statusTip = "Host: " + QString::fromStdString(host->version) + "\nThis player: " + QString::fromStdString(player.version);
        }
        else if (host != nullptr && player.executionPlugin != host->executionPlugin)
        {
            auto name = [](const std::string& plugin) { return plugin.empty() ? QString("none") : QString::fromStdString(plugin); };
            status = "✗ Different execution plugin";
            statusTip = "Execution plugins change the game, so everyone needs the same one.\nHost: " +
                        name(host->executionPlugin) + "\nThis player: " + name(player.executionPlugin);
        }
        else if (host != nullptr && (player.rspPlugin != host->rspPlugin || player.gfxPlugin != host->gfxPlugin))
        {
            status = "⚠ Different plugins";
            statusTip = "Different RSP or video plugins can make the game run differently for this player (a desync).\n"
                        "Host: " + QString::fromStdString(host->rspPlugin) + ", " + QString::fromStdString(host->gfxPlugin) +
                        "\nThis player: " + QString::fromStdString(player.rspPlugin) + ", " + QString::fromStdString(player.gfxPlugin);
        }
        QTableWidgetItem* statusItem = this->playerTable->item(row, ColumnStatus);
        if (statusItem == nullptr)
        {
            statusItem = new QTableWidgetItem();
            statusItem->setFlags(Qt::ItemIsEnabled);
            this->playerTable->setItem(row, ColumnStatus, statusItem);
        }
        statusItem->setText(status);
        statusItem->setToolTip(statusTip);
    }

    this->updatingTable = false;

    // mode: the host can change it at any time, also during a game
    // (not rebuilt while its list is open, which the refresh timer would close)
    if (!this->modeComboBox->view()->isVisible())
    {
        QSignalBlocker blocker(this->modeComboBox);
        this->modeComboBox->clear();
        this->modeComboBox->addItem("Lockstep", QVariant::fromValue(QPair<int, int>(int(NetplayNew::Mode::Lockstep), NetplayNew::HostPlayer)));
        this->modeComboBox->addItem("Rollback", QVariant::fromValue(QPair<int, int>(int(NetplayNew::Mode::Rollback), NetplayNew::HostPlayer)));
        {
            // (unavailable, with the reason, when a player's setup can't roll back)
            std::string rollbackReason;
            if (!this->session->CanRollback(rollbackReason))
            {
                int index = this->modeComboBox->count() - 1;
                QStandardItemModel* model = qobject_cast<QStandardItemModel*>(this->modeComboBox->model());
                if (model != nullptr && info.mode != NetplayNew::Mode::Rollback)
                {
                    model->item(index)->setEnabled(false);
                }
                this->modeComboBox->setItemText(index, "Rollback (unavailable)");
                this->modeComboBox->setItemData(index, QString::fromStdString(rollbackReason), Qt::ToolTipRole);
            }
        }
        this->modeComboBox->addItem("Golf", QVariant::fromValue(QPair<int, int>(int(NetplayNew::Mode::Golf), NetplayNew::HostPlayer)));
        for (const NetplayNew::PlayerInfo& player : info.players)
        {
            // not a spectator: they can close their game at any time, and
            // everyone would wait for them (the host's closing it ends it anyway)
            if (player.ports == 0 && player.id != NetplayNew::HostPlayer)
            {
                continue;
            }
            this->modeComboBox->addItem("Authority: " + QString::fromStdString(player.name),
                                        QVariant::fromValue(QPair<int, int>(int(NetplayNew::Mode::Authority), player.id)));
        }
        for (int i = 0; i < this->modeComboBox->count(); i++)
        {
            auto [mode, player] = this->modeComboBox->itemData(i).value<QPair<int, int>>();
            if (mode == int(info.mode) && (info.mode != NetplayNew::Mode::Authority || player == info.authorityPlayer))
            {
                this->modeComboBox->setCurrentIndex(i);
                break;
            }
        }
        this->modeComboBox->setEnabled(isHost);
    }

    this->pauseButton->setVisible(info.playing && !this->ended);
    this->pauseButton->setText(info.paused ? "Resume" : "Pause");

    // buttons
    std::string reason;
    bool canStart = isHost && this->session->CanStart(reason);
    this->startButton->setVisible(info.isHost);
    this->startButton->setEnabled(canStart);
    this->leaveButton->setText(this->ended ? "Close" : (info.isHost ? "End Session" : "Leave"));

    if (this->ended)
    {
        this->startReasonLabel->setText("The session is over.");
    }
    else if (info.playing)
    {
        QString text = info.paused ? "Paused by " + QString::fromStdString(info.pausedBy) + "." : QString("Playing.");
        if (info.mode == NetplayNew::Mode::Rollback && info.rollback.rollbacks > 0)
        {
            text += QString(" Rolled back %1 times (up to %2 frames).").arg(info.rollback.rollbacks).arg(info.rollback.deepest);
        }
        this->startReasonLabel->setText(text);
    }
    else if (info.isHost)
    {
        this->startReasonLabel->setText(canStart ? QString() : QString::fromStdString(reason));
    }
    else
    {
        this->startReasonLabel->setText(info.connected ? "Waiting for the host to start the game." : "Connecting...");
    }

#ifdef NETPLAY_NEW_TESTS
    // (tests, see SetAutoStart)
    if (isHost && !info.playing && !this->autoStarted && this->autoStartPlayers > 0 &&
        static_cast<int>(info.players.size()) >= this->autoStartPlayers &&
        std::all_of(info.players.begin(), info.players.end(), [](const NetplayNew::PlayerInfo& p) { return p.hasGame; }))
    {
        std::vector<NetplayNew::PlayerInfo> players = info.players;
        std::sort(players.begin(), players.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        for (size_t i = 0; i < players.size(); i++)
        {
            uint8_t ports = i < 4 ? static_cast<uint8_t>(1 << i) : 0;
            if (players[i].ports != ports)
            {
                this->session->SetPorts(players[i].id, ports);
            }
        }
        std::string error;
        uint8_t authority = this->autoStartAuthority >= 0 && this->autoStartAuthority < static_cast<int>(players.size())
                                ? players[this->autoStartAuthority].id
                                : NetplayNew::HostPlayer;
        NetplayNew::SessionInfo now = this->session->GetInfo();
        if (now.mode != static_cast<NetplayNew::Mode>(this->autoStartMode) || now.authorityPlayer != authority)
        {
            this->session->SetMode(static_cast<NetplayNew::Mode>(this->autoStartMode), authority, error);
        }
        if (this->session->CanStart(error))
        {
            this->autoStarted = true;
            this->log("Starting the game for the test");
            this->on_startButton_clicked();
        }
    }
#endif // NETPLAY_NEW_TESTS
}

void NetplaySessionWindow::log(QString message)
{
    this->logList->addItem(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + message);
    this->logList->scrollToBottom();

#ifdef NETPLAY_NEW_TESTS
    // (tests, see SetAutoStart: the log is read from the output)
    if (qEnvironmentVariableIsSet("RMG_NETPLAY_NEW_AUTO"))
    {
        std::printf("[NETPLAY] %s\n", message.toUtf8().constData());
        std::fflush(stdout);
    }
#endif // NETPLAY_NEW_TESTS
}

QString NetplaySessionWindow::findRom(QString md5)
{
    for (auto it = this->roms.cbegin(); it != this->roms.cend(); ++it)
    {
        if (QString::fromStdString(it.value().MD5).compare(md5, Qt::CaseInsensitive) == 0)
        {
            return it.key();
        }
    }
    return QString();
}

void NetplaySessionWindow::leave(void)
{
    this->session->Leave();
    this->ended = true;
    emit this->Left();
}

void NetplaySessionWindow::on_modeComboBox_activated(int index)
{
    auto [mode, player] = this->modeComboBox->itemData(index).value<QPair<int, int>>();
    std::string error;
    if (!this->session->SetMode(static_cast<NetplayNew::Mode>(mode), static_cast<uint8_t>(player), error))
    {
        this->log(QString::fromStdString(error));
    }
    this->refresh();
}

void NetplaySessionWindow::on_pauseButton_clicked(void)
{
    this->session->SetPaused(!this->session->GetInfo().paused);
}

void NetplaySessionWindow::on_startButton_clicked(void)
{
    std::string error;
    std::vector<uint8_t> cheats = serialize_cheats(get_enabled_cheats(this->romFile));
    if (!this->session->Start(cheats, error))
    {
        QtMessageBox::Error(this, "Couldn't start the game", QString::fromStdString(error));
    }
    this->refresh();
}

void NetplaySessionWindow::on_leaveButton_clicked(void)
{
    this->close();
}
