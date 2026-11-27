/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef NETPLAYNEW_NETPLAYSESSIONWINDOW_HPP
#define NETPLAYNEW_NETPLAYSESSIONWINDOW_HPP

#include <RMG-Core/RomSettings.hpp>
#include <RMG-Netplay-New/Session.hpp>

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QString>
#include <QTableWidget>

#include <memory>

namespace UserInterface
{
namespace Dialog
{
// The Netplay Session window: who's in the session, their ports and lag,
// whether everyone is ready, and the host's controls. It owns the session
// and stays open while the game runs.
class NetplaySessionWindow : public QDialog
{
    Q_OBJECT

  public:
    NetplaySessionWindow(QWidget* parent, const QMap<QString, CoreRomSettings>& roms);
    ~NetplaySessionWindow(void);

    // gameInternalName is the name in the ROM header (golf games start in Golf mode)
    bool Host(QString nickname, QString romFile, QString gameName, QString gameMD5, QString gameInternalName,
              uint16_t port, bool mapPort, QString& error);
    bool Join(QString nickname, QString address, uint16_t port, QString& error);

    // whether the session is over (the window only shows why)
    bool HasEnded(void) const;

#ifdef NETPLAY_NEW_TESTS
    // (tests) the host starts the game in this mode once this many players,
    // the host too, have the game, giving each the next port; in Authority
    // mode, the authority is the player who joined authority-th (0: the host)
    void SetAutoStart(int players, int mode, int authority);
#endif // NETPLAY_NEW_TESTS

    // the game stopped on this machine
    void OnGameStopped(void);

  signals:
    // start emulation of this ROM; the session's callbacks and cheats are already set
    void PlayGame(QString romFile);
    // show a message on screen while the game runs
    void ShowMessage(QString message);
    // the user left; the window can be deleted once no game runs
    void Left(void);

  public slots:
    // Escape goes through closeEvent too, so leaving always asks and cleans up
    void reject(void) Q_DECL_OVERRIDE;

  protected:
    void closeEvent(QCloseEvent* event) Q_DECL_OVERRIDE;

  private:
    std::unique_ptr<NetplayNew::Session> session;
    QMap<QString, CoreRomSettings> roms;
    QString romFile;
    QString nickname;
    // the host's addresses to share (the router's part is added as it's known)
    QString addressText;
    bool ended = false;
#ifdef NETPLAY_NEW_TESTS
    int autoStartPlayers = 0;
    int autoStartMode = 0;
    int autoStartAuthority = 0;
    bool autoStarted = false;
#endif // NETPLAY_NEW_TESTS
    bool sentHasGame = false;
    bool updatingTable = false;

    QLabel* gameLabel;
    QLabel* addressLabel;
    QTableWidget* playerTable;
    QLabel* startReasonLabel;
    QComboBox* modeComboBox;
    QPushButton* pauseButton;
    QPushButton* startButton;
    QPushButton* leaveButton;
    QListWidget* logList;

    // this machine's info; the plugins are the ones it would play romFile with
    NetplayNew::LocalInfo localInfo(QString nickname, QString romFile);
    void handleEvent(const NetplayNew::Event& event);
    void refresh(void);
    void log(QString message);
    QString findRom(QString md5);
    void leave(void);

  private slots:
    void on_modeComboBox_activated(int index);
    void on_pauseButton_clicked(void);
    void on_startButton_clicked(void);
    void on_leaveButton_clicked(void);
};
} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAYNEW_NETPLAYSESSIONWINDOW_HPP
