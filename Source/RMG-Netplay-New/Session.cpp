/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "Session.hpp"

#include <RMG-Core/m64p/api/m64p_plugin.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>

using namespace NetplayNew;

//
// Protocol
//

namespace
{
enum MessageType : uint8_t
{
    // guest -> host: u32 protocol, string name, version, rsp, gfx, execution plugin
    MsgHello = 1,
    // host -> guest: u8 your player id
    MsgWelcome,
    // host -> guest: string reason (the host disconnects afterwards)
    MsgReject,
    // host -> all: the session (see BuildStateLocked)
    MsgState,
    // guest -> host: u8 has game, string rsp, gfx, execution plugin (for that game)
    MsgStatus,
    // host -> all: bytes cheats, u8 mode, u8 owner[4], u8 streams, streams * (u8 player, u8 authority)
    MsgStart,
    // host -> all: u8 kind, bytes data
    MsgSync,
    // guest -> host: u8 count, count * u8 plugin (one per owned port, ascending)
    MsgPortInfo,
    // host -> all: 4 * (u8 present, u8 plugin)
    MsgPortTable,
    // any -> host -> all: string reason; the game ended, the session continues
    MsgGameEnded,
    // host -> all: string reason; the session is over
    MsgEnd,
    // input channel: see BuildFramesLocked
    MsgFrames,
    // input channel, owner -> (host ->) authority: u8 stream, u32 sequence, u8 count, count * u32
    MsgInputUpdate,
    // host -> authority: u8 stream, u8 new authority; asks to hand the stream over
    MsgRequestAuthority,
    // authority -> host -> all: u8 stream, u8 new authority, u32 first frame the new authority writes
    MsgDelegate,
    // guest -> host: u8 paused; host -> all: u8 paused, string who
    MsgPause,
    // input channel, guest -> host: u32 first frame, u8 count, count * u64 state hash
    MsgHashes,
    // host -> all: u8 player, u32 frame; the player's game drifted apart from the host's
    MsgDesync,
    // owner -> (host ->) authority: u8 stream, u8 port index, u8 event; for the
    // authority to put into the next frame it writes (an authority that just
    // handed the stream over sends it back to the host, which passes it on)
    MsgEvent,
    // guest -> host: u32 how long the guest's frames take to emulate (the 90th
    // percentile of recent frames) in microseconds, for the automatic lag
    MsgBusy,
};

// frames per stream per message; with redundancy, a message carries every
// frame the receiver hasn't acknowledged yet, up to this many
constexpr uint32_t MaxFramesPerMessage = 64;

// resend unacknowledged frames this often while a game runs
constexpr uint64_t ResendIntervalMs = 15;

// how long to wait for input before telling the user who we're waiting for
constexpr uint64_t WaitingNoticeMs = 300;

// how often the host asks an authority again to hand a stream over
constexpr uint64_t RequestIntervalMs = 500;

// a port mapping lasts 2 hours; renew it well before
constexpr uint64_t PortMappingRenewMs = 50 * 60 * 1000;

// frames further ahead than this are refused (a broken or hostile peer)
constexpr uint32_t MaxFramesAhead = 60 * 60;

// a guest sends its state hashes this many frames at a time, each one twice
constexpr uint32_t HashBatch = 10;

// hashes older than this many frames are forgotten
constexpr uint32_t HashWindow = 60 * 60;

uint64_t now_ms(void)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::vector<int> ports_of(uint8_t mask)
{
    std::vector<int> ports;
    for (int i = 0; i < 4; i++)
    {
        if (mask & (1 << i))
        {
            ports.push_back(i);
        }
    }
    return ports;
}

// the BUTTONS bits: 0-3 D-pad, 4 Start, 5 Z, 6 B, 7 A, 8-11 C buttons, 12 R, 13 L, 16-31 the stick
uint32_t masked(const std::vector<uint32_t>& values, uint32_t mask)
{
    uint32_t any = 0;
    for (uint32_t value : values)
    {
        any |= value & mask;
    }
    return any;
}
} // namespace

bool NetplayNew::IsGolfGame(const std::string& internalName, uint32_t& mask)
{
    // Mario Golf: aiming with the D-pad and looking around with the C buttons
    // isn't taking a shot, so those don't hand the authority over
    if (internalName == "MarioGolf64")
    {
        mask = 0xFFFFF0F0;
        return true;
    }

    mask = 0xFFFFFFFF;
    return false;
}

//
// Session
//

Session::Session(EventHandler handler) : handler(std::move(handler))
{
    std::memset(&this->callbacks, 0, sizeof(this->callbacks));
    this->callbacks.api_version     = M64P_NETPLAY_NEW_API_VERSION;
    this->callbacks.context         = this;
    this->callbacks.sync            = &Session::CoreSync;
    this->callbacks.configure_ports = &Session::CoreConfigurePorts;
    this->callbacks.sample          = &Session::CoreSample;
    this->callbacks.get_input       = &Session::CoreGetInput;
    this->callbacks.frame_end       = &Session::CoreFrameEnd;
    this->callbacks.frame_hash      = &Session::CoreFrameHash;
    std::memset(this->portTable, 0, sizeof(this->portTable));
}

Session::~Session(void)
{
    this->Leave();
}

bool Session::Host(uint16_t port, const LocalInfo& local, const std::string& gameName, const std::string& gameMD5,
                   const std::string& gameInternalName, std::string& error)
{
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->local = local;
        this->info  = SessionInfo();
        this->info.connected = true;
        this->info.isHost    = true;
        this->info.myId      = HostPlayer;
        this->info.gameName  = gameName;
        this->info.gameMD5   = gameMD5;
        this->info.port      = port;

        uint32_t mask = 0xFFFFFFFF;
        if (IsGolfGame(gameInternalName, mask))
        {
            this->info.mode = Mode::Golf;
        }
        this->info.golfMask = mask;

        PlayerInfo host;
        host.id              = HostPlayer;
        host.name            = local.name;
        host.ports           = 0x1;
        host.hasGame         = true;
        host.version         = local.version;
        host.rspPlugin       = local.rspPlugin;
        host.gfxPlugin       = local.gfxPlugin;
        host.executionPlugin = local.executionPlugin;
        this->info.players.push_back(host);

        this->callbacks.is_host = 1;
    }

    Transport::Handlers handlers;
    handlers.connected    = [this](PeerId peer) { this->OnConnected(peer); };
    handlers.disconnected = [this](PeerId peer) { this->OnDisconnected(peer); };
    handlers.received     = [this](PeerId peer, Channel channel, const uint8_t* data, size_t size) {
        this->OnReceived(peer, channel, data, size);
    };
    handlers.tick = [this]() { this->OnTick(); };

    return this->transport.Listen(port, MaxPlayers - 1, std::move(handlers), error);
}

bool Session::Join(const std::string& address, uint16_t port, const LocalInfo& local, std::string& error)
{
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->local = local;
        this->info  = SessionInfo();
        this->info.isHost = false;
        this->callbacks.is_host = 0;
    }

    Transport::Handlers handlers;
    handlers.connected    = [this](PeerId peer) { this->OnConnected(peer); };
    handlers.disconnected = [this](PeerId peer) { this->OnDisconnected(peer); };
    handlers.received     = [this](PeerId peer, Channel channel, const uint8_t* data, size_t size) {
        this->OnReceived(peer, channel, data, size);
    };
    handlers.tick = [this]() { this->OnTick(); };

    return this->transport.Connect(address, port, std::move(handlers), error);
}

void Session::Leave(void)
{
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->info.isHost && !this->ended)
        {
            PacketWriter message;
            message.U8(MsgEnd).String("The host ended the session");
            this->BroadcastLocked(message.Data());
        }
        this->ended = true;
        this->gameRunning = false;
    }
    this->condition.notify_all();

    // closing joins the network thread, so the mutex can't be held here
    this->transport.Close();

    // the router stops forwarding the port
    if (this->portMappingThread.joinable())
    {
        this->portMappingThread.join();
    }
    if (this->portMapper)
    {
        this->portMapper->Unmap();
        this->portMapper.reset();
    }
}

void Session::MapPort(void)
{
    std::lock_guard<std::mutex> lock(this->mutex);
    if (!this->info.isHost || this->ended || this->portMapper)
    {
        return;
    }
    this->portMapper = std::make_unique<PortMapper>();
    this->info.portMapping = SessionInfo::PortMapping::Pending;
    this->StartPortMappingLocked();
}

void Session::StartPortMappingLocked(void)
{
    // the previous run is over (portMappingRunning is false), so this doesn't wait
    if (this->portMappingThread.joinable())
    {
        this->portMappingThread.join();
    }

    this->portMappingRunning = true;
    this->lastPortMapping    = now_ms();
    uint16_t port = this->info.port;

    this->portMappingThread = std::thread([this, port]() {
        PortMapper::Result result = this->portMapper->Map(port);

        std::vector<Event> events;
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            bool wasMapped = this->info.portMapping == SessionInfo::PortMapping::Mapped;
            this->info.portMapping        = result.mapped ? SessionInfo::PortMapping::Mapped : SessionInfo::PortMapping::Failed;
            this->info.portMappingMethod  = result.method;
            this->info.publicAddress      = result.publicAddress;
            this->info.portMappingMessage = result.message;
            if (result.mapped && !wasMapped)
            {
                events.push_back({EventType::Message, "The router forwards UDP port " + std::to_string(port) + " (" + result.method + ")", {}});
            }
            else if (!result.mapped)
            {
                events.push_back({EventType::Message, "Couldn't get the router to forward the port: " + result.message, {}});
            }
            events.push_back({EventType::StateChanged, "", {}});
            this->portMappingRunning = false;
        }
        this->Emit(events);
    });
}

SessionInfo Session::GetInfo(void)
{
    std::lock_guard<std::mutex> lock(this->mutex);
    SessionInfo info = this->info;
    info.inputBy.clear();
    if (this->gameRunning)
    {
        for (auto& [player, stream] : this->streams)
        {
            info.inputBy[player] = stream.authority;
        }
    }
    return info;
}

void Session::SetHasGame(bool hasGame, const LocalInfo& plugins)
{
    std::lock_guard<std::mutex> lock(this->mutex);
    if (this->info.isHost || this->hostPeer == InvalidPeer)
    {
        return;
    }

    PacketWriter message;
    message.U8(MsgStatus)
        .U8(hasGame ? 1 : 0)
        .String(plugins.rspPlugin)
        .String(plugins.gfxPlugin)
        .String(plugins.executionPlugin);
    this->transport.Send(this->hostPeer, Channel::Control, message.Data());
}

void Session::SetPorts(uint8_t player, uint8_t ports)
{
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (!this->info.isHost || this->info.playing)
        {
            return;
        }

        PlayerInfo* target = this->FindPlayerLocked(player);
        if (target == nullptr)
        {
            return;
        }

        ports &= 0x0F;
        for (PlayerInfo& other : this->info.players)
        {
            if (other.id != player)
            {
                other.ports &= ~ports;
            }
        }
        target->ports = ports;

        this->BroadcastLocked(this->BuildStateLocked());
    }
    this->Emit(EventType::StateChanged);
}

void Session::SetLag(uint8_t player, uint8_t lag)
{
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (!this->info.isHost)
        {
            return;
        }

        PlayerInfo* target = this->FindPlayerLocked(player);
        if (target == nullptr)
        {
            return;
        }

        target->lag = std::min<uint8_t>(lag, 60);
        this->lagStates[player].fixed = true;
        this->BroadcastLocked(this->BuildStateLocked());
    }
    this->Emit(EventType::StateChanged);
}

void Session::AdjustLagLocked(uint64_t now)
{
    // The host sets every player's lag (their input delay in Lockstep) from
    // what it measures, about once a second, so nobody has to:
    //
    // - A player's frames have to reach every other player with controllers
    //   before those players need them. Guests reach each other through the
    //   host, so a path's delay is half the round trip on each leg it takes.
    // - Each leg's round trip variation (ENet's mean deviation) is added, so
    //   a jittery connection gets the frames it needs before they're late.
    // - A receiver doesn't need a frame's input the moment the frame starts
    //   being late: it emulates a frame in less than a frame's time and the
    //   speed limiter sleeps the rest, so a short wait only takes the place
    //   of some of that sleep. Half of that spare time (going by its slower
    //   frames) is counted as delay it can absorb, so on a LAN the lag is 0.
    //   Until a receiver has played a while, a quarter of a frame is assumed.
    // - Frames last as long as the game's VIs at full speed: 1/60 s, or 1/50 s
    //   for PAL. (Not the measured rate: a game slowed down by waiting would
    //   look like it had more spare time, and lower its own lag.)
    //
    // While no game runs the lag goes straight to what's needed. During a
    // game it moves one frame at a time: up at once when the network gets
    // slower, down only after a few checks in a row say it can, so it
    // doesn't flap. Each step costs a repeated or skipped input frame.

    // one-way delay in milliseconds between two players, with a margin for jitter
    auto delay = [this](const PlayerInfo& a, const PlayerInfo& b) -> double {
        auto leg = [this](const PlayerInfo& guest) {
            auto it = this->lagStates.find(guest.id);
            double jitter = it != this->lagStates.end() ? it->second.jitter : 0.0;
            return guest.ping / 2.0 + jitter;
        };
        double ms = 0.0;
        if (a.id != HostPlayer)
        {
            ms += leg(a);
        }
        if (b.id != HostPlayer)
        {
            ms += leg(b);
        }
        return ms;
    };

    double frameMs = this->frameUs / 1000.0;

    // the delay a receiver absorbs in its spare time each frame
    auto slack = [this, frameMs](const PlayerInfo& receiver) -> double {
        auto it = this->lagStates.find(receiver.id);
        if (it == this->lagStates.end() || it->second.busyUs < 0)
        {
            return frameMs / 4.0;
        }
        return std::max(0.0, frameMs - it->second.busyUs / 1000.0) / 2.0;
    };

    this->lagStates[HostPlayer].busyUs = this->BusyLocked();

    for (PlayerInfo& player : this->info.players)
    {
        LagState& state = this->lagStates[player.id];
        if (player.ports == 0 || state.fixed)
        {
            state.lowerChecks = 0;
            continue;
        }

        // the receiver that needs the most time for this player's frames
        double frames = 0.0;
        for (const PlayerInfo& other : this->info.players)
        {
            if (other.id != player.id && other.ports != 0)
            {
                frames = std::max(frames, (delay(player, other) - slack(other)) / frameMs);
            }
        }
        uint8_t needed = static_cast<uint8_t>(std::min(60.0, std::ceil(frames)));

        uint8_t lag = player.lag;
        if (!this->gameRunning)
        {
            lag = needed;
            state.lowerChecks = 0;
        }
        else if (needed > lag)
        {
            lag++;
            state.lowerChecks = 0;
        }
        else if (needed < lag)
        {
            if (++state.lowerChecks >= 3)
            {
                lag--;
                state.lowerChecks = 0;
            }
        }
        else
        {
            state.lowerChecks = 0;
        }

        // (shared with everyone in the next state broadcast)
        player.lag = lag;
    }

    // players who left
    for (auto it = this->lagStates.begin(); it != this->lagStates.end();)
    {
        it = this->info.Find(it->first) == nullptr ? this->lagStates.erase(it) : std::next(it);
    }
}

void Session::SetMode(Mode mode, uint8_t authorityPlayer)
{
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (!this->info.isHost)
        {
            return;
        }

        if (this->FindPlayerLocked(authorityPlayer) == nullptr)
        {
            authorityPlayer = HostPlayer;
        }

        bool golfStarts = mode == Mode::Golf && this->info.mode != Mode::Golf;
        this->info.mode            = mode;
        this->info.authorityPlayer = authorityPlayer;

        if (this->gameRunning)
        {
            if (golfStarts)
            {
                // golf starts with the host holding every stream; from then on,
                // whoever plays takes it
                for (auto& [player, stream] : this->streams)
                {
                    if (stream.authority == HostPlayer)
                    {
                        continue;
                    }
                    PacketWriter request;
                    request.U8(MsgRequestAuthority).U8(player).U8(HostPlayer);
                    this->SendToPlayerLocked(stream.authority, Channel::Control, request.Data());
                }
            }
            this->EnforceModeLocked(true);
        }

        this->BroadcastLocked(this->BuildStateLocked());
    }
    this->condition.notify_all();
    this->Emit(EventType::StateChanged);
}

bool Session::CanStart(std::string& reason)
{
    std::lock_guard<std::mutex> lock(this->mutex);

    if (!this->info.isHost)
    {
        reason = "Only the host can start the game";
        return false;
    }
    if (this->info.playing)
    {
        reason = "The game is already running";
        return false;
    }

    const PlayerInfo* host = this->info.Find(HostPlayer);
    bool anyPorts = false;
    for (const PlayerInfo& player : this->info.players)
    {
        anyPorts = anyPorts || player.ports != 0;
        if (!player.hasGame)
        {
            reason = player.name + " doesn't have the game";
            return false;
        }
        if (player.version != host->version)
        {
            reason = player.name + " runs a different version of RMG (" + player.version + ")";
            return false;
        }
        // execution plugins change the game, so everyone needs the same one
        if (player.executionPlugin != host->executionPlugin)
        {
            reason = player.name + " uses a different execution plugin (" +
                     (player.executionPlugin.empty() ? std::string("none") : player.executionPlugin) + ")";
            return false;
        }
    }

    if (!anyPorts)
    {
        reason = "No one has a controller port";
        return false;
    }

    return true;
}

bool Session::Start(const std::vector<uint8_t>& cheats, std::string& error)
{
    if (!this->CanStart(error))
    {
        return false;
    }

    Event event{EventType::GameStarting, "", cheats};
    {
        std::lock_guard<std::mutex> lock(this->mutex);

        uint8_t owners[4] = {NoPlayer, NoPlayer, NoPlayer, NoPlayer};
        for (const PlayerInfo& player : this->info.players)
        {
            for (int port : ports_of(player.ports))
            {
                owners[port] = player.id;
            }
        }

        // who writes each stream at first
        std::map<uint8_t, uint8_t> authorities;
        for (int i = 0; i < 4; i++)
        {
            if (owners[i] == NoPlayer)
            {
                continue;
            }
            switch (this->info.mode)
            {
            case Mode::Lockstep:
                authorities[owners[i]] = owners[i];
                break;
            case Mode::Authority:
                authorities[owners[i]] = this->info.authorityPlayer;
                break;
            case Mode::Golf:
                authorities[owners[i]] = HostPlayer;
                break;
            }
        }

        this->info.paused = false;
        this->StartGameLocked(this->info.mode, owners, authorities);

        PacketWriter message;
        message.U8(MsgStart).Bytes(cheats).U8(static_cast<uint8_t>(this->info.mode));
        for (int i = 0; i < 4; i++)
        {
            message.U8(owners[i]);
        }
        message.U8(static_cast<uint8_t>(authorities.size()));
        for (auto& [player, authority] : authorities)
        {
            message.U8(player).U8(authority);
        }
        this->BroadcastLocked(message.Data());
        this->BroadcastLocked(this->BuildStateLocked());
    }

    this->Emit({event});
    return true;
}

void Session::SetPaused(bool paused)
{
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->ended || !this->info.playing || this->info.paused == paused)
        {
            return;
        }

        if (this->info.isHost)
        {
            std::string by = this->PlayerNameLocked(HostPlayer);
            PacketWriter message;
            message.U8(MsgPause).U8(paused ? 1 : 0).String(by);
            this->BroadcastLocked(message.Data());
            this->ApplyPauseLocked(paused, by, events);
        }
        else if (this->hostPeer != InvalidPeer)
        {
            PacketWriter message;
            message.U8(MsgPause).U8(paused ? 1 : 0);
            this->transport.Send(this->hostPeer, Channel::Control, message.Data());
        }
    }
    this->condition.notify_all();
    this->Emit(events);
}

const m64p_netplay_new_callbacks* Session::GetCallbacks(void)
{
    return &this->callbacks;
}

void Session::GameStopped(void)
{
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (!this->info.playing)
        {
            return;
        }

        const PlayerInfo* me = this->info.Find(this->info.myId);
        bool hasPorts = me != nullptr && me->ports != 0 && this->streams.count(this->info.myId) != 0;

        // a spectator closing their game only affects them
        if (hasPorts || this->info.isHost)
        {
            PacketWriter message;
            message.U8(MsgGameEnded).String(this->PlayerNameLocked(this->info.myId) + " stopped the game");
            if (this->info.isHost)
            {
                this->BroadcastLocked(message.Data());
            }
            else if (this->hostPeer != InvalidPeer)
            {
                this->transport.Send(this->hostPeer, Channel::Control, message.Data());
            }
        }

        this->EndGameLocked();
        if (this->info.isHost)
        {
            this->BroadcastLocked(this->BuildStateLocked());
        }
    }
    this->condition.notify_all();
    this->Emit(EventType::StateChanged);
}

//
// Network handlers
//

void Session::OnConnected(PeerId peer)
{
    std::lock_guard<std::mutex> lock(this->mutex);

    if (this->info.isHost)
    {
        // wait for the guest to say hello
        return;
    }

    this->hostPeer = peer;

    PacketWriter message;
    message.U8(MsgHello)
        .U32(ProtocolVersion)
        .String(this->local.name)
        .String(this->local.version)
        .String(this->local.rspPlugin)
        .String(this->local.gfxPlugin)
        .String(this->local.executionPlugin);
    this->transport.Send(peer, Channel::Control, message.Data());
}

void Session::OnDisconnected(PeerId peer)
{
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->ended)
        {
            return;
        }

        if (!this->info.isHost)
        {
            // lost the host (or never reached it)
            this->ended = true;
            this->EndGameLocked();
            events.push_back({EventType::Ended,
                              this->info.connected ? "Lost the connection to the host" : "Couldn't connect to the host", {}});
        }
        else
        {
            auto it = this->peerPlayers.find(peer);
            if (it == this->peerPlayers.end())
            {
                return;
            }

            uint8_t player = it->second;
            std::string name = this->PlayerNameLocked(player);
            this->peerPlayers.erase(it);
            this->playerPeers.erase(player);
            this->acks.erase(player);
            this->lastFramesSent.erase(peer);

            if (this->info.authorityPlayer == player)
            {
                this->info.authorityPlayer = HostPlayer;
            }

            bool hadStream = this->gameRunning && this->streams.count(player) != 0;
            if (this->gameRunning)
            {
                // the host takes over whatever the player was writing
                for (auto& [streamPlayer, stream] : this->streams)
                {
                    if (stream.player == player)
                    {
                        // their controller stays plugged in, idle
                        std::fill(stream.latest.begin(), stream.latest.end(), 0);
                    }
                    this->TakeOverLocked(stream, player);
                }
            }

            this->info.players.erase(std::remove_if(this->info.players.begin(), this->info.players.end(),
                                                    [player](const PlayerInfo& p) { return p.id == player; }),
                                     this->info.players.end());
            this->BroadcastLocked(this->BuildStateLocked());
            if (this->gameRunning)
            {
                this->SendFramesLocked();
            }

            events.push_back({EventType::Message, hadStream ? name + " left; their controller is idle now" : name + " left", {}});
            events.push_back({EventType::StateChanged, "", {}});
        }
    }
    this->condition.notify_all();
    this->Emit(events);
}

void Session::OnReceived(PeerId peer, Channel channel, const uint8_t* data, size_t size)
{
    std::vector<Event> events;

    try
    {
        PacketReader reader(data, size);
        uint8_t type = reader.U8();

        if (type == MsgFrames)
        {
            this->HandleFrames(peer, reader);
            return;
        }
        if (type == MsgInputUpdate)
        {
            this->HandleInputUpdate(peer, reader);
            return;
        }
        if (type == MsgHashes)
        {
            this->HandleHashes(peer, reader);
            return;
        }

        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->ended)
        {
            return;
        }

        if (this->info.isHost)
        {
            this->HandleHostMessage(peer, type, reader, events);
        }
        else if (peer == this->hostPeer)
        {
            this->HandleGuestMessage(type, reader, events);
        }
    }
    catch (const std::exception&)
    {
        // a malformed message; ignore it
    }

    this->condition.notify_all();
    this->Emit(events);
}

void Session::HandleHostMessage(PeerId peer, uint8_t type, PacketReader& reader, std::vector<Event>& events)
{
    auto known = this->peerPlayers.find(peer);

    if (type == MsgHello)
    {
        if (known != this->peerPlayers.end())
        {
            return;
        }

        uint32_t protocol = reader.U32();

        auto reject = [&](const std::string& reason) {
            PacketWriter message;
            message.U8(MsgReject).String(reason);
            this->transport.Send(peer, Channel::Control, message.Data());
            this->transport.Disconnect(peer);
        };

        // check the version before reading anything else, since the rest can differ
        if (protocol != ProtocolVersion)
        {
            reject("The host runs a different version of netplay (protocol " + std::to_string(ProtocolVersion) +
                   ", yours is " + std::to_string(protocol) + ")");
            return;
        }

        PlayerInfo player;
        player.name            = reader.String();
        player.version         = reader.String();
        player.rspPlugin       = reader.String();
        player.gfxPlugin       = reader.String();
        player.executionPlugin = reader.String();

        if (this->info.playing)
        {
            reject("The game has already started");
            return;
        }
        if (static_cast<int>(this->info.players.size()) >= MaxPlayers || this->nextPlayerId == NoPlayer)
        {
            reject("The session is full");
            return;
        }

        player.id = this->nextPlayerId++;
        if (player.name.empty())
        {
            player.name = "Player " + std::to_string(player.id + 1);
        }

        // the first free port, or spectate
        uint8_t used = 0;
        for (const PlayerInfo& other : this->info.players)
        {
            used |= other.ports;
        }
        for (int i = 0; i < 4; i++)
        {
            if (!(used & (1 << i)))
            {
                player.ports = static_cast<uint8_t>(1 << i);
                break;
            }
        }

        // start with the lag the host has
        const PlayerInfo* host = this->info.Find(HostPlayer);
        player.lag = host != nullptr ? host->lag : 0;

        this->info.players.push_back(player);
        this->peerPlayers[peer]      = player.id;
        this->playerPeers[player.id] = peer;

        PacketWriter welcome;
        welcome.U8(MsgWelcome).U8(player.id);
        this->transport.Send(peer, Channel::Control, welcome.Data());

        this->BroadcastLocked(this->BuildStateLocked());
        events.push_back({EventType::Message, player.name + " joined", {}});
        events.push_back({EventType::StateChanged, "", {}});
        return;
    }

    if (known == this->peerPlayers.end())
    {
        return;
    }
    uint8_t playerId = known->second;
    PlayerInfo* player = this->FindPlayerLocked(playerId);
    if (player == nullptr)
    {
        return;
    }

    switch (type)
    {
    case MsgStatus:
        player->hasGame         = reader.U8() != 0;
        player->rspPlugin       = reader.String();
        player->gfxPlugin       = reader.String();
        player->executionPlugin = reader.String();
        this->BroadcastLocked(this->BuildStateLocked());
        events.push_back({EventType::StateChanged, "", {}});
        break;

    case MsgPortInfo:
    {
        uint8_t count = reader.U8();
        std::vector<uint8_t> plugins;
        for (uint8_t i = 0; i < count; i++)
        {
            plugins.push_back(reader.U8());
        }
        this->portPlugins[playerId] = plugins;
        break;
    }

    case MsgGameEnded:
    {
        std::string reason = reader.String();
        if (this->info.playing)
        {
            PacketWriter message;
            message.U8(MsgGameEnded).String(reason);
            this->BroadcastLocked(message.Data());
            this->EndGameLocked();
            this->BroadcastLocked(this->BuildStateLocked());
            events.push_back({EventType::GameEnded, reason, {}});
            events.push_back({EventType::StateChanged, "", {}});
        }
        break;
    }

    case MsgDelegate:
    {
        uint8_t streamPlayer = reader.U8();
        uint8_t to           = reader.U8();
        uint32_t from        = reader.U32();
        auto it = this->streams.find(streamPlayer);
        // only the current authority can hand a stream over; anything else is stale
        if (!this->gameRunning || it == this->streams.end() || it->second.authority != playerId)
        {
            break;
        }
        if (to != HostPlayer && this->playerPeers.count(to) == 0)
        {
            to = HostPlayer; // handed to someone who just left
        }
        PacketWriter message;
        message.U8(MsgDelegate).U8(streamPlayer).U8(to).U32(from);
        this->BroadcastLocked(message.Data(), playerId);
        this->ApplyDelegateLocked(it->second, to, from);
        // the sender learns about a replacement authority too
        if (to == HostPlayer)
        {
            this->SendToPlayerLocked(playerId, Channel::Control, message.Data());
        }
        events.push_back({EventType::StateChanged, "", {}});
        break;
    }

    case MsgEvent:
    {
        uint8_t streamPlayer = reader.U8();
        uint8_t port         = reader.U8();
        uint8_t event        = reader.U8();
        auto it = this->streams.find(streamPlayer);
        if (this->gameRunning && it != this->streams.end() && port < it->second.ports.size() && event != 0)
        {
            this->RouteEventLocked(it->second, port, event);
        }
        break;
    }

    case MsgBusy:
    {
        uint32_t busyUs = reader.U32();
        this->lagStates[playerId].busyUs = std::min<uint32_t>(busyUs, 1000000);
        break;
    }

    case MsgPause:
    {
        bool paused = reader.U8() != 0;
        if (this->info.playing && this->info.paused != paused)
        {
            PacketWriter message;
            message.U8(MsgPause).U8(paused ? 1 : 0).String(player->name);
            this->BroadcastLocked(message.Data());
            this->ApplyPauseLocked(paused, player->name, events);
        }
        break;
    }

    default:
        break;
    }
}

void Session::HandleGuestMessage(uint8_t type, PacketReader& reader, std::vector<Event>& events)
{
    switch (type)
    {
    case MsgWelcome:
        this->info.myId      = reader.U8();
        this->info.connected = true;
        events.push_back({EventType::Connected, "", {}});
        break;

    case MsgReject:
        this->ended = true;
        events.push_back({EventType::Ended, reader.String(), {}});
        break;

    case MsgState:
    {
        SessionInfo state;
        state.connected       = this->info.connected;
        state.isHost          = false;
        state.myId            = this->info.myId;
        state.gameName        = reader.String();
        state.gameMD5         = reader.String();
        state.playing         = reader.U8() != 0;
        state.mode            = static_cast<Mode>(reader.U8());
        state.authorityPlayer = reader.U8();
        state.golfMask        = reader.U32();
        uint8_t count         = reader.U8();
        for (uint8_t i = 0; i < count; i++)
        {
            PlayerInfo player;
            player.id              = reader.U8();
            player.name            = reader.String();
            player.ports           = reader.U8();
            player.lag             = reader.U8();
            player.ping            = reader.U16();
            player.hasGame         = reader.U8() != 0;
            player.version         = reader.String();
            player.rspPlugin       = reader.String();
            player.gfxPlugin       = reader.String();
            player.executionPlugin = reader.String();
            state.players.push_back(player);
        }

        // whether a game runs here, and whether it's paused, is decided by
        // the Start, GameEnded and Pause messages
        state.playing  = this->info.playing;
        state.paused   = this->info.paused;
        state.pausedBy = this->info.pausedBy;
        state.desyncs  = this->info.desyncs;

        // a player who left keeps their controller plugged in, idle
        if (this->gameRunning)
        {
            for (auto& [player, stream] : this->streams)
            {
                if (state.Find(player) == nullptr)
                {
                    std::fill(stream.latest.begin(), stream.latest.end(), 0);
                }
            }
        }

        this->info = state;
        events.push_back({EventType::StateChanged, "", {}});
        break;
    }

    case MsgStart:
    {
        std::vector<uint8_t> cheats = reader.Bytes();
        Mode mode = static_cast<Mode>(reader.U8());
        uint8_t owners[4];
        for (int i = 0; i < 4; i++)
        {
            owners[i] = reader.U8();
        }
        std::map<uint8_t, uint8_t> authorities;
        uint8_t count = reader.U8();
        for (uint8_t i = 0; i < count; i++)
        {
            uint8_t player = reader.U8();
            authorities[player] = reader.U8();
        }
        this->info.paused = false;
        this->StartGameLocked(mode, owners, authorities);
        events.push_back({EventType::GameStarting, "", cheats});
        break;
    }

    case MsgSync:
    {
        int kind = reader.U8();
        this->syncData[kind] = reader.Bytes();
        break;
    }

    case MsgPortTable:
        for (int i = 0; i < 4; i++)
        {
            this->portTable[i].present = reader.U8();
            this->portTable[i].plugin  = reader.U8();
        }
        this->hasPortTable = true;
        break;

    case MsgGameEnded:
    {
        std::string reason = reader.String();
        if (this->info.playing)
        {
            this->EndGameLocked();
            events.push_back({EventType::GameEnded, reason, {}});
        }
        break;
    }

    case MsgEnd:
        this->ended = true;
        this->EndGameLocked();
        events.push_back({EventType::Ended, reader.String(), {}});
        break;

    case MsgRequestAuthority:
    {
        uint8_t streamPlayer = reader.U8();
        uint8_t to           = reader.U8();
        auto it = this->streams.find(streamPlayer);
        if (this->gameRunning && it != this->streams.end() && it->second.authority == this->info.myId && to != this->info.myId)
        {
            this->DelegateLocked(it->second, to);
            events.push_back({EventType::StateChanged, "", {}});
        }
        break;
    }

    case MsgDelegate:
    {
        uint8_t streamPlayer = reader.U8();
        uint8_t to           = reader.U8();
        uint32_t from        = reader.U32();
        auto it = this->streams.find(streamPlayer);
        if (this->gameRunning && it != this->streams.end())
        {
            this->ApplyDelegateLocked(it->second, to, from);
            events.push_back({EventType::StateChanged, "", {}});
        }
        break;
    }

    case MsgPause:
    {
        bool paused = reader.U8() != 0;
        std::string by = reader.String();
        if (this->info.playing)
        {
            this->ApplyPauseLocked(paused, by, events);
        }
        break;
    }

    case MsgEvent:
    {
        uint8_t streamPlayer = reader.U8();
        uint8_t port         = reader.U8();
        uint8_t event        = reader.U8();
        auto it = this->streams.find(streamPlayer);
        if (this->gameRunning && it != this->streams.end() && port < it->second.ports.size() && event != 0)
        {
            this->RouteEventLocked(it->second, port, event);
        }
        break;
    }

    case MsgDesync:
    {
        uint8_t player = reader.U8();
        uint32_t frame = reader.U32();
        if (this->info.playing && this->info.desyncs.count(player) == 0)
        {
            this->info.desyncs[player] = frame;
            std::string who = player == this->info.myId ? std::string("Your game") : this->PlayerNameLocked(player) + "'s game";
            events.push_back({EventType::Desync, who + " drifted apart from the host's at frame " + std::to_string(frame), {}});
            events.push_back({EventType::StateChanged, "", {}});
        }
        break;
    }

    default:
        break;
    }
}

void Session::HandleFrames(PeerId peer, PacketReader& reader)
{
    bool newFrames = false;
    uint8_t sender = NoPlayer;

    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->ended || !this->gameRunning)
        {
            return;
        }

        if (this->info.isHost)
        {
            auto it = this->peerPlayers.find(peer);
            if (it == this->peerPlayers.end())
            {
                return;
            }
            sender = it->second;
        }
        else
        {
            if (peer != this->hostPeer)
            {
                return;
            }
            sender = HostPlayer;
        }

        uint8_t streamCount = reader.U8();
        for (uint8_t i = 0; i < streamCount; i++)
        {
            uint8_t player = reader.U8();
            uint32_t first = reader.U32();
            uint16_t count = reader.U16();

            auto it = this->streams.find(player);
            if (it == this->streams.end())
            {
                return; // can't know the frame size; drop the message
            }
            Stream& stream = it->second;
            size_t width = stream.ports.size();

            std::vector<uint32_t> values(width);
            std::vector<uint8_t> events(width);
            for (uint32_t f = first; f < first + count; f++)
            {
                for (size_t k = 0; k < width; k++)
                {
                    values[k] = reader.U32();
                    events[k] = reader.U8();
                }

                // Frames never change once written, so a frame is taken from
                // whoever has it. Only frames this machine writes itself, and
                // frames absurdly far ahead, are refused.
                bool mine = stream.authority == this->info.myId && f >= stream.nextWrite;
                if (mine || f >= stream.count + MaxFramesAhead)
                {
                    continue;
                }
                if (f < stream.known.size() && stream.known[f])
                {
                    continue;
                }

                uint32_t before = stream.count;
                this->SetFrameLocked(stream, f, values, events);
                newFrames = newFrames || stream.count != before;
            }
        }

        uint8_t ackCount = reader.U8();
        for (uint8_t i = 0; i < ackCount; i++)
        {
            uint8_t player = reader.U8();
            uint32_t have  = reader.U32();
            uint32_t& ack  = this->acks[sender][player];
            ack = std::max(ack, have);
        }

        // the host passes new frames on right away
        if (newFrames && this->info.isHost)
        {
            for (auto& [player, guestPeer] : this->playerPeers)
            {
                if (player != sender)
                {
                    this->SendFramesTo(guestPeer, player);
                }
            }
        }
    }

    if (newFrames)
    {
        this->condition.notify_all();
    }
}

void Session::HandleInputUpdate(PeerId peer, PacketReader& reader)
{
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->ended || !this->gameRunning)
        {
            return;
        }

        uint8_t streamPlayer = reader.U8();
        uint32_t sequence    = reader.U32();
        uint8_t count        = reader.U8();
        std::vector<uint32_t> values;
        for (uint8_t i = 0; i < count; i++)
        {
            values.push_back(reader.U32());
        }

        auto it = this->streams.find(streamPlayer);
        if (it == this->streams.end() || values.size() != it->second.ports.size())
        {
            return;
        }
        Stream& stream = it->second;

        if (this->info.isHost)
        {
            auto sender = this->peerPlayers.find(peer);
            if (sender == this->peerPlayers.end() || sender->second != streamPlayer)
            {
                return;
            }

            // pass it on to the stream's authority
            if (stream.authority != HostPlayer)
            {
                if (stream.authority != streamPlayer)
                {
                    PacketWriter message;
                    message.U8(MsgInputUpdate).U8(streamPlayer).U32(sequence).U8(count);
                    for (uint32_t value : values)
                    {
                        message.U32(value);
                    }
                    this->SendToPlayerLocked(stream.authority, Channel::Input, message.Data());
                }
                return;
            }
        }

        if (stream.authority != this->info.myId)
        {
            return;
        }

        // updates may arrive out of order; keep the newest
        if (sequence <= stream.latestSequence && stream.latestSequence != 0)
        {
            return;
        }
        stream.latestSequence = sequence;
        stream.latest = values;

        // Golf: the owner started playing, so they take over every stream this
        // machine writes. Their input so far is in their stream already, so
        // nothing they pressed gets lost, only delayed until the handover.
        if (this->info.mode == Mode::Golf && masked(values, this->info.golfMask) != 0)
        {
            for (auto& [player, other] : this->streams)
            {
                if (other.authority == this->info.myId)
                {
                    this->DelegateLocked(other, streamPlayer);
                }
            }
            events.push_back({EventType::StateChanged, "", {}});
        }
    }
    this->condition.notify_all();
    this->Emit(events);
}

void Session::HandleHashes(PeerId peer, PacketReader& reader)
{
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->ended || !this->gameRunning || !this->info.isHost)
        {
            return;
        }

        auto sender = this->peerPlayers.find(peer);
        if (sender == this->peerPlayers.end())
        {
            return;
        }

        uint32_t first = reader.U32();
        uint8_t count  = reader.U8();
        for (uint8_t i = 0; i < count; i++)
        {
            this->CompareHashLocked(sender->second, first + i, static_cast<uint64_t>(reader.I64()), events);
        }
    }
    this->Emit(events);
}

void Session::CompareHashLocked(uint8_t player, uint32_t frame, uint64_t hash, std::vector<Event>& events)
{
    // the host compares everyone's game with its own, once per player
    if (this->info.desyncs.count(player) != 0)
    {
        return;
    }

    auto mine = this->myHashes.find(frame);
    if (mine == this->myHashes.end())
    {
        // the host hasn't run this frame yet
        // (within reach: hashes for frames far ahead would never be pruned)
        uint32_t latest = this->myHashes.empty() ? 0 : this->myHashes.rbegin()->first;
        if ((this->myHashes.empty() || frame > latest) && frame <= latest + MaxFramesAhead)
        {
            this->otherHashes[player][frame] = hash;
        }
        return;
    }

    if (mine->second == hash)
    {
        return;
    }

    this->info.desyncs[player] = frame;
    PacketWriter message;
    message.U8(MsgDesync).U8(player).U32(frame);
    this->BroadcastLocked(message.Data());

    events.push_back({EventType::Desync, this->PlayerNameLocked(player) + "'s game drifted apart from yours at frame " +
                                             std::to_string(frame), {}});
    events.push_back({EventType::StateChanged, "", {}});
}

void Session::OnTick(void)
{
    std::lock_guard<std::mutex> lock(this->mutex);
    if (this->ended)
    {
        return;
    }

    uint64_t now = now_ms();

    if (this->info.isHost)
    {
        // pings change all the time; share them once a second
        if (now - this->lastStateBroadcast >= 1000)
        {
            for (PlayerInfo& player : this->info.players)
            {
                auto it = this->playerPeers.find(player.id);
                if (it != this->playerPeers.end())
                {
                    player.ping = static_cast<uint16_t>(std::min<uint32_t>(this->transport.GetRoundTripTime(it->second), 0xFFFF));
                    this->lagStates[player.id].jitter = this->transport.GetRoundTripTimeVariance(it->second);
                }
            }
            this->AdjustLagLocked(now);
            this->BroadcastLocked(this->BuildStateLocked());
            this->lastStateBroadcast = now;
        }

        if (this->gameRunning)
        {
            this->EnforceModeLocked(false);
        }

        // keep the router forwarding the port
        if (this->portMapper && !this->portMappingRunning && this->info.portMapping == SessionInfo::PortMapping::Mapped &&
            now - this->lastPortMapping >= PortMappingRenewMs)
        {
            this->StartPortMappingLocked();
        }
    }

    // resend what may have been lost, and acknowledge what arrived
    if (this->gameRunning)
    {
        if (this->info.isHost)
        {
            for (auto& [player, peer] : this->playerPeers)
            {
                if (now - this->lastFramesSent[peer] >= ResendIntervalMs)
                {
                    this->SendFramesTo(peer, player);
                }
            }
        }
        else if (this->hostPeer != InvalidPeer && now - this->lastFramesSent[this->hostPeer] >= ResendIntervalMs)
        {
            this->SendFramesTo(this->hostPeer, HostPlayer);
        }

        // how long this machine's frames take, for the host's automatic lag
        if (!this->info.isHost && this->hostPeer != InvalidPeer && now - this->lastBusySent >= 1000)
        {
            int64_t busy = this->BusyLocked();
            if (busy >= 0)
            {
                PacketWriter message;
                message.U8(MsgBusy).U32(static_cast<uint32_t>(busy));
                this->transport.Send(this->hostPeer, Channel::Control, message.Data());
            }
            this->lastBusySent = now;
        }
    }
}

//
// Helpers
//

void Session::Emit(const std::vector<Event>& events)
{
    if (!this->handler)
    {
        return;
    }
    for (const Event& event : events)
    {
        this->handler(event);
    }
}

void Session::Emit(EventType type, const std::string& text)
{
    this->Emit(std::vector<Event>{{type, text, {}}});
}

std::vector<uint8_t> Session::BuildStateLocked(void)
{
    PacketWriter message;
    message.U8(MsgState)
        .String(this->info.gameName)
        .String(this->info.gameMD5)
        .U8(this->info.playing ? 1 : 0)
        .U8(static_cast<uint8_t>(this->info.mode))
        .U8(this->info.authorityPlayer)
        .U32(this->info.golfMask)
        .U8(static_cast<uint8_t>(this->info.players.size()));
    for (const PlayerInfo& player : this->info.players)
    {
        message.U8(player.id)
            .String(player.name)
            .U8(player.ports)
            .U8(player.lag)
            .U16(player.ping)
            .U8(player.hasGame ? 1 : 0)
            .String(player.version)
            .String(player.rspPlugin)
            .String(player.gfxPlugin)
            .String(player.executionPlugin);
    }
    return message.Data();
}

void Session::BroadcastLocked(const std::vector<uint8_t>& message, uint8_t exceptPlayer)
{
    if (!this->info.isHost)
    {
        return;
    }
    for (auto& [player, peer] : this->playerPeers)
    {
        if (player != exceptPlayer)
        {
            this->transport.Send(peer, Channel::Control, message);
        }
    }
}

void Session::SendToPlayerLocked(uint8_t player, Channel channel, const std::vector<uint8_t>& message)
{
    if (this->info.isHost)
    {
        auto it = this->playerPeers.find(player);
        if (it != this->playerPeers.end())
        {
            this->transport.Send(it->second, channel, message);
        }
    }
    else if (this->hostPeer != InvalidPeer)
    {
        // guests only talk to the host
        this->transport.Send(this->hostPeer, channel, message);
    }
}

void Session::StartGameLocked(Mode mode, const uint8_t owners[4], const std::map<uint8_t, uint8_t>& authorities)
{
    this->gameRunning    = true;
    this->gameNumber++;
    this->busySamples.clear();
    this->busyNext = 0;
    this->info.playing   = true;
    this->info.mode      = mode;
    this->streams.clear();
    this->acks.clear();
    this->syncData.clear();
    this->portPlugins.clear();
    this->hasPortTable   = false;
    this->lastSampled    = -1;
    this->updateSequence = 0;
    this->myHashes.clear();
    this->otherHashes.clear();
    this->hashesSentThrough = 0;
    this->info.desyncs.clear();
    std::memset(this->portTable, 0, sizeof(this->portTable));

    for (int i = 0; i < 4; i++)
    {
        this->owners[i] = owners[i];
        if (owners[i] != NoPlayer)
        {
            Stream& stream = this->streams[owners[i]];
            stream.player = owners[i];
            stream.ports.push_back(i);
        }
    }

    for (auto& [player, stream] : this->streams)
    {
        stream.latest.assign(stream.ports.size(), 0);
        stream.pendingEvents.assign(stream.ports.size(), {});
        auto it = authorities.find(player);
        stream.authority = it != authorities.end() ? it->second : player;
        stream.handoffs  = {{0, stream.authority}};
    }
}

void Session::EndGameLocked(void)
{
    this->gameRunning  = false;
    this->info.playing = false;
    this->info.paused  = false;
}

std::vector<uint8_t> Session::BuildFramesLocked(uint8_t recipient)
{
    // message: u8 MsgFrames,
    //          u8 streams, streams * (u8 player, u32 first, u16 count, count * ports * (u32 buttons, u8 event)),
    //          u8 acks, acks * (u8 player, u32 frames received)
    //
    // Both ends of a link send each other whatever frames the other doesn't
    // have yet, for every stream, and acknowledge everything they have. That
    // way it doesn't matter who wrote a frame or where it came from.
    PacketWriter message;
    message.U8(MsgFrames);

    size_t countOffset = message.Size();
    uint8_t streamCount = 0;
    message.U8(0);

    std::map<uint8_t, uint32_t>& recipientAcks = this->acks[recipient];

    for (auto& [player, stream] : this->streams)
    {
        uint32_t first = std::min(recipientAcks[player], stream.count);
        uint32_t count = std::min(stream.count - first, MaxFramesPerMessage);
        if (count == 0)
        {
            continue;
        }

        message.U8(player).U32(first).U16(static_cast<uint16_t>(count));
        size_t width = stream.ports.size();
        for (uint32_t f = first; f < first + count; f++)
        {
            for (size_t k = 0; k < width; k++)
            {
                message.U32(stream.frames[static_cast<size_t>(f) * width + k]);
                message.U8(stream.events[static_cast<size_t>(f) * width + k]);
            }
        }
        streamCount++;
    }
    message.Patch(countOffset, streamCount);

    message.U8(static_cast<uint8_t>(this->streams.size()));
    for (auto& [player, stream] : this->streams)
    {
        message.U8(player).U32(stream.count);
    }

    return message.Data();
}

void Session::SendFramesTo(PeerId peer, uint8_t recipient)
{
    this->transport.Send(peer, Channel::Input, this->BuildFramesLocked(recipient));
    this->lastFramesSent[peer] = now_ms();
}

void Session::SendFramesLocked(void)
{
    if (this->info.isHost)
    {
        for (auto& [player, peer] : this->playerPeers)
        {
            this->SendFramesTo(peer, player);
        }
    }
    else if (this->hostPeer != InvalidPeer)
    {
        this->SendFramesTo(this->hostPeer, HostPlayer);
    }
}

PlayerInfo* Session::FindPlayerLocked(uint8_t id)
{
    for (PlayerInfo& player : this->info.players)
    {
        if (player.id == id)
        {
            return &player;
        }
    }
    return nullptr;
}

std::string Session::PlayerNameLocked(uint8_t id)
{
    PlayerInfo* player = this->FindPlayerLocked(id);
    return player != nullptr ? player->name : "Player " + std::to_string(id + 1);
}

//
// Authority
//

int64_t Session::BusyLocked(void)
{
    // the 90th percentile: the slower frames matter, a rare spike doesn't
    if (this->busySamples.size() < 30)
    {
        return -1;
    }
    std::vector<uint32_t> sorted = this->busySamples;
    size_t index = sorted.size() * 9 / 10;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(index), sorted.end());
    return sorted[index];
}

uint32_t Session::LagLocked(const Stream& stream)
{
    // the lag only exists to give the other players' frames time to arrive in
    // Lockstep; an authority waits for no one, so its frames apply at once
    if (this->info.mode != Mode::Lockstep)
    {
        return 0;
    }
    const PlayerInfo* owner = this->info.Find(stream.player);
    return owner != nullptr ? owner->lag : 0;
}

void Session::SetFrameLocked(Stream& stream, uint32_t frame, const std::vector<uint32_t>& values,
                             const std::vector<uint8_t>& events)
{
    size_t width = stream.ports.size();
    if (stream.known.size() <= frame)
    {
        stream.known.resize(static_cast<size_t>(frame) + 1, 0);
        stream.frames.resize((static_cast<size_t>(frame) + 1) * width, 0);
        stream.events.resize((static_cast<size_t>(frame) + 1) * width, 0);
    }
    for (size_t k = 0; k < width; k++)
    {
        stream.frames[static_cast<size_t>(frame) * width + k] = values[k];
        stream.events[static_cast<size_t>(frame) * width + k] = events[k];
    }
    stream.known[frame] = 1;

    while (stream.count < stream.known.size() && stream.known[stream.count])
    {
        stream.count++;
    }
}

void Session::WriteAuthoredLocked(Stream& stream, int64_t throughFrame)
{
    // An authority writes frames up to `throughFrame`. When the lag went down,
    // those frames may already be written, so nothing happens; when it went
    // up, or the authority just took the stream over, the frames in between
    // get the owner's latest input.
    if (stream.authority != this->info.myId || throughFrame < static_cast<int64_t>(stream.nextWrite))
    {
        return;
    }
    std::vector<uint8_t> events(stream.ports.size());
    for (int64_t f = stream.nextWrite; f <= throughFrame; f++)
    {
        // one waiting event per port goes into this frame, and only this one
        for (size_t k = 0; k < events.size(); k++)
        {
            events[k] = 0;
            if (!stream.pendingEvents[k].empty())
            {
                events[k] = stream.pendingEvents[k].front();
                stream.pendingEvents[k].pop_front();
            }
        }
        this->SetFrameLocked(stream, static_cast<uint32_t>(f), stream.latest, events);
    }
    stream.nextWrite = static_cast<uint32_t>(throughFrame + 1);
}

void Session::DelegateLocked(Stream& stream, uint8_t to)
{
    // Only the authority hands a stream over: it stops writing and names the
    // first frame the next authority writes, so no frame is written twice.
    if (stream.authority != this->info.myId || to == this->info.myId)
    {
        return;
    }

    uint32_t from = stream.nextWrite;
    stream.authority = to;
    this->NoteHandoffLocked(stream, to, from);

    PacketWriter message;
    message.U8(MsgDelegate).U8(stream.player).U8(to).U32(from);
    if (this->info.isHost)
    {
        this->BroadcastLocked(message.Data());
    }
    else if (this->hostPeer != InvalidPeer)
    {
        this->transport.Send(this->hostPeer, Channel::Control, message.Data());
    }

    // events this machine didn't get to write follow the stream (after the
    // handover, on the same ordered channel, so they arrive in order)
    for (size_t k = 0; k < stream.pendingEvents.size(); k++)
    {
        std::deque<uint8_t> pending;
        pending.swap(stream.pendingEvents[k]);
        for (uint8_t event : pending)
        {
            this->RouteEventLocked(stream, k, event);
        }
    }
}

void Session::RouteEventLocked(Stream& stream, size_t port, uint8_t event)
{
    // the authority writes it into its next frame; anyone else passes it on,
    // guests through the host (which knows who the authority is)
    if (stream.authority == this->info.myId)
    {
        stream.pendingEvents[port].push_back(event);
        return;
    }

    PacketWriter message;
    message.U8(MsgEvent).U8(stream.player).U8(static_cast<uint8_t>(port)).U8(event);
    if (this->info.isHost)
    {
        this->SendToPlayerLocked(stream.authority, Channel::Control, message.Data());
    }
    else if (this->hostPeer != InvalidPeer)
    {
        this->transport.Send(this->hostPeer, Channel::Control, message.Data());
    }
}

void Session::NoteHandoffLocked(Stream& stream, uint8_t to, uint32_t from)
{
    // handoffs come in order; one from the same frame replaces the last
    // (the host notes its own handoffs twice)
    while (!stream.handoffs.empty() && stream.handoffs.back().from >= from)
    {
        stream.handoffs.pop_back();
    }
    stream.handoffs.push_back({from, to});

    // the ones whose frames have all arrived can't matter anymore
    while (stream.handoffs.size() >= 2 && stream.handoffs[1].from <= stream.count)
    {
        stream.handoffs.erase(stream.handoffs.begin());
    }
}

void Session::TakeOverLocked(Stream& stream, uint8_t player)
{
    // The host takes over what a player who left was writing. It relays only
    // unbroken runs of frames, so frames from them it doesn't have reached
    // no one else either, and it can write those itself.
    while (stream.handoffs.size() >= 2 && stream.handoffs[1].from <= stream.count)
    {
        stream.handoffs.erase(stream.handoffs.begin());
    }

    size_t width = stream.ports.size();

    // an earlier part they wrote, whose last frames never arrived: they're
    // written idle (the frames before and after it come from others)
    std::vector<uint32_t> idle(width, 0);
    std::vector<uint8_t> noEvents(width, 0);
    for (size_t i = 0; i + 1 < stream.handoffs.size(); i++)
    {
        if (stream.handoffs[i].authority != player)
        {
            continue;
        }
        for (uint32_t f = std::max(stream.count, stream.handoffs[i].from); f < stream.handoffs[i + 1].from; f++)
        {
            if (f >= stream.known.size() || !stream.known[f])
            {
                this->SetFrameLocked(stream, f, idle, noEvents);
            }
        }
        stream.handoffs[i].authority = HostPlayer;
    }

    if (stream.authority != player)
    {
        return;
    }

    // their own part: what the host has of it past `count` never left the
    // host, so it's dropped and the host writes it from there on. Frames
    // before their part may still be on the way from an earlier authority;
    // those are kept and still accepted.
    uint32_t start = std::max(stream.count, stream.handoffs.empty() ? 0u : stream.handoffs.back().from);
    if (stream.known.size() > start)
    {
        stream.frames.resize(static_cast<size_t>(start) * width);
        stream.events.resize(static_cast<size_t>(start) * width);
        stream.known.resize(start);
    }
    stream.authority = HostPlayer;
    stream.nextWrite = start;
    this->NoteHandoffLocked(stream, HostPlayer, start);

    PacketWriter message;
    message.U8(MsgDelegate).U8(stream.player).U8(HostPlayer).U32(start);
    this->BroadcastLocked(message.Data());

    this->WriteAuthoredLocked(stream, this->lastSampled + this->LagLocked(stream));
}

void Session::ApplyDelegateLocked(Stream& stream, uint8_t to, uint32_t from)
{
    this->NoteHandoffLocked(stream, to, from);
    stream.authority = to;
    if (to != this->info.myId)
    {
        return;
    }

    // this machine writes the stream from `from` on; it may not have the
    // frames before it yet, but they're on their way
    stream.nextWrite = from;
    if (stream.player == this->info.myId)
    {
        stream.latestSequence = 0;
    }

    // catch up to where this machine's emulation is, so it doesn't wait on itself
    if (this->lastSampled >= 0)
    {
        this->WriteAuthoredLocked(stream, this->lastSampled + this->LagLocked(stream));
    }
    this->SendFramesLocked();
}

uint8_t Session::DesiredAuthorityLocked(const Stream& stream)
{
    auto present = [&](uint8_t player) { return player == HostPlayer || this->playerPeers.count(player) != 0; };

    switch (this->info.mode)
    {
    case Mode::Lockstep:
        return present(stream.player) ? stream.player : HostPlayer;
    case Mode::Authority:
        return present(this->info.authorityPlayer) ? this->info.authorityPlayer : HostPlayer;
    case Mode::Golf:
    default:
        // whoever is playing; the authorities sort it out among themselves
        return NoPlayer;
    }
}

void Session::EnforceModeLocked(bool now)
{
    // The host makes the authorities match the mode: it hands over the
    // streams it writes itself and asks the other authorities to hand over
    // theirs (again, if a request crossed a handover).
    uint64_t time = now_ms();
    for (auto& [player, stream] : this->streams)
    {
        uint8_t desired = this->DesiredAuthorityLocked(stream);
        if (desired == NoPlayer || stream.authority == desired)
        {
            continue;
        }

        if (stream.authority == HostPlayer)
        {
            this->DelegateLocked(stream, desired);
            this->ApplyDelegateLocked(stream, desired, stream.nextWrite);
        }
        else if (now || time - stream.lastRequest >= RequestIntervalMs)
        {
            PacketWriter request;
            request.U8(MsgRequestAuthority).U8(player).U8(desired);
            this->SendToPlayerLocked(stream.authority, Channel::Control, request.Data());
            stream.lastRequest = time;
        }
    }
}

void Session::ApplyPauseLocked(bool paused, const std::string& by, std::vector<Event>& events)
{
    if (this->info.paused == paused)
    {
        return;
    }
    this->info.paused   = paused;
    this->info.pausedBy = by;
    events.push_back({paused ? EventType::Paused : EventType::Resumed, by, {}});
    events.push_back({EventType::StateChanged, "", {}});
}

//
// Core callbacks
//

int Session::CoreSync(void* context, m64p_netplay_new_sync kind, void* data, unsigned int size)
{
    return static_cast<Session*>(context)->Sync(kind, data, size);
}

int Session::CoreConfigurePorts(void* context, const m64p_netplay_new_port local[4], m64p_netplay_new_port ports[4])
{
    return static_cast<Session*>(context)->ConfigurePorts(local, ports);
}

void Session::CoreSample(void* context, unsigned int frame, const m64p_netplay_new_input local[4])
{
    static_cast<Session*>(context)->Sample(frame, local);
}

int Session::CoreGetInput(void* context, unsigned int frame, int port, m64p_netplay_new_input* input)
{
    return static_cast<Session*>(context)->GetInput(frame, port, input);
}

int Session::CoreFrameEnd(void* context, unsigned int frame, unsigned int busyUs, unsigned int frameUs)
{
    return static_cast<Session*>(context)->FrameEnd(frame, busyUs, frameUs);
}

void Session::CoreFrameHash(void* context, unsigned int frame, uint64_t hash)
{
    static_cast<Session*>(context)->FrameHash(frame, hash);
}

void Session::FrameHash(unsigned int frame, uint64_t hash)
{
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->ended || !this->gameRunning)
        {
            return;
        }

        this->myHashes[frame] = hash;
        while (!this->myHashes.empty() && this->myHashes.begin()->first + HashWindow < frame)
        {
            this->myHashes.erase(this->myHashes.begin());
        }

        if (this->info.isHost)
        {
            // compare with the hashes that arrived before the host got here
            for (auto& [player, hashes] : this->otherHashes)
            {
                auto it = hashes.find(frame);
                if (it != hashes.end())
                {
                    uint64_t theirs = it->second;
                    hashes.erase(it);
                    this->CompareHashLocked(player, frame, theirs, events);
                }
                while (!hashes.empty() && hashes.begin()->first + HashWindow < frame)
                {
                    hashes.erase(hashes.begin());
                }
            }
        }
        else if (frame + 1 >= this->hashesSentThrough + HashBatch && this->hostPeer != InvalidPeer)
        {
            // the last two batches: each hash goes out twice, in case a message is lost
            uint32_t first = frame + 1 >= 2 * HashBatch ? frame + 1 - 2 * HashBatch : 0;
            PacketWriter message;
            size_t countOffset;
            message.U8(MsgHashes).U32(first);
            countOffset = message.Size();
            message.U8(0);
            uint8_t count = 0;
            for (uint32_t f = first; f <= frame; f++)
            {
                auto it = this->myHashes.find(f);
                if (it == this->myHashes.end())
                {
                    break;
                }
                message.I64(static_cast<int64_t>(it->second));
                count++;
            }
            message.Patch(countOffset, count);
            this->transport.Send(this->hostPeer, Channel::Input, message.Data());
            this->hashesSentThrough = frame + 1;
        }
    }
    this->Emit(events);
}

int Session::Sync(m64p_netplay_new_sync kind, void* data, unsigned int size)
{
    std::unique_lock<std::mutex> lock(this->mutex);

    if (this->info.isHost)
    {
        if (!this->gameRunning)
        {
            return 0;
        }

        PacketWriter message;
        message.U8(MsgSync).U8(static_cast<uint8_t>(kind)).Bytes(data, size);
        this->BroadcastLocked(message.Data());
        return 1;
    }

    this->condition.wait(lock, [&] {
        return this->ended || !this->gameRunning || this->syncData.count(kind) != 0;
    });

    auto it = this->syncData.find(kind);
    if (it == this->syncData.end() || it->second.size() != size)
    {
        return 0;
    }

    if (size > 0)
    {
        std::memcpy(data, it->second.data(), size);
    }
    return 1;
}

int Session::ConfigurePorts(const m64p_netplay_new_port local[4], m64p_netplay_new_port ports[4])
{
    std::unique_lock<std::mutex> lock(this->mutex);

    if (!this->gameRunning)
    {
        return 0;
    }

    uint8_t me = this->info.myId;

    // this machine's first controller drives its first port, the second its second, ...
    std::vector<int> myPorts;
    auto mine = this->streams.find(me);
    if (mine != this->streams.end())
    {
        myPorts = mine->second.ports;
    }

    std::vector<uint8_t> myPlugins;
    for (size_t k = 0; k < myPorts.size(); k++)
    {
        myPlugins.push_back(static_cast<uint8_t>(local[k].present ? local[k].plugin : PLUGIN_NONE));
    }

    if (this->info.isHost)
    {
        this->portPlugins[me] = myPlugins;

        // wait until every player with ports told us their paks
        this->condition.wait(lock, [&] {
            if (this->ended || !this->gameRunning)
            {
                return true;
            }
            for (auto& [player, stream] : this->streams)
            {
                if (this->portPlugins.count(player) == 0 && (player == HostPlayer || this->playerPeers.count(player) != 0))
                {
                    return false;
                }
            }
            return true;
        });

        if (this->ended || !this->gameRunning)
        {
            return 0;
        }

        for (int i = 0; i < 4; i++)
        {
            this->portTable[i].present = 0;
            this->portTable[i].plugin  = PLUGIN_NONE;
        }
        for (auto& [player, stream] : this->streams)
        {
            const std::vector<uint8_t>& plugins = this->portPlugins[player];
            for (size_t k = 0; k < stream.ports.size(); k++)
            {
                int port = stream.ports[k];
                this->portTable[port].present = 1;
                this->portTable[port].plugin  = k < plugins.size() ? plugins[k] : PLUGIN_NONE;
            }
        }
        this->hasPortTable = true;

        PacketWriter message;
        message.U8(MsgPortTable);
        for (int i = 0; i < 4; i++)
        {
            message.U8(static_cast<uint8_t>(this->portTable[i].present)).U8(static_cast<uint8_t>(this->portTable[i].plugin));
        }
        this->BroadcastLocked(message.Data());
    }
    else
    {
        if (this->hostPeer == InvalidPeer)
        {
            return 0;
        }

        if (!myPorts.empty())
        {
            PacketWriter message;
            message.U8(MsgPortInfo).U8(static_cast<uint8_t>(myPlugins.size()));
            for (uint8_t plugin : myPlugins)
            {
                message.U8(plugin);
            }
            this->transport.Send(this->hostPeer, Channel::Control, message.Data());
        }

        this->condition.wait(lock, [&] { return this->ended || !this->gameRunning || this->hasPortTable; });
        if (!this->hasPortTable || this->ended || !this->gameRunning)
        {
            return 0;
        }
    }

    for (int i = 0; i < 4; i++)
    {
        ports[i].present          = this->portTable[i].present;
        ports[i].plugin           = this->portTable[i].plugin;
        ports[i].local_controller = -1;
    }
    for (size_t k = 0; k < myPorts.size(); k++)
    {
        ports[myPorts[k]].local_controller = static_cast<int>(k);
    }

    return 1;
}

void Session::Sample(unsigned int frame, const m64p_netplay_new_input local[4])
{
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (!this->gameRunning)
        {
            return;
        }

        this->lastSampled = frame;
        uint8_t me = this->info.myId;

        // this machine's controllers drive its own stream
        auto mine = this->streams.find(me);
        if (mine != this->streams.end())
        {
            Stream& stream = mine->second;
            for (size_t k = 0; k < stream.ports.size(); k++)
            {
                stream.latest[k] = local[k].buttons;
                // a pak switch goes into exactly one frame, wherever this stream is written
                if (local[k].pak >= 0)
                {
                    this->RouteEventLocked(stream, k, static_cast<uint8_t>(local[k].pak));
                }
            }

            // another machine writes this stream: send it the input
            if (stream.authority != me)
            {
                PacketWriter message;
                message.U8(MsgInputUpdate).U8(me).U32(++this->updateSequence).U8(static_cast<uint8_t>(stream.latest.size()));
                for (uint32_t value : stream.latest)
                {
                    message.U32(value);
                }
                this->SendToPlayerLocked(stream.authority, Channel::Input, message.Data());
            }
        }

        // write every stream this machine is the authority of
        for (auto& [player, stream] : this->streams)
        {
            this->WriteAuthoredLocked(stream, static_cast<int64_t>(frame) + this->LagLocked(stream));
        }

        this->SendFramesLocked();
    }

    this->condition.notify_all();
}

int Session::GetInput(unsigned int frame, int port, m64p_netplay_new_input* input)
{
    std::unique_lock<std::mutex> lock(this->mutex);

    input->buttons = 0;
    input->pak     = -1;

    if (port < 0 || port > 3 || this->owners[port] == NoPlayer)
    {
        return this->gameRunning ? 1 : 0;
    }

    auto it = this->streams.find(this->owners[port]);
    if (it == this->streams.end())
    {
        return 0;
    }
    Stream& stream = it->second;

    // the lock is let go while waiting; if the game ended and another one
    // started meanwhile, the streams were replaced, so stream isn't valid
    const uint32_t game = this->gameNumber;
    auto sameGame = [&]() { return !this->ended && this->gameRunning && this->gameNumber == game; };

    uint64_t waitStart = now_ms();
    bool noticed = false;

    while (sameGame() && stream.count <= frame)
    {
        this->condition.wait_for(lock, std::chrono::milliseconds(50));

        // while paused, everyone waits on purpose
        if (this->info.paused)
        {
            waitStart = now_ms();
        }

        if (!noticed && sameGame() && now_ms() - waitStart >= WaitingNoticeMs && stream.count <= frame)
        {
            noticed = true;
            std::string name = this->PlayerNameLocked(stream.authority);
            lock.unlock();
            this->Emit(EventType::Waiting, name);
            lock.lock();
        }
    }

    if (noticed)
    {
        lock.unlock();
        this->Emit(EventType::WaitingDone);
        lock.lock();
    }

    if (!sameGame())
    {
        return 0;
    }

    size_t k = std::find(stream.ports.begin(), stream.ports.end(), port) - stream.ports.begin();
    size_t index = static_cast<size_t>(frame) * stream.ports.size() + k;
    input->buttons = stream.frames[index];
    input->pak     = stream.events[index] != 0 ? stream.events[index] : -1;
    return 1;
}

int Session::FrameEnd(unsigned int frame, unsigned int busyUs, unsigned int frameUs)
{
    std::unique_lock<std::mutex> lock(this->mutex);

    if (frameUs >= 10000 && frameUs <= 50000)
    {
        this->frameUs = frameUs;
    }

    // about two seconds of frames, for the automatic lag
    constexpr size_t BusySamples = 120;
    if (this->busySamples.size() < BusySamples)
    {
        this->busySamples.push_back(busyUs);
    }
    else
    {
        this->busySamples[this->busyNext] = busyUs;
        this->busyNext = (this->busyNext + 1) % BusySamples;
    }

    // a paused game waits here, at the end of a frame
    while (this->info.paused && !this->ended && this->gameRunning)
    {
        this->condition.wait_for(lock, std::chrono::milliseconds(50));
    }

    if (this->ended || !this->gameRunning)
    {
        return -1;
    }

    // Catching up: if the frames this machine waits for are further ahead
    // than their lag explains, it fell behind, so it runs without the speed
    // limiter until it's back. A machine that writes no frames (a spectator,
    // or anyone but the authority) keeps a small buffer instead.
    bool writesAny = false;
    for (auto& [player, stream] : this->streams)
    {
        writesAny = writesAny || stream.authority == this->info.myId;
    }

    int64_t leastAhead = INT64_MAX;
    for (auto& [player, stream] : this->streams)
    {
        if (stream.authority == this->info.myId)
        {
            continue;
        }
        int64_t ahead = static_cast<int64_t>(stream.count) - static_cast<int64_t>(frame) - 1;
        if (writesAny)
        {
            ahead -= this->LagLocked(stream);
        }
        leastAhead = std::min(leastAhead, ahead);
    }

    if (leastAhead == INT64_MAX)
    {
        return 0;
    }
    return writesAny ? (leastAhead >= 2) : (leastAhead >= 4);
}
