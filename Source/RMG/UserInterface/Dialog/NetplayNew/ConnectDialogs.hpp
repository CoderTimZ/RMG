/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef NETPLAYNEW_CONNECTDIALOGS_HPP
#define NETPLAYNEW_CONNECTDIALOGS_HPP

#include <RMG-Core/RomSettings.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QMap>
#include <QSpinBox>
#include <QString>

namespace UserInterface
{
namespace Dialog
{
// Netplay > Host Game...
class HostGameDialog : public QDialog
{
    Q_OBJECT

  public:
    HostGameDialog(QWidget* parent, const QMap<QString, CoreRomSettings>& roms);

    QString GetNickname(void) const;
    QString GetRomFile(void) const;
    QString GetGameName(void) const;
    QString GetGameMD5(void) const;
    QString GetGameInternalName(void) const;
    uint16_t GetPort(void) const;
    bool GetMapPort(void) const;

  private:
    QMap<QString, CoreRomSettings> roms;
    QCheckBox* mapPortCheckBox;
    QLineEdit* nicknameEdit;
    QComboBox* gameComboBox;
    QSpinBox* portSpinBox;

  private slots:
    void accept(void) Q_DECL_OVERRIDE;
};

// Netplay > Join Game...
class JoinGameDialog : public QDialog
{
    Q_OBJECT

  public:
    JoinGameDialog(QWidget* parent);

    QString GetNickname(void) const;
    QString GetAddress(void) const;
    uint16_t GetPort(void) const;

  private:
    QLineEdit* nicknameEdit;
    QLineEdit* addressEdit;
    QString address;
    uint16_t port = 0;

  private slots:
    void accept(void) Q_DECL_OVERRIDE;
};
} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAYNEW_CONNECTDIALOGS_HPP
