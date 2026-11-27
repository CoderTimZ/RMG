/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMG_NETPLAY_NEW_TRANSPORT_HPP
#define RMG_NETPLAY_NEW_TRANSPORT_HPP

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;
typedef struct _ENetAddress ENetAddress;

namespace NetplayNew
{
// ENet channels
enum class Channel : uint8_t
{
    // reliable and ordered: session messages and file data
    Control = 0,
    // unreliable and unsequenced: input frames, which carry their own redundancy
    Input = 1,
};

// A connection to another machine
using PeerId = uint32_t;
constexpr PeerId InvalidPeer = 0;

// An address of this machine that others can connect to
struct LocalAddress
{
    std::string address;
    bool ipv6 = false;
    // the address can be reached from the internet (a global IPv6
    // address); otherwise it only works on the local network
    bool global = false;

    // address:port, with brackets around IPv6 addresses
    std::string WithPort(uint16_t port) const
    {
        return (this->ipv6 ? "[" + this->address + "]" : this->address) + ":" + std::to_string(port);
    }
};

// One UDP socket (ENet host) and a thread that receives from it.
// The socket is dual-stack: it talks IPv6 and IPv4 (as IPv4-mapped
// IPv6 addresses) at the same time.
//
// Events are delivered on the transport's thread with no lock held, so
// handlers may call Send(). Send() may be called from any thread and goes
// out immediately.
class Transport
{
  public:
    struct Handlers
    {
        std::function<void(PeerId)> connected;
        std::function<void(PeerId)> disconnected;
        std::function<void(PeerId, Channel, const uint8_t*, size_t)> received;
        // called about every 5 ms, to resend what wasn't acknowledged
        std::function<void(void)> tick;
    };

    Transport(void);
    ~Transport(void);

    // listens on the port, on every IPv6 and IPv4 address;
    // returns false and sets the error on failure
    bool Listen(uint16_t port, int maxPeers, Handlers handlers, std::string& error);

    // connects to a host (a name, an IPv4 or an IPv6 address); the connected or
    // disconnected handler reports the result. A name with several addresses is
    // tried address by address, IPv6 first.
    bool Connect(const std::string& address, uint16_t port, Handlers handlers, std::string& error);

    // stops the thread and closes the socket, after telling peers we left
    void Close(void);

    void Send(PeerId peer, Channel channel, const std::vector<uint8_t>& data);
    void Disconnect(PeerId peer);

    // round trip time in milliseconds, 0 if unknown
    uint32_t GetRoundTripTime(PeerId peer);
    // how much the round trip time varies (ENet's mean deviation), in milliseconds
    uint32_t GetRoundTripTimeVariance(PeerId peer);

    // this machine's addresses that others could connect to: IPv4 addresses
    // on the local network and IPv6 addresses (global ones first)
    static std::vector<LocalAddress> GetLocalAddresses(void);

  private:
    std::recursive_mutex mutex;
    ENetHost* host = nullptr;
    std::vector<ENetPeer*> peers; // index = PeerId - 1
    Handlers handlers;
    std::thread thread;
    std::atomic<bool> running{false};
    // the host's addresses still to try while connecting (guest)
    std::vector<ENetAddress> candidates;
    bool everConnected = false;

    // for testing on one machine (only with NETPLAY_NEW_TESTS):
    // RMG_NETPLAY_NEW_DELAY_MS=N holds every outgoing message back N
    // milliseconds (in order), as a slower network would, and the round trip
    // times include it; RMG_NETPLAY_NEW_DROP_PERCENT=N drops N% of the
    // outgoing input packets. Both are read when the transport starts.
    uint32_t delayMs = 0;
#ifdef NETPLAY_NEW_TESTS
    int dropPercent = 0;
    std::minstd_rand dropRandom;
#endif // NETPLAY_NEW_TESTS
    struct DelayedMessage
    {
        uint64_t due;
        PeerId peer;
        Channel channel;
        std::vector<uint8_t> data;
    };
    std::deque<DelayedMessage> delayed;

    bool Start(ENetHost* host, Handlers handlers);
    void SendNow(PeerId peer, Channel channel, const std::vector<uint8_t>& data);
    void SendDelayed(void);
    void Run(void);
    PeerId GetId(ENetPeer* peer);
    ENetPeer* GetPeer(PeerId id);
    bool ConnectNextCandidate(void);
};
} // namespace NetplayNew

#endif // RMG_NETPLAY_NEW_TRANSPORT_HPP
