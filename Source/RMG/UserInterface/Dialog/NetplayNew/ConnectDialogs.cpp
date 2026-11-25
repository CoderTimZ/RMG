/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ConnectDialogs.hpp"

#include "Utilities/QtMessageBox.hpp"

#include <RMG-Core/Settings.hpp>
#include <RMG-Netplay-New/Session.hpp>

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

using namespace UserInterface::Dialog;
using namespace Utilities;

static QLineEdit* create_nickname_edit(QWidget* parent)
{
    QLineEdit* edit = new QLineEdit(parent);
    edit->setMaxLength(32);
    edit->setText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_Nickname)));
    return edit;
}

static QDialogButtonBox* create_buttons(QDialog* dialog, QString okText)
{
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(okText);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

//
// HostGameDialog
//

HostGameDialog::HostGameDialog(QWidget* parent, const QMap<QString, CoreRomSettings>& roms)
    : QDialog(parent), roms(roms)
{
    this->setWindowTitle("Host Game");
    this->setMinimumWidth(420);

    this->nicknameEdit = create_nickname_edit(this);

    // games, sorted by name
    this->gameComboBox = new QComboBox(this);
    QList<QPair<QString, QString>> games;
    for (auto it = roms.cbegin(); it != roms.cend(); ++it)
    {
        // games missing from the ROM database have no good name; show their file name instead
        QString name = QString::fromStdString(it.value().GoodName);
        if (name.isEmpty() || name.endsWith("(unknown rom)"))
        {
            name = QFileInfo(it.key()).completeBaseName();
        }
        games.append({name, it.key()});
    }
    std::sort(games.begin(), games.end(), [](const auto& a, const auto& b) {
        return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
    });
    // start with the game hosted last time
    QString lastMD5 = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_NewGameMD5));
    for (const auto& game : games)
    {
        this->gameComboBox->addItem(game.first, game.second);
        if (!lastMD5.isEmpty() && QString::fromStdString(roms.value(game.second).MD5).compare(lastMD5, Qt::CaseInsensitive) == 0)
        {
            this->gameComboBox->setCurrentIndex(this->gameComboBox->count() - 1);
        }
    }

    this->portSpinBox = new QSpinBox(this);
    this->portSpinBox->setRange(1024, 65535);
    this->portSpinBox->setValue(CoreSettingsGetIntValue(SettingsID::Netplay_NewPort));

    this->mapPortCheckBox = new QCheckBox("Ask the router to forward the port (UPnP / NAT-PMP)", this);
    this->mapPortCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Netplay_NewPortMapping));

    QLabel* help = new QLabel("Other players connect to your IP address on this UDP port. Players outside your "
                              "network need the port forwarded to this computer: RMG can ask the router to, or you "
                              "can set it up in the router's settings.", this);
    help->setWordWrap(true);

    QFormLayout* form = new QFormLayout();
    form->addRow("Nickname:", this->nicknameEdit);
    form->addRow("Game:", this->gameComboBox);
    form->addRow("UDP port:", this->portSpinBox);
    form->addRow("", this->mapPortCheckBox);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(help);
    layout->addWidget(create_buttons(this, "Host"));
}

QString HostGameDialog::GetNickname(void) const
{
    return this->nicknameEdit->text().trimmed();
}

QString HostGameDialog::GetRomFile(void) const
{
    return this->gameComboBox->currentData().toString();
}

QString HostGameDialog::GetGameName(void) const
{
    return this->gameComboBox->currentText();
}

QString HostGameDialog::GetGameMD5(void) const
{
    return QString::fromStdString(this->roms.value(this->GetRomFile()).MD5).toUpper();
}

bool HostGameDialog::GetMapPort(void) const
{
    return this->mapPortCheckBox->isChecked();
}

QString HostGameDialog::GetGameInternalName(void) const
{
    return QString::fromStdString(this->roms.value(this->GetRomFile()).InternalName).trimmed();
}

uint16_t HostGameDialog::GetPort(void) const
{
    return static_cast<uint16_t>(this->portSpinBox->value());
}

void HostGameDialog::accept(void)
{
    if (this->GetNickname().isEmpty())
    {
        QtMessageBox::Error(this, "Enter a nickname");
        return;
    }
    if (this->GetRomFile().isEmpty())
    {
        QtMessageBox::Error(this, "Choose a game. Games come from the ROM browser, so add your ROM folder first.");
        return;
    }

    CoreSettingsSetValue(SettingsID::Netplay_Nickname, this->GetNickname().toStdString());
    CoreSettingsSetValue(SettingsID::Netplay_NewPort, static_cast<int>(this->GetPort()));
    CoreSettingsSetValue(SettingsID::Netplay_NewGameMD5, this->GetGameMD5().toStdString());
    CoreSettingsSetValue(SettingsID::Netplay_NewPortMapping, this->GetMapPort());
    CoreSettingsSave();
    QDialog::accept();
}

//
// JoinGameDialog
//

JoinGameDialog::JoinGameDialog(QWidget* parent) : QDialog(parent)
{
    this->setWindowTitle("Join Game");
    this->setMinimumWidth(420);

    this->nicknameEdit = create_nickname_edit(this);

    this->addressEdit = new QLineEdit(this);
    this->addressEdit->setPlaceholderText("address:port, for example 203.0.113.7:6420 or [2001:db8::7]:6420");
    this->addressEdit->setText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_NewAddress)));

    QLabel* help = new QLabel("Ask the host for their address. The port is " +
                              QString::number(NetplayNew::DefaultPort) + " unless they changed it.", this);
    help->setWordWrap(true);

    QFormLayout* form = new QFormLayout();
    form->addRow("Nickname:", this->nicknameEdit);
    form->addRow("Host address:", this->addressEdit);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(help);
    layout->addWidget(create_buttons(this, "Join"));
}

QString JoinGameDialog::GetNickname(void) const
{
    return this->nicknameEdit->text().trimmed();
}

QString JoinGameDialog::GetAddress(void) const
{
    return this->address;
}

uint16_t JoinGameDialog::GetPort(void) const
{
    return this->port;
}

void JoinGameDialog::accept(void)
{
    if (this->GetNickname().isEmpty())
    {
        QtMessageBox::Error(this, "Enter a nickname");
        return;
    }

    // accepted: name, name:port, IPv4, IPv4:port, IPv6, [IPv6] and [IPv6]:port
    QString text = this->addressEdit->text().trimmed();
    QString portText;
    if (text.startsWith('['))
    {
        int bracket = text.indexOf(']');
        if (bracket < 0)
        {
            QtMessageBox::Error(this, "The address is missing its closing ]");
            return;
        }
        this->address = text.mid(1, bracket - 1);
        QString rest  = text.mid(bracket + 1);
        if (!rest.isEmpty())
        {
            if (!rest.startsWith(':'))
            {
                QtMessageBox::Error(this, "Put a colon between ] and the port");
                return;
            }
            portText = rest.mid(1);
        }
    }
    else if (text.count(':') == 1)
    {
        int colon     = text.indexOf(':');
        this->address = text.left(colon);
        portText      = text.mid(colon + 1);
    }
    else
    {
        // a name, an IPv4 address or an IPv6 address without a port
        this->address = text;
    }

    this->port = NetplayNew::DefaultPort;
    if (!portText.isEmpty())
    {
        bool ok   = false;
        int value = portText.toInt(&ok);
        if (!ok || value < 1 || value > 65535)
        {
            QtMessageBox::Error(this, "The port after the colon isn't valid");
            return;
        }
        this->port = static_cast<uint16_t>(value);
    }

    if (this->address.isEmpty())
    {
        QtMessageBox::Error(this, "Enter the host's address");
        return;
    }

    CoreSettingsSetValue(SettingsID::Netplay_Nickname, this->GetNickname().toStdString());
    CoreSettingsSetValue(SettingsID::Netplay_NewAddress, text.toStdString());
    CoreSettingsSave();
    QDialog::accept();
}
