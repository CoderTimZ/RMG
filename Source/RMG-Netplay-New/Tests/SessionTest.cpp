// Loopback tests for RMG-Netplay-New's Session: a host, a guest player and a
// guest spectator in one process, each driving the core callbacks the way
// mupen64plus-core does, with jitter. Every scenario checks that all
// machines see exactly the same input on every port at every frame (the
// game would desync otherwise), plus what that scenario is about.
//
//   RMG_NETPLAY_NEW_TEST_ADDRESS  address to join (default 127.0.0.1; try ::1 or localhost)
//   RMG_NETPLAY_NEW_DROP_PERCENT  drop this many percent of input packets
#include <RMG-Netplay-New/Session.hpp>
#include <RMG-Core/m64p/api/m64p_plugin.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

static std::string g_address = "127.0.0.1";

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
};

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
    uint8_t lastAuthority = NoPlayer;
    // stands in for the emulated machine's state: a running hash of all the input it saw
    uint64_t state = 0xcbf29ce484222325ull;

    for (unsigned frame = 0; frame < Frames; frame++)
    {
        if (m.leaveAt != 0 && frame == m.leaveAt)
        {
            m.session->Leave();
            return;
        }

        // the game reads the controllers on most frames; sampling happens on the first read
        bool reads = (frame % 7) != 3;
        m64p_netplay_new_input localInput[4] = {{inputOf(m.id, frame), pak_switch_of(isHost, frame)},
                                                {0, -1}, {0, -1}, {0, -1}};
        cb->sample(cb->context, frame, localInput);

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

        for (int port = 0; port < 4; port++)
        {
            state = (state ^ m.seen[frame][port]) * 0x100000001b3ull;
        }
        if (m.desyncAt != 0 && frame == m.desyncAt)
        {
            state ^= 1;
        }
        cb->frame_hash(cb->context, frame, state);

        int r = cb->frame_end(cb->context, frame, m.busyUs, 1000000 / 60);
        if (r < 0)
        {
            std::printf("  [%s] frame_end says the session ended at %u\n", m.label, frame);
            m.ok = false;
            return;
        }
        m.caughtUp += r;
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

        // a frame takes about 16 ms, with jitter (unless catching up)
        if (r == 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(14000 + rng() % 5000));
        }
    }
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
    // runs on the test thread once the game is going
    std::function<void(Machine& host, Machine& player, Machine& spectator)> during;
    // extra checks
    std::function<bool(Machine& host, Machine& player, Machine& spectator)> check;
};

static uint32_t default_input(uint8_t player, unsigned frame)
{
    return (uint32_t(player) << 24) ^ (frame * 2654435761u);
}

static bool run(const Scenario& scenario)
{
    std::printf("=== %s\n", scenario.name);

    Machine host{"host"}, player{"player"}, spectator{"spectator"};
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
    LocalInfo info{"Host", "v1", "rsp", "gfx", ""};
    if (!host.session->Host(scenario.port, info, "Test Game", "ABC", scenario.internalName, error))
    {
        std::printf("  host failed: %s\n", error.c_str());
        return false;
    }
    info.name = "Player";
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
        host.session->SetMode(scenario.mode, scenario.playerIsAuthority ? player.id : HostPlayer);
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

    bool ok = host.ok && spectator.ok && (scenario.playerLeavesAt != 0 || player.ok);

    // every machine saw the same input on every frame it ran
    for (unsigned f = 0; ok && f < Frames; f++)
    {
        for (int p = 0; p < 4; p++)
        {
            uint32_t h = host.seen[f][p], s = spectator.seen[f][p];
            uint32_t g = f < player.framesRun ? player.seen[f][p] : h;
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
        auto expect = [&](const std::vector<std::array<int, 3>>& paks, const char* label, unsigned framesRun) {
            std::vector<std::array<int, 3>> wanted;
            for (const auto& p : host.paks)
            {
                if (unsigned(p[0]) < framesRun)
                {
                    wanted.push_back(p);
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
        ok = expect(player.paks, "player", player.framesRun) && ok;

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
            host.session->SetMode(Mode::Lockstep);
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

    bool ok = true;
    for (const Scenario& scenario : scenarios)
    {
        ok = run(scenario) && ok;
    }

    std::printf(ok ? "ALL PASS\n" : "SOME FAILED\n");
    return ok ? 0 : 1;
}
