/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMG_NETPLAY_NEW_SESSION_HPP
#define RMG_NETPLAY_NEW_SESSION_HPP

#include "Nat.hpp"
#include "Packet.hpp"
#include "Transport.hpp"

#include <RMG-Core/m64p/api/m64p_types.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace NetplayNew
{
// bump whenever messages change; machines with different versions can't play together
constexpr uint32_t ProtocolVersion = 5;
constexpr uint16_t DefaultPort     = 6420;
// players and spectators, including the host
constexpr int MaxPlayers = 8;
constexpr uint8_t NoPlayer = 0xFF;
constexpr uint8_t HostPlayer = 0;

// Who writes each player's input frames
enum class Mode : uint8_t
{
    // every player writes their own input and waits for everyone else's;
    // one player lagging lags everyone
    Lockstep = 0,
    // one player writes everyone's input from what they send; only the
    // players who lag fall behind
    Authority = 1,
    // like Authority, but whoever starts playing becomes the authority
    Golf = 2,
};

// Games that are best played in golf mode, and which buttons count as
// "starting to play" in them (the rest, like looking around, don't).
// Returns false for other games.
bool IsGolfGame(const std::string& internalName, uint32_t& mask);

struct PlayerInfo
{
    uint8_t id = NoPlayer;
    std::string name;
    // bit i set: this player has N64 port i+1. No ports: spectator
    uint8_t ports = 0;
    // input delay in frames (VIs), used in Lockstep mode
    uint8_t lag = 0;
    // round trip time to the host in milliseconds
    uint16_t ping = 0;
    // whether this player has the session's game
    bool hasGame = false;
    std::string version;
    std::string rspPlugin;
    std::string gfxPlugin;
    // empty when no execution plugin is attached
    std::string executionPlugin;
};

struct SessionInfo
{
    bool connected = false;
    bool isHost    = false;
    uint8_t myId   = NoPlayer;
    std::string gameName;
    std::string gameMD5;
    // whether a game is being played
    bool playing = false;
    std::vector<PlayerInfo> players;

    Mode mode = Mode::Lockstep;
    // in Authority mode: who writes everyone's input
    uint8_t authorityPlayer = HostPlayer;
    // in Golf mode: input that hands the authority over (see IsGolfGame)
    uint32_t golfMask = 0xFFFFFFFF;

    bool paused = false;
    std::string pausedBy;

    // host: whether the router forwards the session's UDP port (see Session::MapPort)
    enum class PortMapping
    {
        Off,
        Pending,
        Mapped,
        Failed,
    };
    PortMapping portMapping = PortMapping::Off;
    uint16_t port = 0;
    // how it was mapped ("UPnP" or "NAT-PMP"), the router's public IPv4 address,
    // and why it failed or a warning
    std::string portMappingMethod;
    std::string publicAddress;
    std::string portMappingMessage;

    // during a game: who writes each player's input (player -> player)
    std::map<uint8_t, uint8_t> inputBy;

    // during a game: players whose game drifted apart from the host's, and the
    // first frame where it was noticed (player -> frame)
    std::map<uint8_t, uint32_t> desyncs;

    const PlayerInfo* Find(uint8_t id) const
    {
        for (const PlayerInfo& player : this->players)
        {
            if (player.id == id)
            {
                return &player;
            }
        }
        return nullptr;
    }
};

// what this machine tells the others about itself
struct LocalInfo
{
    std::string name;
    std::string version;
    std::string rspPlugin;
    std::string gfxPlugin;
    std::string executionPlugin;
};

enum class EventType
{
    // joined the host's session (guest)
    Connected,
    // the players, ports, lag, pings, mode or authorities changed
    StateChanged,
    // a game starts now; the cheats are the host's
    GameStarting,
    // the game ended for everyone; text says why
    GameEnded,
    // waiting for another player's input; text names them
    Waiting,
    WaitingDone,
    // the game was paused or resumed; text names who did it
    Paused,
    Resumed,
    // a player's game drifted apart from the host's; text says who and where
    Desync,
    // something to tell the user
    Message,
    // the session is over; text says why
    Ended,
};

struct Event
{
    EventType type;
    std::string text;
    std::vector<uint8_t> cheats;
};

// A session of the new netplay, as the host or as a guest.
//
// The host relays everything between guests (star topology). Every player
// has a stream of input frames, one frame per VI. Each stream has exactly
// one authority, the only machine that writes its frames: its owner in
// Lockstep mode, one player in Authority mode, whoever is playing in Golf
// mode. An authority that isn't the owner writes the owner's frames from
// the input updates the owner sends it. Handing a stream to another
// authority names the first frame the new authority writes, so every frame
// is still written exactly once.
//
// Input frames are kept for the whole game, indexed by frame number, and a
// frame is never written twice. This keeps rollback possible later: a
// rollback policy could predict missing frames and correct them once the
// real frames arrive, without changing how frames are written or sent.
//
// Events are delivered on the network or emulation thread; the handler
// must not call back into the session synchronously.
class Session
{
  public:
    using EventHandler = std::function<void(const Event&)>;

    explicit Session(EventHandler handler);
    ~Session(void);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // gameInternalName is the name in the ROM header; golf games start in Golf mode
    bool Host(uint16_t port, const LocalInfo& local, const std::string& gameName, const std::string& gameMD5,
              const std::string& gameInternalName, std::string& error);
    bool Join(const std::string& address, uint16_t port, const LocalInfo& local, std::string& error);

    // leaves the session (as the host: ends it for everyone). A guest who
    // leaves during a game hands their input to the host, which keeps their
    // controller idle, and the game goes on.
    void Leave(void);

    SessionInfo GetInfo(void);

    // guest: tells the host whether this machine has the session's game, and
    // which plugins it would play it with (a game can override the plugins)
    void SetHasGame(bool hasGame, const LocalInfo& plugins);

    // host: assigns N64 ports (a bitmask) to a player, taking them from anyone else
    void SetPorts(uint8_t player, uint8_t ports);
    // host: fixes a player's input delay in frames, which the automatic lag
    // then leaves alone (the window doesn't offer this; the tests use it)
    void SetLag(uint8_t player, uint8_t lag);
    // host: who writes input, also during a game; authorityPlayer is for Authority mode
    void SetMode(Mode mode, uint8_t authorityPlayer = HostPlayer);
    // host: whether the game can start; reason says why not
    bool CanStart(std::string& reason);
    // host: starts the game for everyone
    bool Start(const std::vector<uint8_t>& cheats, std::string& error);

    // anyone: pauses or resumes the game for everyone
    void SetPaused(bool paused);

    // host: asks the router to forward the session's UDP port, in the
    // background (see SessionInfo::portMapping); the session renews the
    // mapping while it runs and removes it when it ends
    void MapPort(void);

    // the core callbacks for this session (the context is this session)
    const m64p_netplay_new_callbacks* GetCallbacks(void);

    // the game stopped on this machine (the user closed it or it failed)
    void GameStopped(void);

  private:
    struct Stream
    {
        uint8_t player = NoPlayer;
        // the stream's N64 ports, in ascending order
        std::vector<int> ports;
        // ports.size() values per frame: the buttons, and an event (0: none,
        // otherwise the pak the port switches to on that frame);
        // known[f] says whether frame f arrived
        std::vector<uint32_t> frames;
        std::vector<uint8_t> events;
        std::vector<uint8_t> known;
        // frames 0..count-1 are all known
        uint32_t count = 0;
        // who writes this stream now
        uint8_t authority = NoPlayer;
        // who wrote which frames: each authority in turn and the first frame
        // they write, oldest first, kept from the one that wrote frame `count`
        // on (earlier ones can't matter anymore). The host uses it when a
        // player leaves: frames before their part may still be on the way
        // from an earlier authority who's still here.
        struct Handoff
        {
            uint32_t from;
            uint8_t authority;
        };
        std::vector<Handoff> handoffs;
        // when this machine is the authority: the next frame it writes
        uint32_t nextWrite = 0;
        // the owner's latest input: their local input on their machine,
        // and the input updates they send on the authority's
        std::vector<uint32_t> latest;
        uint32_t latestSequence = 0;
        // when this machine is the authority: events (per port) waiting for
        // the next frame it writes; each goes into exactly one frame
        std::vector<std::deque<uint8_t>> pendingEvents;
        // host: when it last asked the authority to hand this stream over
        uint64_t lastRequest = 0;
    };

    std::mutex mutex;
    std::condition_variable condition;
    EventHandler handler;
    Transport transport;
    m64p_netplay_new_callbacks callbacks;

    // session
    SessionInfo info;
    LocalInfo local;
    bool ended = false;
    uint8_t nextPlayerId = 1;
    PeerId hostPeer = InvalidPeer;                  // guest
    std::map<PeerId, uint8_t> peerPlayers;          // host: peer -> player (once said hello)
    std::map<uint8_t, PeerId> playerPeers;          // host: player -> peer
    uint64_t lastStateBroadcast = 0;

    // host: the automatic lag (see AdjustLagLocked)
    struct LagState
    {
        // the round trip time's variation to the host, in milliseconds
        uint32_t jitter = 0;
        // SetLag fixed this player's lag
        bool fixed = false;
        // how many checks in a row found the lag higher than needed
        int lowerChecks = 0;
        // how long their frames take to emulate (the 90th percentile of
        // recent frames) in microseconds, -1 until they've played a while
        int64_t busyUs = -1;
    };
    std::map<uint8_t, LagState> lagStates;
    // how long a frame lasts at full speed, from the core (PAL games run at 50 frames a second)
    uint32_t frameUs = 1000000 / 60;

    // this machine: how long its recent frames took to emulate, in microseconds
    std::vector<uint32_t> busySamples;
    size_t busyNext = 0;
    uint64_t lastBusySent = 0;

    // host: port mapping on the router
    std::unique_ptr<PortMapper> portMapper;
    std::thread portMappingThread;
    std::atomic<bool> portMappingRunning{false};
    uint64_t lastPortMapping = 0;

    // game
    bool gameRunning = false;
    // counts the games started, so a wait that outlives its game notices
    uint32_t gameNumber = 0;
    uint8_t owners[4] = {NoPlayer, NoPlayer, NoPlayer, NoPlayer};
    std::map<uint8_t, Stream> streams;
    // what the link partner has of each stream: on the host [guest][stream],
    // on a guest [HostPlayer][stream]
    std::map<uint8_t, std::map<uint8_t, uint32_t>> acks;
    std::map<PeerId, uint64_t> lastFramesSent;
    std::map<int, std::vector<uint8_t>> syncData;
    std::map<uint8_t, std::vector<uint8_t>> portPlugins; // host: player -> plugin per owned port
    bool hasPortTable = false;
    m64p_netplay_new_port portTable[4];
    // the last frame this machine sampled (-1: none yet)
    int64_t lastSampled = -1;
    uint32_t updateSequence = 0;

    // desync detection: this machine's hashes, and on the host everyone's
    // hashes that it can't compare yet (player -> frame -> hash)
    std::map<uint32_t, uint64_t> myHashes;
    std::map<uint8_t, std::map<uint32_t, uint64_t>> otherHashes;
    uint32_t hashesSentThrough = 0;

    // network handlers (transport thread)
    void OnConnected(PeerId peer);
    void OnDisconnected(PeerId peer);
    void OnReceived(PeerId peer, Channel channel, const uint8_t* data, size_t size);
    void OnTick(void);
    void StartPortMappingLocked(void);

    void HandleHostMessage(PeerId peer, uint8_t type, PacketReader& reader, std::vector<Event>& events);
    void HandleGuestMessage(uint8_t type, PacketReader& reader, std::vector<Event>& events);
    void HandleFrames(PeerId peer, PacketReader& reader);
    void HandleInputUpdate(PeerId peer, PacketReader& reader);
    void HandleHashes(PeerId peer, PacketReader& reader);
    void CompareHashLocked(uint8_t player, uint32_t frame, uint64_t hash, std::vector<Event>& events);

    // helpers; the ones ending in Locked expect the mutex to be held
    void Emit(const std::vector<Event>& events);
    void Emit(EventType type, const std::string& text = "");
    std::vector<uint8_t> BuildStateLocked(void);
    void BroadcastLocked(const std::vector<uint8_t>& message, uint8_t exceptPlayer = NoPlayer);
    void SendToPlayerLocked(uint8_t player, Channel channel, const std::vector<uint8_t>& message);
    void StartGameLocked(Mode mode, const uint8_t owners[4], const std::map<uint8_t, uint8_t>& authorities);
    void EndGameLocked(void);
    std::vector<uint8_t> BuildFramesLocked(uint8_t recipient);
    void SendFramesLocked(void);
    void SendFramesTo(PeerId peer, uint8_t recipient);
    PlayerInfo* FindPlayerLocked(uint8_t id);
    std::string PlayerNameLocked(uint8_t id);

    // authority
    uint32_t LagLocked(const Stream& stream);
    void AdjustLagLocked(uint64_t now);
    int64_t BusyLocked(void);
    void SetFrameLocked(Stream& stream, uint32_t frame, const std::vector<uint32_t>& values, const std::vector<uint8_t>& events);
    void RouteEventLocked(Stream& stream, size_t port, uint8_t event);
    void WriteAuthoredLocked(Stream& stream, int64_t throughFrame);
    void DelegateLocked(Stream& stream, uint8_t to);
    void ApplyDelegateLocked(Stream& stream, uint8_t to, uint32_t from);
    void NoteHandoffLocked(Stream& stream, uint8_t to, uint32_t from);
    void TakeOverLocked(Stream& stream, uint8_t player);
    uint8_t DesiredAuthorityLocked(const Stream& stream);
    void EnforceModeLocked(bool now);
    void ApplyPauseLocked(bool paused, const std::string& by, std::vector<Event>& events);

    // core callbacks (emulation thread)
    static int CoreSync(void* context, m64p_netplay_new_sync kind, void* data, unsigned int size);
    static int CoreConfigurePorts(void* context, const m64p_netplay_new_port local[4], m64p_netplay_new_port ports[4]);
    static void CoreSample(void* context, unsigned int frame, const m64p_netplay_new_input local[4]);
    static int CoreGetInput(void* context, unsigned int frame, int port, m64p_netplay_new_input* input);
    static int CoreFrameEnd(void* context, unsigned int frame, unsigned int busyUs, unsigned int frameUs);
    static void CoreFrameHash(void* context, unsigned int frame, uint64_t hash);

    int Sync(m64p_netplay_new_sync kind, void* data, unsigned int size);
    int ConfigurePorts(const m64p_netplay_new_port local[4], m64p_netplay_new_port ports[4]);
    void Sample(unsigned int frame, const m64p_netplay_new_input local[4]);
    int GetInput(unsigned int frame, int port, m64p_netplay_new_input* input);
    int FrameEnd(unsigned int frame, unsigned int busyUs, unsigned int frameUs);
    void FrameHash(unsigned int frame, uint64_t hash);
};
} // namespace NetplayNew

#endif // RMG_NETPLAY_NEW_SESSION_HPP
