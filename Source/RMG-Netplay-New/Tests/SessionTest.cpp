// Loopback tests for RMG-Netplay-New's Session: a host, a guest player and a
// guest spectator in one process, each driving the core callbacks the way
// mupen64plus-core does, with jitter. Every scenario checks that all
// machines see exactly the same input on every port at every frame (the
// game would desync otherwise), plus what that scenario is about. In
// Rollback mode the stand-in core saves its state, goes back and re-runs
// frames like mupen64plus-core, and what counts is the input every frame
// ended up with.
//
//   RMG_NETPLAY_NEW_TEST_ADDRESS  address to join (default 127.0.0.1; try ::1 or localhost)
//   RMG_NETPLAY_NEW_DROP_PERCENT  drop this many percent of input packets (a scenario may set its own)
//   RMG_NETPLAY_NEW_TEST_ONLY     run only the scenarios whose names contain this
#include <RMG-Netplay-New/Session.hpp>
#include <RMG-Core/m64p/api/m64p_plugin.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

using namespace NetplayNew;
using namespace std::chrono_literals;

constexpr unsigned Frames = 900;
constexpr uint32_t NotRead = 0xFFFFFFFF;
constexpr uint32_t EmptyPort = 0xDEADBEEF;
// the most bytes the session puts into one Frames message (MaxFramesBytes)
constexpr size_t MaxFramesBytes = 1200;

static std::string g_address = "127.0.0.1";
// RMG_NETPLAY_NEW_DROP_PERCENT as the test was started with
static std::string g_dropPercent;

using InputFunction = std::function<uint32_t(uint8_t player, unsigned frame)>;

struct Machine
{
    const char* label;
    std::unique_ptr<Session> session;
    std::atomic<bool> starting{false};
    std::atomic<bool> ended{false};
    std::thread emu;
    uint8_t id = NoPlayer;
    // seen[frame][port]
    std::vector<std::array<uint32_t, 4>> seen;
    std::vector<uint8_t> syncedSram;
    int64_t clock = 0;
    m64p_netplay_new_port ports[4];
    bool ok = true;
    int caughtUp = 0;
    unsigned framesRun = 0;
    // leave the session at this frame (0: don't)
    unsigned leaveAt = 0;
    // how long each frame takes to emulate, as the core would report it
    unsigned busyUs = 3000;
    // who wrote this player's own stream, frame by frame (sampled from GetInfo)
    std::set<uint8_t> authoritiesSeen;
    int authorityChanges = 0;
    std::atomic<int> pausedEvents{0};
    std::atomic<int> desyncEvents{0};
    // pretend this machine's game drifts apart at this frame (0: it doesn't)
    unsigned desyncAt = 0;
    // pak switches as the core gets them: (frame fetched, port, pak)
    std::vector<std::array<int, 3>> paks;
    // rollback, as the stand-in core saw it
    unsigned rollbacks = 0;
    unsigned deepest = 0;
    unsigned slowDowns = 0;
    // frames below this were final when the machine stopped
    unsigned finalFrames = 0;
    // execution plugin messages: the ones this machine's stand-in plugin
    // sent, and the ones each frame delivered (in the end, after re-runs)
    std::vector<std::string> sent;
    std::vector<std::vector<std::string>> delivered;
    bool speaker = false;
    // send a message on every frame from this one for 30 frames (0: don't)
    unsigned messageBurstAt = 0;
    // also send the big messages and the many small ones of large_messages_of
    bool largeMessages = false;
};

// what a machine's stand-in plugin broadcasts, on frames it samples (like a
// server's messages, which arrive in real time, not again when re-running):
// the host often (one of them big, to cross several packets), the others now and then
static std::string message_of(bool isHost, uint8_t id, unsigned frame)
{
    if (frame + 150 >= Frames)
    {
        return std::string();
    }
    if (isHost && frame % 37 == 5)
    {
        std::string message = "h" + std::to_string(frame) + ":";
        message.append(frame == 301 ? 20000 : frame % 300, char('a' + frame % 26));
        return message;
    }
    if (!isHost && frame % 53 == 7 + id)
    {
        return "g" + std::to_string(id) + ":" + std::to_string(frame);
    }
    return std::string();
}

// more of them: the host sends messages of 8 KB, 16 KB and the most a plugin
// may send on three frames in a row, and 40 small ones on one frame (more
// than a frame holds); each guest sends one of 6 KB
static std::vector<std::string> large_messages_of(bool isHost, uint8_t id, unsigned frame)
{
    std::vector<std::string> messages;
    auto padded = [](std::string message, size_t size) {
        message.resize(size, '#');
        return message;
    };
    if (isHost && frame >= 100 && frame <= 102)
    {
        size_t size = frame == 100 ? 8000 : frame == 101 ? 16000 : M64P_NETPLAY_NEW_MAX_MESSAGE;
        messages.push_back(padded("h" + std::to_string(frame) + ":", size));
    }
    if (isHost && frame == 300)
    {
        for (int i = 0; i < 40; i++)
        {
            messages.push_back("h300:many" + std::to_string(i));
        }
    }
    if (!isHost && frame == 250u + id)
    {
        messages.push_back(padded("g" + std::to_string(id) + ":" + std::to_string(frame), 6000));
    }
    return messages;
}

// SetMode, saying why it refused
static void set_mode(Session& session, Mode mode, uint8_t authorityPlayer = HostPlayer)
{
    std::string error;
    if (!session.SetMode(mode, authorityPlayer, error))
    {
        std::printf("  SetMode refused: %s\n", error.c_str());
    }
}

static void set_env(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr)
    {
        setenv(name, value, 1);
    }
    else
    {
        unsetenv(name);
    }
#endif
}

// Pak switches the players make: (player is the host?, frame, pak). The
// player's come in a burst, one of them on a frame the game doesn't read.
struct PakSwitch
{
    bool host;
    unsigned frame;
    int pak;
};
static const PakSwitch PakSwitches[] = {
    {false, 200, PLUGIN_MEMPAK},
    {false, 201, PLUGIN_NONE},
    {false, 213, PLUGIN_RUMBLE_PAK}, // 213 % 7 == 3: the game doesn't read the controllers then
    {true, 450, PLUGIN_RUMBLE_PAK},
};

static int pak_switch_of(bool host, unsigned frame)
{
    for (const PakSwitch& s : PakSwitches)
    {
        if (s.host == host && s.frame == frame)
        {
            return s.pak;
        }
    }
    return -1;
}

static void emulate(Machine& m, bool isHost, InputFunction inputOf)
{
    const m64p_netplay_new_callbacks* cb = m.session->GetCallbacks();
    std::mt19937 rng(m.id * 7 + 1);

    m64p_netplay_new_settings settings{};
    settings.count_per_op = isHost ? 2 : 99;
    if (!cb->sync(cb->context, M64P_NETPLAY_NEW_SYNC_SETTINGS, &settings, sizeof(settings)) || settings.count_per_op != 2)
    {
        std::printf("  [%s] settings sync failed\n", m.label);
        m.ok = false;
        return;
    }
    m.clock = isHost ? 1700000000 : 0;
    cb->sync(cb->context, M64P_NETPLAY_NEW_SYNC_CLOCK, &m.clock, sizeof(m.clock));

    std::vector<uint8_t> sram(0x8000);
    for (size_t i = 0; i < sram.size(); i++)
    {
        sram[i] = isHost ? uint8_t(i * 31) : 0;
    }
    if (!cb->sync(cb->context, M64P_NETPLAY_NEW_SYNC_SRAM, sram.data(), unsigned(sram.size())))
    {
        std::printf("  [%s] sram sync failed\n", m.label);
        m.ok = false;
        return;
    }
    m.syncedSram = sram;

    m64p_netplay_new_port local[4] = {};
    local[0].present = 1;
    local[0].plugin  = PLUGIN_MEMPAK;
    if (!cb->configure_ports(cb->context, local, m.ports))
    {
        std::printf("  [%s] configure_ports failed\n", m.label);
        m.ok = false;
        return;
    }

    m.seen.assign(Frames, {NotRead, NotRead, NotRead, NotRead});
    m.delivered.assign(Frames, {});
    uint8_t lastAuthority = NoPlayer;
    m.speaker = cb->speaker(cb->context) != 0;

    // a machine that fails stops the game for everyone, or they'd wait for it forever
    struct StopOnFailure
    {
        Machine& m;
        ~StopOnFailure()
        {
            if (!m.ok)
            {
                m.session->GameStopped();
            }
        }
    } stopOnFailure{m};
    // stands in for the emulated machine's state: a running hash of all the input it saw
    uint64_t state = 0xcbf29ce484222325ull;

    // Rollback, as mupen64plus-core does it: snapshots of the state at the
    // start of frames the session asks for, going back to one when it says
    // so, and re-running the frames from there (never sampling them again)
    struct Snapshot
    {
        unsigned frame;
        uint64_t state;
        size_t paks;
    };
    std::vector<Snapshot> snapshots;
    unsigned nextSample = 0;
    unsigned rerunUntil = 0;
    unsigned confirmed = 0;

    // at a frame boundary; returns true if the game went back (frame is the
    // frame to run next)
    auto boundary = [&](unsigned& frame) -> bool {
        int want = 0;
        int target = cb->rollback(cb->context, frame, &confirmed, &want);

        if (target >= 0)
        {
            auto it = std::find_if(snapshots.begin(), snapshots.end(), [&](const Snapshot& snapshot) { return snapshot.frame == unsigned(target); });
            if (it == snapshots.end())
            {
                std::printf("  [%s] FAIL: told to go back to frame %d, which wasn't saved\n", m.label, target);
                m.ok = false;
                return false;
            }
            state = it->state;
            m.paks.resize(it->paks);
            snapshots.erase(std::remove_if(snapshots.begin(), snapshots.end(), [&](const Snapshot& snapshot) { return snapshot.frame > unsigned(target); }),
                            snapshots.end());
            rerunUntil = std::max(rerunUntil, frame);
            m.rollbacks++;
            m.deepest = std::max(m.deepest, frame - unsigned(target));
            frame = unsigned(target);
            return true;
        }

        // the newest snapshot of a final frame is as far back as a rollback goes
        const Snapshot* base = nullptr;
        for (const Snapshot& snapshot : snapshots)
        {
            if (snapshot.frame <= confirmed && (base == nullptr || snapshot.frame > base->frame))
            {
                base = &snapshot;
            }
        }
        if (base != nullptr)
        {
            unsigned keep = base->frame;
            for (auto it = snapshots.begin(); it != snapshots.end();)
            {
                if (it->frame < keep)
                {
                    cb->snapshot(cb->context, it->frame, 0);
                    it = snapshots.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // like the core, a frame sometimes can't be saved before the game
        // reads its input (an RSP task was running); it gets no snapshot
        if (want && frame % 11 != 5)
        {
            snapshots.push_back({frame, state, m.paks.size()});
            cb->snapshot(cb->context, frame, 1);
        }
        return false;
    };

    unsigned frame = 0;
    while (m.ok)
    {
        if (frame == Frames)
        {
            // the game is over, but some of its last frames may still turn
            // out to have been guessed wrong
            if (confirmed >= Frames)
            {
                break;
            }
            if (!boundary(frame))
            {
                std::this_thread::sleep_for(2ms);
            }
            continue;
        }

        bool rerun = frame < rerunUntil;
        if (m.leaveAt != 0 && frame == m.leaveAt && !rerun)
        {
            m.finalFrames = confirmed;
            m.session->Leave();
            return;
        }

        // the game reads the controllers on most frames; sampling happens on
        // the first read, once per frame, even if the frame is re-run
        bool reads = (frame % 7) != 3;
        if (frame >= nextSample)
        {
            m64p_netplay_new_input localInput[4] = {{inputOf(m.id, frame), pak_switch_of(isHost, frame)},
                                                    {0, -1}, {0, -1}, {0, -1}};
            cb->sample(cb->context, frame, localInput);
            nextSample = frame + 1;

            std::vector<std::string> messages;
            std::string message = message_of(isHost, m.id, frame);
            if (message.empty() && m.messageBurstAt != 0 && frame >= m.messageBurstAt && frame < m.messageBurstAt + 30)
            {
                message = "h" + std::to_string(frame) + ":burst";
            }
            if (!message.empty())
            {
                messages.push_back(message);
            }
            if (m.largeMessages)
            {
                for (const std::string& large : large_messages_of(isHost, m.id, frame))
                {
                    messages.push_back(large);
                }
            }
            for (const std::string& each : messages)
            {
                if (!cb->send_message(cb->context, each.data(), unsigned(each.size())))
                {
                    std::printf("  [%s] FAIL: couldn't send a message at frame %u\n", m.label, frame);
                    m.ok = false;
                }
                m.sent.push_back(each);
            }
        }

        // like the core: every frame's pak switch is taken once per port, at
        // the first read or at the end of the frame when the game doesn't read
        auto read = [&](int port, uint32_t& buttons) -> bool {
            m64p_netplay_new_input value;
            if (!cb->get_input(cb->context, frame, port, &value))
            {
                std::printf("  [%s] get_input failed at frame %u\n", m.label, frame);
                m.ok = false;
                return false;
            }
            buttons = value.buttons;
            if (value.pak >= 0)
            {
                m.paks.push_back({int(frame), port, value.pak});
            }
            return true;
        };

        if (reads)
        {
            for (int port = 0; port < 4; port++)
            {
                uint32_t value = EmptyPort;
                if (m.ports[port].present && !read(port, value))
                {
                    return;
                }
                m.seen[frame][port] = value;
            }
        }
        else
        {
            for (int port = 0; port < 4; port++)
            {
                uint32_t ignored;
                if (m.ports[port].present && !read(port, ignored))
                {
                    return;
                }
            }
        }

        // the messages put into this frame, after its input (like the core)
        m.delivered[frame].clear();
        for (unsigned index = 0;; index++)
        {
            unsigned size = 0;
            const void* data = cb->get_message(cb->context, frame, index, &size);
            if (data == nullptr)
            {
                break;
            }
            std::string message(static_cast<const char*>(data), size);
            m.delivered[frame].push_back(message);
            for (char c : message)
            {
                state = (state ^ uint8_t(c)) * 0x100000001b3ull;
            }
        }

        for (int port = 0; port < 4; port++)
        {
            state = (state ^ m.seen[frame][port]) * 0x100000001b3ull;
        }
        if (m.desyncAt != 0 && frame == m.desyncAt)
        {
            state ^= 1;
        }
        cb->frame_hash(cb->context, frame, state, frame);

        int r = cb->frame_end(cb->context, frame, m.busyUs, 1000000 / 60, rerun ? M64P_NETPLAY_NEW_RERUN : 0);
        if (r < 0)
        {
            std::printf("  [%s] frame_end says the session ended at %u\n", m.label, frame);
            m.ok = false;
            return;
        }
        if (!rerun)
        {
            m.caughtUp += (r & M64P_NETPLAY_NEW_SKIP_LIMITER) ? 1 : 0;
            m.slowDowns += (r & M64P_NETPLAY_NEW_SLOW_DOWN) ? 1 : 0;
            m.framesRun = frame + 1;

            SessionInfo info = m.session->GetInfo();
            auto it = info.inputBy.find(HostPlayer);
            if (it != info.inputBy.end())
            {
                m.authoritiesSeen.insert(it->second);
                if (lastAuthority != NoPlayer && it->second != lastAuthority)
                {
                    m.authorityChanges++;
                }
                lastAuthority = it->second;
            }
        }

        frame++;
        if (boundary(frame))
        {
            continue;
        }

        // a frame takes about 16 ms, with jitter (unless catching up or re-run)
        if (!rerun && !(r & M64P_NETPLAY_NEW_SKIP_LIMITER))
        {
            std::this_thread::sleep_for(std::chrono::microseconds(14000 + rng() % 5000 + ((r & M64P_NETPLAY_NEW_SLOW_DOWN) ? 1000 : 0)));
        }
    }
    m.finalFrames = confirmed;
}

struct Scenario
{
    const char* name;
    uint16_t port;
    std::string internalName;
    Mode mode = Mode::Lockstep;
    bool playerIsAuthority = false;
    uint8_t hostLag = 2;
    uint8_t playerLag = 3;
    // leave the lag to the host's automatic lag instead of fixing it
    bool autoLag = false;
    // how long the player's frames take to emulate
    unsigned playerBusyUs = 3000;
    InputFunction inputOf;
    unsigned playerLeavesAt = 0;
    unsigned spectatorDesyncsAt = 0;
    // RMG_NETPLAY_NEW_DELAY_MS for this scenario (0: none)
    unsigned delayMs = 0;
    // the execution plugin every machine has (none: empty)
    std::string hostExecutionPlugin;
    // the host sends a message every frame for 30 frames from this one (0: it doesn't)
    unsigned hostMessageBurstAt = 0;
    // everyone sends the messages of large_messages_of too
    bool largeMessages = false;
    // RMG_NETPLAY_NEW_DROP_PERCENT for this scenario (-1: as the test was started)
    int dropPercent = -1;
    // runs on the test thread once the game is going
    std::function<void(Machine& host, Machine& player, Machine& spectator)> during;
    // extra checks
    std::function<bool(Machine& host, Machine& player, Machine& spectator)> check;
};

static uint32_t default_input(uint8_t player, unsigned frame)
{
    return (uint32_t(player) << 24) ^ (frame * 2654435761u);
}

// input that changes every few frames, like a player's: a guess (the last
// input) is right most of the time and wrong whenever it changes
static uint32_t changing_input(uint8_t player, unsigned frame)
{
    return (uint32_t(player) << 24) ^ ((frame / 6) * 2654435761u);
}

static bool run(const Scenario& scenario)
{
    std::printf("=== %s\n", scenario.name);

    Machine host{"host"}, player{"player"}, spectator{"spectator"};

    // the transports read the test delay when they start
    if (scenario.delayMs != 0)
    {
        set_env("RMG_NETPLAY_NEW_DELAY_MS", std::to_string(scenario.delayMs).c_str());
    }
    if (scenario.dropPercent >= 0)
    {
        set_env("RMG_NETPLAY_NEW_DROP_PERCENT", std::to_string(scenario.dropPercent).c_str());
    }
    auto make = [](Machine& m) {
        m.session = std::make_unique<Session>([&m](const Event& event) {
            switch (event.type)
            {
            case EventType::GameStarting:
                m.starting = true;
                break;
            case EventType::Ended:
                m.ended = true;
                break;
            case EventType::Paused:
                m.pausedEvents++;
                break;
            case EventType::Desync:
                m.desyncEvents++;
                std::printf("  [%s] desync: %s\n", m.label, event.text.c_str());
                break;
            case EventType::Message:
            case EventType::GameEnded:
                std::printf("  [%s] %s\n", m.label, event.text.c_str());
                break;
            default:
                break;
            }
        });
    };
    make(host);
    make(player);
    make(spectator);

    std::string error;
    LocalInfo info{"Host", "v1", "rsp", "gfx", scenario.hostExecutionPlugin, ""};
    if (!host.session->Host(scenario.port, info, "Test Game", "ABC", scenario.internalName, error))
    {
        std::printf("  host failed: %s\n", error.c_str());
        return false;
    }
    info.name = "Player";
    info.executionPlugin = scenario.hostExecutionPlugin;
    if (!player.session->Join(g_address, scenario.port, info, error))
    {
        std::printf("  join failed: %s\n", error.c_str());
        return false;
    }
    std::this_thread::sleep_for(50ms); // the player joins first, so it gets port 3 below either way
    info.name = "Watcher";
    if (!spectator.session->Join(g_address, scenario.port, info, error))
    {
        std::printf("  join failed: %s\n", error.c_str());
        return false;
    }

    for (int i = 0; i < 300 && host.session->GetInfo().players.size() < 3; i++)
    {
        std::this_thread::sleep_for(10ms);
    }
    if (host.session->GetInfo().players.size() != 3)
    {
        std::printf("  not everyone joined\n");
        return false;
    }

    host.id      = HostPlayer;
    player.id    = player.session->GetInfo().myId;
    spectator.id = spectator.session->GetInfo().myId;
    player.leaveAt = scenario.playerLeavesAt;
    host.messageBurstAt = scenario.hostMessageBurstAt;
    for (Machine* m : {&host, &player, &spectator})
    {
        m->largeMessages = scenario.largeMessages;
    }
    player.busyUs = scenario.playerBusyUs;
    spectator.desyncAt = scenario.spectatorDesyncsAt;

    player.session->SetHasGame(true, info);
    spectator.session->SetHasGame(true, info);

    host.session->SetPorts(HostPlayer, 0x1);
    host.session->SetPorts(player.id, 0x4); // port 3
    host.session->SetPorts(spectator.id, 0x0);
    if (!scenario.autoLag)
    {
        host.session->SetLag(HostPlayer, scenario.hostLag);
        host.session->SetLag(player.id, scenario.playerLag);
    }
    else
    {
        // the host sets the lag about once a second, from what it measures
        std::this_thread::sleep_for(1500ms);
    }
    if (scenario.internalName.empty() || scenario.mode != Mode::Golf)
    {
        set_mode(*host.session, scenario.mode, scenario.playerIsAuthority ? player.id : HostPlayer);
    }

    std::string reason;
    for (int i = 0; i < 300 && !host.session->CanStart(reason); i++)
    {
        std::this_thread::sleep_for(10ms);
    }
    if (!host.session->CanStart(reason))
    {
        std::printf("  can't start: %s\n", reason.c_str());
        return false;
    }
    std::this_thread::sleep_for(100ms); // let the guests see the final state

    if (host.session->GetInfo().mode != scenario.mode)
    {
        std::printf("  FAIL: the session is in mode %d, not %d\n", int(host.session->GetInfo().mode), int(scenario.mode));
        return false;
    }

    if (!host.session->Start({1, 2, 3}, error))
    {
        std::printf("  start failed: %s\n", error.c_str());
        return false;
    }

    for (int i = 0; i < 300 && !(player.starting && spectator.starting); i++)
    {
        std::this_thread::sleep_for(5ms);
    }

    InputFunction inputOf = scenario.inputOf ? scenario.inputOf : InputFunction(default_input);
    host.emu = std::thread(emulate, std::ref(host), true, inputOf);
    std::this_thread::sleep_for(40ms);
    player.emu = std::thread(emulate, std::ref(player), false, inputOf);
    std::this_thread::sleep_for(150ms); // the spectator starts late
    spectator.emu = std::thread(emulate, std::ref(spectator), false, inputOf);

    if (scenario.during)
    {
        scenario.during(host, player, spectator);
    }

    host.emu.join();
    player.emu.join();
    spectator.emu.join();

    set_env("RMG_NETPLAY_NEW_DELAY_MS", nullptr);
    set_env("RMG_NETPLAY_NEW_DROP_PERCENT", g_dropPercent.empty() ? nullptr : g_dropPercent.c_str());

    bool ok = host.ok && spectator.ok && (scenario.playerLeavesAt != 0 || player.ok);

    // every machine saw the same input on every frame it ran (in the end:
    // after correcting its wrong guesses)
    unsigned playerFrames = scenario.playerLeavesAt != 0 ? player.finalFrames : player.framesRun;
    for (unsigned f = 0; ok && f < Frames; f++)
    {
        for (int p = 0; p < 4; p++)
        {
            uint32_t h = host.seen[f][p], s = spectator.seen[f][p];
            uint32_t g = f < playerFrames ? player.seen[f][p] : h;
            if (h != g || h != s)
            {
                std::printf("  MISMATCH frame %u port %d: host %08x player %08x spectator %08x\n", f, p, h, g, s);
                ok = false;
                break;
            }
        }
    }

    // every pak switch reached every machine exactly once, on the same frame
    {
        auto expect = [&](const std::vector<std::array<int, 3>>& all, const char* label, unsigned framesRun) {
            std::vector<std::array<int, 3>> wanted, paks;
            for (const auto& p : host.paks)
            {
                if (unsigned(p[0]) < framesRun)
                {
                    wanted.push_back(p);
                }
            }
            for (const auto& p : all)
            {
                if (unsigned(p[0]) < framesRun)
                {
                    paks.push_back(p);
                }
            }
            if (paks != wanted)
            {
                std::printf("  PAK SWITCH MISMATCH on %s (%zu vs the host's %zu)\n", label, paks.size(), wanted.size());
                return false;
            }
            return true;
        };
        ok = expect(spectator.paks, "spectator", Frames) && ok;
        ok = expect(player.paks, "player", playerFrames) && ok;

        // the host saw the player's three (port 3), in order, and its own (port 1)
        std::vector<int> playerPaks, hostPaks;
        for (const auto& p : host.paks)
        {
            (p[1] == 2 ? playerPaks : hostPaks).push_back(p[2]);
        }
        if (playerPaks != std::vector<int>{PLUGIN_MEMPAK, PLUGIN_NONE, PLUGIN_RUMBLE_PAK} ||
            hostPaks != std::vector<int>{PLUGIN_RUMBLE_PAK})
        {
            std::printf("  FAIL: pak switches lost or repeated (player's %zu, host's %zu)\n", playerPaks.size(), hostPaks.size());
            ok = false;
        }
        std::printf("  pak switches at frames:");
        for (const auto& p : host.paks)
        {
            std::printf(" %d(port %d)", p[0], p[1] + 1);
        }
        std::printf("\n");
    }

    // every machine got the same messages on the same frames; every message
    // the host's or the spectator's plugin sent arrived exactly once (and the
    // player's, unless it left), and the host's no earlier than it was sent
    {
        bool same = true;
        for (unsigned f = 0; same && f < Frames; f++)
        {
            bool playerSame = f >= playerFrames || player.delivered[f] == host.delivered[f];
            if (!playerSame || spectator.delivered[f] != host.delivered[f])
            {
                std::printf("  MESSAGE MISMATCH at frame %u: host %zu, player %zu, spectator %zu\n", f,
                            host.delivered[f].size(), f < playerFrames ? player.delivered[f].size() : 0,
                            spectator.delivered[f].size());
                same = false;
            }
        }
        std::map<std::string, std::vector<unsigned>> at;
        for (unsigned f = 0; f < Frames; f++)
        {
            for (const std::string& message : host.delivered[f])
            {
                at[message].push_back(f);
            }
        }
        unsigned delivered = 0, lost = 0, repeated = 0, early = 0, delay = 0;
        for (Machine* m : {&host, &player, &spectator})
        {
            for (const std::string& message : m->sent)
            {
                auto it = at.find(message);
                unsigned sentAt = unsigned(std::stoul(message.substr(m == &host ? 1 : message.find(':') + 1)));
                if (it == at.end())
                {
                    lost += (m == &player && scenario.playerLeavesAt != 0) ? 0 : 1;
                    continue;
                }
                delivered++;
                repeated += it->second.size() > 1 ? 1 : 0;
                // (a guest's can land on an earlier frame: in Rollback it runs ahead of the host, which writes it)
                early += (m == &host && it->second[0] < sentAt) ? 1 : 0;
                delay = std::max(delay, it->second[0] - std::min(it->second[0], sentAt));
            }
        }
        std::printf("  messages: %u delivered (at most %u frames after they were sent), %u lost, %u repeated, %u early\n",
                    delivered, delay, lost, repeated, early);
        // a message never makes a guess wrong (they apply MessageDelayFrames late)
        uint64_t misses = 0;
        for (Machine* m : {&host, &player, &spectator})
        {
            if (m->session != nullptr)
            {
                misses += m->session->GetInfo().rollback.messageMisses;
            }
        }
        if (!same || lost != 0 || repeated != 0 || early != 0 || delivered == 0 || misses != 0)
        {
            std::printf("  FAIL: messages (%llu guesses wrong only about messages)\n", (unsigned long long)misses);
            ok = false;
        }

        // the host is the speaker
        if (!host.speaker || player.speaker || spectator.speaker)
        {
            std::printf("  FAIL: the speaker isn't the host alone\n");
            ok = false;
        }
    }

    // no Frames message got bigger than the session allows (bigger ones would
    // be split, and wait for all their parts)
    for (Machine* m : {&host, &player, &spectator})
    {
        if (m->session != nullptr && m->session->GetInfo().largestFrames > MaxFramesBytes)
        {
            std::printf("  FAIL: %s sent a Frames message of %zu bytes\n", m->label, m->session->GetInfo().largestFrames);
            ok = false;
        }
    }

    // synced data
    for (Machine* m : {&player, &spectator})
    {
        if (m->syncedSram != host.syncedSram || m->clock != host.clock)
        {
            std::printf("  SYNC MISMATCH on %s\n", m->label);
            ok = false;
        }
    }

    if (ok && scenario.check)
    {
        ok = scenario.check(host, player, spectator);
    }

    // the host compared everyone's game with its own
    {
        SessionInfo info = host.session->GetInfo();
        if (scenario.spectatorDesyncsAt == 0 && !info.desyncs.empty())
        {
            std::printf("  FAIL: the host reported a desync that didn't happen\n");
            ok = false;
        }
        if (scenario.spectatorDesyncsAt != 0)
        {
            auto it = info.desyncs.find(spectator.id);
            if (it == info.desyncs.end() || it->second != scenario.spectatorDesyncsAt || info.desyncs.size() != 1 ||
                player.desyncEvents == 0 || spectator.desyncEvents == 0)
            {
                std::printf("  FAIL: the spectator's desync at frame %u wasn't reported right\n", scenario.spectatorDesyncsAt);
                ok = false;
            }
        }
    }

    std::printf("  catch-up frames: host %d player %d spectator %d; authority changes seen by the host: %d\n",
                host.caughtUp, player.caughtUp, spectator.caughtUp, host.authorityChanges);
    for (Machine* m : {&host, &player, &spectator})
    {
        if (m->session == nullptr)
        {
            continue;
        }
        RollbackStats stats = m->session->GetInfo().rollback;
        if (stats.guessedFrames != 0 || m->rollbacks != 0)
        {
            std::printf("  [%s] guessed %llu, rolled back %u times (%llu frames re-run, deepest %u), slowed down %u times\n",
                        m->label, (unsigned long long)stats.guessedFrames, m->rollbacks,
                        (unsigned long long)stats.framesRerun, m->deepest, m->slowDowns);
        }
    }

    // a game ending on the host ends it for everyone, but the session stays
    if (scenario.playerLeavesAt == 0)
    {
        host.session->GameStopped();
        std::this_thread::sleep_for(200ms);
        if (player.session->GetInfo().playing || spectator.session->GetInfo().playing)
        {
            std::printf("  FAIL: the game still runs on a guest after the host stopped it\n");
            ok = false;
        }
    }

    // the host leaving ends the session for the guests
    host.session->Leave();
    for (int i = 0; i < 300 && !(spectator.ended && (player.ended || scenario.playerLeavesAt != 0)); i++)
    {
        std::this_thread::sleep_for(10ms);
    }
    if (!spectator.ended)
    {
        std::printf("  FAIL: the spectator didn't notice the host leaving\n");
        ok = false;
    }

    player.session.reset();
    spectator.session.reset();
    host.session.reset();

    std::printf(ok ? "  PASS\n" : "  FAIL\n");
    return ok;
}

int main()
{
    // progress shows up as it happens, also when the output goes to a file
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (const char* drop = std::getenv("RMG_NETPLAY_NEW_DROP_PERCENT"))
    {
        g_dropPercent = drop;
    }
    if (const char* address = std::getenv("RMG_NETPLAY_NEW_TEST_ADDRESS"))
    {
        g_address = address;
    }
    std::printf("joining %s\n", g_address.c_str());
    for (const LocalAddress& local : Transport::GetLocalAddresses())
    {
        std::printf("local address: %s%s\n", local.WithPort(DefaultPort).c_str(), local.global ? " (global)" : "");
    }

    std::vector<Scenario> scenarios;

    // Lockstep: every player writes their own input `lag` frames ahead.
    // The host pauses the game for a moment in the middle.
    {
        Scenario s;
        s.name = "Lockstep, with a pause and the spectator desyncing at frame 500";
        s.port = 47123;
        s.spectatorDesyncsAt = 500;
        s.during = [](Machine& host, Machine&, Machine&) {
            std::this_thread::sleep_for(5s);
            host.session->SetPaused(true);
            std::this_thread::sleep_for(700ms);
            host.session->SetPaused(false);
        };
        s.check = [](Machine& host, Machine& player, Machine& spectator) {
            auto expected = [](uint8_t id, unsigned lag, unsigned f) { return default_input(id, f >= lag ? f - lag : 0); };
            for (unsigned f = 0; f < Frames; f++)
            {
                if (host.seen[f][0] == NotRead)
                {
                    continue;
                }
                if (host.seen[f][0] != expected(HostPlayer, 2, f) || host.seen[f][2] != expected(player.id, 3, f))
                {
                    std::printf("  WRONG INPUT frame %u\n", f);
                    return false;
                }
                if (host.seen[f][1] != EmptyPort || host.seen[f][3] != EmptyPort)
                {
                    std::printf("  EMPTY PORT READ frame %u\n", f);
                    return false;
                }
            }
            if (player.pausedEvents == 0 || spectator.pausedEvents == 0)
            {
                std::printf("  FAIL: not everyone heard about the pause\n");
                return false;
            }
            return true;
        };
        scenarios.push_back(s);
    }

    // Golf (Mario Golf turns it on): the host and the player take turns
    // "shooting" (pressing A); in between they only aim with the D-pad and
    // look around with the C buttons, which Mario Golf's mask ignores.
    {
        Scenario s;
        s.name = "Golf, taking turns";
        s.port = 47124;
        s.internalName = "MarioGolf64";
        s.mode = Mode::Golf;
        s.inputOf = [](uint8_t player, unsigned frame) -> uint32_t {
            unsigned turn = frame / 120;
            bool myTurn = (player == HostPlayer) == (turn % 2 == 0);
            if (myTurn && frame % 120 >= 20 && frame % 120 < 40)
            {
                return 0x80; // A: a shot
            }
            return (frame % 17 == 0) ? 0x0101 : 0; // D-pad right + C right: aiming, ignored
        };
        s.check = [](Machine& host, Machine& player, Machine&) {
            if (host.authorityChanges < 4 || host.authoritiesSeen.count(player.id) == 0)
            {
                std::printf("  FAIL: the authority didn't change hands enough (%d changes)\n", host.authorityChanges);
                return false;
            }
            // every shot reached the game
            for (unsigned turn = 0; turn < Frames / 120; turn++)
            {
                int port = (turn % 2 == 0) ? 0 : 2;
                bool shot = false;
                for (unsigned f = turn * 120; f < std::min(Frames, turn * 120 + 120); f++)
                {
                    shot = shot || (host.seen[f][port] != NotRead && (host.seen[f][port] & 0x80));
                }
                if (!shot)
                {
                    std::printf("  FAIL: the shot in turn %u never reached the game\n", turn);
                    return false;
                }
            }
            return true;
        };
        scenarios.push_back(s);
    }

    // Authority: the player writes everyone's input, then the host switches
    // to Lockstep in the middle of the game.
    {
        Scenario s;
        s.name = "Authority (the player), then Lockstep";
        s.port = 47125;
        s.mode = Mode::Authority;
        s.playerIsAuthority = true;
        s.during = [](Machine& host, Machine&, Machine&) {
            std::this_thread::sleep_for(6s);
            set_mode(*host.session, Mode::Lockstep);
        };
        s.check = [](Machine& host, Machine& player, Machine&) {
            if (host.authoritiesSeen.count(player.id) == 0 || host.authoritiesSeen.count(HostPlayer) == 0)
            {
                std::printf("  FAIL: the host's input wasn't written by the player and then the host\n");
                return false;
            }
            return true;
        };
        scenarios.push_back(s);
    }

    // A player leaves in the middle: the host takes over their input and
    // the game goes on with their controller idle.
    {
        Scenario s;
        s.name = "A player leaves mid-game";
        s.port = 47126;
        s.playerLeavesAt = 400;
        s.check = [](Machine& host, Machine& player, Machine&) {
            if (host.framesRun != Frames)
            {
                std::printf("  FAIL: the host stopped at frame %u\n", host.framesRun);
                return false;
            }
            // near the end, the player's port is idle
            for (unsigned f = Frames - 100; f < Frames; f++)
            {
                if (host.seen[f][2] != NotRead && host.seen[f][2] != 0)
                {
                    std::printf("  FAIL: the player's port isn't idle at frame %u (%08x)\n", f, host.seen[f][2]);
                    return false;
                }
            }
            return true;
        };
        scenarios.push_back(s);
    }

    // The player writes everyone's input (the host's too, so the messages the
    // host sends go into frames the player writes), then leaves: the host
    // takes over, and the messages it sent them that no frame had yet go into
    // its own frames (the message check below covers that none get lost)
    {
        Scenario s;
        s.name = "Authority (the player), who leaves mid-game";
        s.port = 47135;
        s.mode = Mode::Authority;
        s.playerIsAuthority = true;
        s.playerLeavesAt = 400;
        s.hostMessageBurstAt = 385;
        s.check = [](Machine& host, Machine& player, Machine&) {
            if (host.authoritiesSeen.count(player.id) == 0 || host.authoritiesSeen.count(HostPlayer) == 0 ||
                host.framesRun != Frames)
            {
                std::printf("  FAIL: the player didn't write the host's input and then the host\n");
                return false;
            }
            std::printf("  the host sent %u messages to the stream's authority again\n", host.session->GetInfo().messagesResent);
            return true;
        };
        scenarios.push_back(s);
    }

    // Lockstep with the automatic lag: on loopback the delay fits in the
    // spare time of every frame, so nobody needs any lag.
    {
        Scenario s;
        s.name = "Lockstep with the automatic lag";
        s.port = 47127;
        s.autoLag = true;
        s.check = [](Machine& host, Machine& player, Machine&) {
            SessionInfo info = host.session->GetInfo();
            uint8_t hostLag = info.Find(HostPlayer)->lag, playerLag = info.Find(player.id)->lag;
            std::printf("  automatic lag: host %u, player %u\n", hostLag, playerLag);
            if (hostLag != 0 || playerLag != 0)
            {
                std::printf("  FAIL: on loopback the automatic lag should be 0\n");
                return false;
            }
            return true;
        };
        scenarios.push_back(s);
    }

    // The same, but the player's machine is slow: its frames take nearly a
    // whole frame to emulate, so it has no spare time to wait in, and the
    // host's frames (which it receives) need a frame of lag. The player's
    // frames still don't: the host has the spare time.
    {
        Scenario s;
        s.name = "Lockstep with the automatic lag and a slow player";
        s.port = 47128;
        s.autoLag = true;
        s.playerBusyUs = 16500;
        s.check = [](Machine& host, Machine& player, Machine&) {
            SessionInfo info = host.session->GetInfo();
            uint8_t hostLag = info.Find(HostPlayer)->lag, playerLag = info.Find(player.id)->lag;
            std::printf("  automatic lag: host %u, player %u (player's ping %u ms)\n", hostLag, playerLag,
                        info.Find(player.id)->ping);
            if (hostLag < 1 || playerLag != 0)
            {
                std::printf("  FAIL: the host's lag should cover the slow player, and the player's should be 0\n");
                return false;
            }
            return true;
        };
        scenarios.push_back(s);
    }

    // what every Rollback scenario checks: the players guessed, guessed
    // wrong and went back, never further than the window allows; the
    // spectator never guessed
    auto rolledBack = [](Machine& host, Machine& player, Machine& spectator) {
        for (Machine* m : {&host, &player})
        {
            if (m->session == nullptr)
            {
                continue;
            }
            RollbackStats stats = m->session->GetInfo().rollback;
            if (stats.guessedFrames == 0 || m->rollbacks == 0)
            {
                std::printf("  FAIL: %s didn't guess and roll back (%llu guesses, %u rollbacks)\n", m->label,
                            (unsigned long long)stats.guessedFrames, m->rollbacks);
                return false;
            }
            // (one more than the window: a frame may have no snapshot)
            if (m->deepest > RollbackWindow + 2)
            {
                std::printf("  FAIL: %s rolled back %u frames, past the window\n", m->label, m->deepest);
                return false;
            }
        }
        if (spectator.session->GetInfo().rollback.guessedFrames != 0 || spectator.rollbacks != 0)
        {
            std::printf("  FAIL: the spectator guessed\n");
            return false;
        }
        return true;
    };

    // Rollback at lag 0: on loopback a frame often isn't there yet when
    // it's needed, so it's guessed, and input that changes every few frames
    // makes some guesses wrong. The host pauses in the middle.
    {
        Scenario s;
        s.name = "Rollback at lag 0, with a pause";
        s.port = 47129;
        s.mode = Mode::Rollback;
        s.hostLag = 0;
        s.playerLag = 0;
        s.inputOf = changing_input;
        s.during = [](Machine& host, Machine&, Machine&) {
            std::this_thread::sleep_for(5s);
            host.session->SetPaused(true);
            std::this_thread::sleep_for(700ms);
            host.session->SetPaused(false);
        };
        s.check = [rolledBack](Machine& host, Machine& player, Machine& spectator) {
            if (player.pausedEvents == 0 || spectator.pausedEvents == 0)
            {
                std::printf("  FAIL: not everyone heard about the pause\n");
                return false;
            }
            return rolledBack(host, player, spectator);
        };
        scenarios.push_back(s);
    }

    // Rollback over a slower network (25 ms each way) with the automatic
    // lag: the lag covers what guessing doesn't (RollbackHide frames)
    {
        Scenario s;
        s.name = "Rollback with 25 ms of delay each way and the automatic lag";
        s.port = 47130;
        s.mode = Mode::Rollback;
        s.autoLag = true;
        s.delayMs = 25;
        s.inputOf = changing_input;
        s.check = [rolledBack](Machine& host, Machine& player, Machine& spectator) {
            SessionInfo info = host.session->GetInfo();
            uint8_t hostLag = info.Find(HostPlayer)->lag, playerLag = info.Find(player.id)->lag;
            std::printf("  automatic lag: host %u, player %u (player's ping %u ms)\n", hostLag, playerLag,
                        info.Find(player.id)->ping);
            if (hostLag > 2 || playerLag > 2)
            {
                std::printf("  FAIL: Rollback should need less lag here\n");
                return false;
            }
            return rolledBack(host, player, spectator);
        };
        scenarios.push_back(s);
    }

    // Rollback when a player leaves: the host writes their input from then
    // on (idle), which everyone guessed differently
    {
        Scenario s;
        s.name = "Rollback, a player leaves mid-game";
        s.port = 47131;
        s.mode = Mode::Rollback;
        s.hostLag = 0;
        s.playerLag = 0;
        s.inputOf = changing_input;
        s.playerLeavesAt = 400;
        s.check = [rolledBack](Machine& host, Machine& player, Machine& spectator) {
            if (host.framesRun != Frames)
            {
                std::printf("  FAIL: the host stopped at frame %u\n", host.framesRun);
                return false;
            }
            for (unsigned f = Frames - 100; f < Frames; f++)
            {
                if (host.seen[f][2] != NotRead && host.seen[f][2] != 0)
                {
                    std::printf("  FAIL: the player's port isn't idle at frame %u (%08x)\n", f, host.seen[f][2]);
                    return false;
                }
            }
            return rolledBack(host, player, spectator);
        };
        scenarios.push_back(s);
    }

    // Switching modes mid-game: Rollback, Lockstep, Authority, Rollback
    {
        Scenario s;
        s.name = "Rollback, then Lockstep, Authority and Rollback again";
        s.port = 47132;
        s.mode = Mode::Rollback;
        s.hostLag = 0;
        s.playerLag = 0;
        s.inputOf = changing_input;
        s.during = [](Machine& host, Machine& player, Machine&) {
            std::this_thread::sleep_for(3s);
            set_mode(*host.session, Mode::Lockstep);
            std::this_thread::sleep_for(2s);
            set_mode(*host.session, Mode::Authority, player.id);
            std::this_thread::sleep_for(2s);
            set_mode(*host.session, Mode::Rollback);
        };
        s.check = rolledBack;
        scenarios.push_back(s);
    }

    // Big messages (up to the most a plugin may send), many small ones on
    // one frame, and a guest's: the player writes the host's frames, so the
    // frames that carry the host's messages go from the player through the
    // host to the spectator, with a fifth of the input packets lost. Each
    // message's bytes go once, on the reliable channel; the frames carry
    // only the numbers, and nobody uses a frame before it has the bytes. The
    // message check below covers that every message arrives exactly once, on
    // the same frame everywhere.
    {
        Scenario s;
        s.name = "Large messages, relayed, with 20% of input packets lost";
        s.port = 47136;
        s.mode = Mode::Authority;
        s.playerIsAuthority = true;
        s.largeMessages = true;
        s.dropPercent = 20;
        s.check = [](Machine& host, Machine&, Machine&) {
            size_t largest = 0;
            unsigned onOneFrame = 0;
            for (unsigned f = 0; f < Frames; f++)
            {
                unsigned many = 0;
                for (const std::string& message : host.delivered[f])
                {
                    largest = std::max(largest, message.size());
                    many += message.rfind("h300:many", 0) == 0 ? 1 : 0;
                }
                onOneFrame = std::max(onOneFrame, many);
            }
            std::printf("  largest message %zu bytes; at most %u of the 40 small ones on one frame\n", largest, onOneFrame);
            if (largest != M64P_NETPLAY_NEW_MAX_MESSAGE || onOneFrame == 0 || onOneFrame >= 40)
            {
                std::printf("  FAIL: the big message, or the small ones spread over frames, didn't arrive\n");
                return false;
            }
            return true;
        };
        scenarios.push_back(s);
    }

    bool ok = true;

    // Rollback isn't available with an execution plugin that can't save its state
    {
        std::printf("=== Rollback refused with an execution plugin that can't save its state\n");
        Scenario s;
        s.port = 47133;
        Session host([](const Event&) {});
        LocalInfo info{"Host", "v1", "rsp", "gfx", "Golf Plugin", "execution plugin can't save its state for rollback"};
        std::string error;
        bool refused = host.Host(s.port, info, "Test Game", "ABC", "", error) &&
                       !host.SetMode(Mode::Rollback, HostPlayer, error) && !error.empty() &&
                       host.GetInfo().mode == Mode::Lockstep;
        std::printf("  %s\n  %s\n", error.c_str(), refused ? "PASS" : "FAIL");
        ok = refused && ok;
        host.Leave();
    }
    // and is with one that saves it
    {
        std::printf("=== Rollback allowed with an execution plugin that saves its state\n");
        Session host([](const Event&) {});
        LocalInfo info{"Host", "v1", "rsp", "gfx", "Party Plugin", ""};
        std::string error;
        bool allowed = host.Host(47134, info, "Test Game", "ABC", "", error) &&
                       host.SetMode(Mode::Rollback, HostPlayer, error) && host.GetInfo().mode == Mode::Rollback;
        std::printf("  %s\n", allowed ? "PASS" : ("FAIL: " + error).c_str());
        ok = allowed && ok;
        host.Leave();
    }

    // A spectator can't be the authority: closing their game would leave
    // everyone waiting for the frames they write
    {
        std::printf("=== A spectator can't be the authority\n");
        Session host([](const Event&) {});
        Session watcher([](const Event&) {});
        LocalInfo info{"Host", "v1", "rsp", "gfx", "", ""};
        std::string error;
        bool refused = host.Host(47137, info, "Test Game", "ABC", "", error);
        info.name = "Watcher";
        refused = refused && watcher.Join(g_address, 47137, info, error);
        for (int i = 0; refused && i < 300 && (host.GetInfo().players.size() < 2 || watcher.GetInfo().myId == NoPlayer); i++)
        {
            std::this_thread::sleep_for(10ms);
        }
        uint8_t id = watcher.GetInfo().myId;
        host.SetPorts(id, 0x0);
        refused = refused && host.GetInfo().players.size() == 2 && !host.SetMode(Mode::Authority, id, error) &&
                  !error.empty() && host.GetInfo().mode != Mode::Authority;
        std::printf("  %s\n", error.c_str());
        // with a port, they can
        host.SetPorts(id, 0x2);
        std::string unused;
        bool allowed = host.SetMode(Mode::Authority, id, unused) && host.GetInfo().authorityPlayer == id;
        // and when the host takes their ports away, the host is the authority again
        host.SetPorts(id, 0x0);
        bool reset = host.GetInfo().authorityPlayer == HostPlayer;
        std::printf("  %s\n", refused && allowed && reset ? "PASS" : "FAIL");
        ok = refused && allowed && reset && ok;
        watcher.Leave();
        host.Leave();
    }

    // RMG_NETPLAY_NEW_TEST_ONLY=text runs only the scenarios whose names contain it
    const char* only = std::getenv("RMG_NETPLAY_NEW_TEST_ONLY");
    for (const Scenario& scenario : scenarios)
    {
        if (only == nullptr || std::strstr(scenario.name, only) != nullptr)
        {
            ok = run(scenario) && ok;
        }
    }

    std::printf(ok ? "ALL PASS\n" : "SOME FAILED\n");
    return ok ? 0 : 1;
}
