/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include <enet.h>

#include "Transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

using namespace NetplayNew;

//
// ENet initialization (shared by every transport)
//

static std::mutex l_EnetMutex;
static int l_EnetUsers = 0;

static bool enet_acquire(void)
{
    std::lock_guard<std::mutex> lock(l_EnetMutex);
    if (l_EnetUsers == 0 && enet_initialize() != 0)
    {
        return false;
    }
    l_EnetUsers++;
    return true;
}

static void enet_release(void)
{
    std::lock_guard<std::mutex> lock(l_EnetMutex);
    if (--l_EnetUsers == 0)
    {
        enet_deinitialize();
    }
}

// how long to try one of the host's addresses before trying the next
static void configure_connecting_peer(ENetPeer* peer)
{
    enet_peer_timeout(peer, 32, 2500, 4000);
}

// IPv4 addresses travel as IPv4-mapped IPv6 addresses (::ffff:a.b.c.d)
static void map_ipv4(const in_addr& ipv4, in6_addr& ipv6)
{
    std::memset(&ipv6, 0, sizeof(ipv6));
    ipv6.s6_addr[10] = 0xFF;
    ipv6.s6_addr[11] = 0xFF;
    std::memcpy(&ipv6.s6_addr[12], &ipv4, 4);
}

// the host's addresses, IPv6 first
static std::vector<ENetAddress> resolve(const std::string& name, uint16_t port)
{
    std::vector<ENetAddress> ipv6, ipv4;

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* result = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &result) != 0)
    {
        return {};
    }

    for (addrinfo* info = result; info != nullptr; info = info->ai_next)
    {
        ENetAddress address;
        std::memset(&address, 0, sizeof(address));
        address.port = port;

        if (info->ai_family == AF_INET6)
        {
            const sockaddr_in6* sin6 = reinterpret_cast<const sockaddr_in6*>(info->ai_addr);
            address.host          = sin6->sin6_addr;
            address.sin6_scope_id = static_cast<enet_uint16>(sin6->sin6_scope_id);
            ipv6.push_back(address);
        }
        else if (info->ai_family == AF_INET)
        {
            map_ipv4(reinterpret_cast<const sockaddr_in*>(info->ai_addr)->sin_addr, address.host);
            ipv4.push_back(address);
        }
    }
    freeaddrinfo(result);

    std::vector<ENetAddress> addresses = ipv6;
    addresses.insert(addresses.end(), ipv4.begin(), ipv4.end());

    // getaddrinfo can repeat an address
    std::vector<ENetAddress> unique;
    for (const ENetAddress& address : addresses)
    {
        bool seen = std::any_of(unique.begin(), unique.end(), [&](const ENetAddress& other) {
            return std::memcmp(&other.host, &address.host, sizeof(address.host)) == 0;
        });
        if (!seen)
        {
            unique.push_back(address);
        }
    }
    return unique;
}

// keep the connection's settings the same for every peer
static void configure_peer(ENetPeer* peer)
{
    // never throttle (drop) unreliable packets: input frames are small (a
    // Frames message stays under the MTU and carries only the numbers of
    // execution plugin messages, whose bytes go on the reliable channel) and
    // carry their own redundancy, so dropping them only adds delay
    enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, 0, 0);
    // notice a lost connection within a few seconds
    enet_peer_timeout(peer, 32, 3000, 8000);
}

//
// Transport
//

Transport::Transport(void)
{
}

Transport::~Transport(void)
{
    this->Close();
}

bool Transport::Listen(uint16_t port, int maxPeers, Handlers handlers, std::string& error)
{
    if (!enet_acquire())
    {
        error = "Failed to initialize the network library";
        return false;
    }

    // every IPv6 and IPv4 address (the socket is dual-stack)
    ENetAddress address;
    std::memset(&address, 0, sizeof(address));
    address.host = ENET_HOST_ANY;
    address.port = port;

    ENetHost* host = enet_host_create(&address, static_cast<size_t>(maxPeers), 2, 0, 0);
    if (host == nullptr)
    {
        enet_release();
        error = "Failed to listen on UDP port " + std::to_string(port) + ". Is another program using it?";
        return false;
    }

    return this->Start(host, std::move(handlers));
}

bool Transport::Connect(const std::string& address, uint16_t port, Handlers handlers, std::string& error)
{
    if (!enet_acquire())
    {
        error = "Failed to initialize the network library";
        return false;
    }

    std::vector<ENetAddress> addresses = resolve(address, port);
    if (addresses.empty())
    {
        enet_release();
        error = "Couldn't find the address \"" + address + "\"";
        return false;
    }

    ENetHost* host = enet_host_create(nullptr, 1, 2, 0, 0);
    if (host == nullptr)
    {
        enet_release();
        error = "Failed to open a UDP socket";
        return false;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(this->mutex);
        this->host          = host;
        this->candidates    = addresses;
        this->everConnected = false;
        this->peers.push_back(nullptr); // the host is peer 1

        if (!this->ConnectNextCandidate())
        {
            enet_host_destroy(host);
            this->host = nullptr;
            this->peers.clear();
            enet_release();
            error = "Failed to start connecting";
            return false;
        }
    }

    return this->Start(host, std::move(handlers));
}

bool Transport::ConnectNextCandidate(void)
{
    while (!this->candidates.empty())
    {
        ENetAddress address = this->candidates.front();
        this->candidates.erase(this->candidates.begin());

        ENetPeer* peer = enet_host_connect(this->host, &address, 2, 0);
        if (peer != nullptr)
        {
            configure_connecting_peer(peer);
            this->peers[0] = peer;
            peer->data     = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
            return true;
        }
    }
    return false;
}

namespace
{
uint64_t transport_now_ms(void)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

bool Transport::Start(ENetHost* host, Handlers handlers)
{
#ifdef NETPLAY_NEW_TESTS
    const char* delay = std::getenv("RMG_NETPLAY_NEW_DELAY_MS");
    this->delayMs  = delay != nullptr ? static_cast<uint32_t>(std::max(0, std::atoi(delay))) : 0;
    const char* drop = std::getenv("RMG_NETPLAY_NEW_DROP_PERCENT");
    this->dropPercent = drop != nullptr ? std::atoi(drop) : 0;
#endif // NETPLAY_NEW_TESTS
    this->delayed.clear();
    this->host     = host;
    this->handlers = std::move(handlers);
    this->running  = true;
    this->thread   = std::thread(&Transport::Run, this);
    return true;
}

void Transport::Close(void)
{
    if (this->host == nullptr)
    {
        return;
    }

    this->running = false;
    if (this->thread.joinable())
    {
        this->thread.join();
    }

    std::lock_guard<std::recursive_mutex> lock(this->mutex);

    // say goodbye after whatever is still queued (like a last message),
    // and give the peers a moment to acknowledge it
    int connected = 0;
    for (ENetPeer* peer : this->peers)
    {
        if (peer != nullptr && peer->state == ENET_PEER_STATE_CONNECTED)
        {
            enet_peer_disconnect_later(peer, 0);
            connected++;
        }
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (connected > 0 && std::chrono::steady_clock::now() < deadline)
    {
        ENetEvent event;
        int result = enet_host_service(this->host, &event, 10);
        if (result < 0)
        {
            break;
        }
        if (result > 0)
        {
            if (event.type == ENET_EVENT_TYPE_DISCONNECT || event.type == ENET_EVENT_TYPE_DISCONNECT_TIMEOUT)
            {
                connected--;
            }
            else if (event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                enet_packet_destroy(event.packet);
            }
        }
    }

    for (ENetPeer* peer : this->peers)
    {
        if (peer != nullptr && peer->state != ENET_PEER_STATE_DISCONNECTED)
        {
            enet_peer_reset(peer);
        }
    }

    enet_host_destroy(this->host);
    this->host = nullptr;
    this->peers.clear();
    enet_release();
}

void Transport::Send(PeerId id, Channel channel, const std::vector<uint8_t>& data)
{
    std::lock_guard<std::recursive_mutex> lock(this->mutex);

    ENetPeer* peer = this->GetPeer(id);
    if (peer == nullptr || peer->state != ENET_PEER_STATE_CONNECTED)
    {
        return;
    }

#ifdef NETPLAY_NEW_TESTS
    if (channel == Channel::Input && this->dropPercent > 0 && static_cast<int>(this->dropRandom() % 100) < this->dropPercent)
    {
        return;
    }
#endif // NETPLAY_NEW_TESTS

    if (this->delayMs > 0)
    {
        this->delayed.push_back({transport_now_ms() + this->delayMs, id, channel, data});
        return;
    }

    this->SendNow(id, channel, data);
}

void Transport::SendDelayed(void)
{
    std::lock_guard<std::recursive_mutex> lock(this->mutex);

    uint64_t now = transport_now_ms();
    while (!this->delayed.empty() && this->delayed.front().due <= now)
    {
        DelayedMessage message = std::move(this->delayed.front());
        this->delayed.pop_front();
        this->SendNow(message.peer, message.channel, message.data);
    }
}

void Transport::SendNow(PeerId id, Channel channel, const std::vector<uint8_t>& data)
{
    ENetPeer* peer = this->GetPeer(id);
    if (peer == nullptr || peer->state != ENET_PEER_STATE_CONNECTED)
    {
        return;
    }

    // (an input packet bigger than the MTU, which the session avoids, would
    // be split into unreliable parts too, rather than reliable ones that hold
    // up the control channel)
    enet_uint32 flags = (channel == Channel::Control) ? ENET_PACKET_FLAG_RELIABLE
                                                      : (ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
    if (packet == nullptr)
    {
        return;
    }

    if (enet_peer_send(peer, static_cast<enet_uint8>(channel), packet) != 0)
    {
        enet_packet_destroy(packet);
        return;
    }

    // send it now instead of on the next service call
    enet_host_flush(this->host);
}

void Transport::Disconnect(PeerId id)
{
    std::lock_guard<std::recursive_mutex> lock(this->mutex);

    ENetPeer* peer = this->GetPeer(id);
    if (peer != nullptr)
    {
        enet_peer_disconnect_later(peer, 0);
        enet_host_flush(this->host);
    }
}

uint32_t Transport::GetRoundTripTime(PeerId id)
{
    std::lock_guard<std::recursive_mutex> lock(this->mutex);

    ENetPeer* peer = this->GetPeer(id);
    if (peer == nullptr || peer->state != ENET_PEER_STATE_CONNECTED)
    {
        return 0;
    }
    // (with the test delay, both ways; 0 unless testing)
    return peer->roundTripTime + 2 * this->delayMs;
}

uint32_t Transport::GetRoundTripTimeVariance(PeerId id)
{
    std::lock_guard<std::recursive_mutex> lock(this->mutex);

    ENetPeer* peer = this->GetPeer(id);
    if (peer == nullptr || peer->state != ENET_PEER_STATE_CONNECTED)
    {
        return 0;
    }
    return peer->roundTripTimeVariance;
}

PeerId Transport::GetId(ENetPeer* peer)
{
    return static_cast<PeerId>(reinterpret_cast<uintptr_t>(peer->data));
}

ENetPeer* Transport::GetPeer(PeerId id)
{
    if (id == InvalidPeer || id > this->peers.size())
    {
        return nullptr;
    }
    return this->peers[id - 1];
}

void Transport::Run(void)
{
    struct Event
    {
        ENetEventType type;
        PeerId peer;
        Channel channel;
        std::vector<uint8_t> data;
    };
    std::vector<Event> events;

    while (this->running)
    {
        events.clear();

        {
            std::lock_guard<std::recursive_mutex> lock(this->mutex);

            ENetEvent event;
            while (enet_host_service(this->host, &event, 0) > 0)
            {
                switch (event.type)
                {
                case ENET_EVENT_TYPE_CONNECT:
                {
                    PeerId id = this->GetId(event.peer);
                    if (id == InvalidPeer)
                    {
                        // a new peer connected to us
                        this->peers.push_back(event.peer);
                        id = static_cast<PeerId>(this->peers.size());
                        event.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
                    }
                    configure_peer(event.peer);
                    this->everConnected = true;
                    this->candidates.clear();
                    events.push_back({event.type, id, Channel::Control, {}});
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT:
                // this ENet reports a peer that stopped answering (or a connect
                // that never got an answer) separately; it's a disconnect all the same
                case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
                {
                    PeerId id = this->GetId(event.peer);
                    if (id != InvalidPeer)
                    {
                        this->peers[id - 1] = nullptr;
                        event.peer->data = nullptr;

                        // couldn't reach this address of the host: try its next one
                        if (!this->everConnected && !this->candidates.empty() && this->ConnectNextCandidate())
                        {
                            break;
                        }

                        events.push_back({ENET_EVENT_TYPE_DISCONNECT, id, Channel::Control, {}});
                    }
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE:
                {
                    PeerId id = this->GetId(event.peer);
                    if (id != InvalidPeer)
                    {
                        events.push_back({event.type, id, static_cast<Channel>(event.channelID),
                                          std::vector<uint8_t>(event.packet->data, event.packet->data + event.packet->dataLength)});
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
                default:
                    break;
                }
            }
        }

        for (const Event& event : events)
        {
            switch (event.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                if (this->handlers.connected)
                {
                    this->handlers.connected(event.peer);
                }
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if (this->handlers.disconnected)
                {
                    this->handlers.disconnected(event.peer);
                }
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                if (this->handlers.received)
                {
                    this->handlers.received(event.peer, event.channel, event.data.data(), event.data.size());
                }
                break;
            default:
                break;
            }
        }

        if (this->handlers.tick)
        {
            this->handlers.tick();
        }

        this->SendDelayed();
        bool delaying;
        {
            std::lock_guard<std::recursive_mutex> lock(this->mutex);
            delaying = !this->delayed.empty();
        }

        // wait for the next packet (this wakes up as soon as one arrives)
        enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
        enet_socket_wait(this->host->socket, &condition, delaying ? 1 : 5);
    }
}

std::vector<LocalAddress> Transport::GetLocalAddresses(void)
{
    std::vector<LocalAddress> addresses;

    // Winsock has to be started for inet_ntop
    if (!enet_acquire())
    {
        return addresses;
    }

    // Global IPv6 addresses are 2000::/3. Link-local (fe80::/10) and loopback
    // addresses are useless to others, and unique local ones (fc00::/7) only
    // work nearby, where IPv4 works too.
    auto add = [&](const sockaddr* address) {
        char text[INET6_ADDRSTRLEN] = {0};
        if (address->sa_family == AF_INET)
        {
            const in_addr& ipv4  = reinterpret_cast<const sockaddr_in*>(address)->sin_addr;
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&ipv4);
            if (bytes[0] == 127 || (bytes[0] == 169 && bytes[1] == 254))
            {
                return;
            }
            if (inet_ntop(AF_INET, &ipv4, text, sizeof(text)) != nullptr)
            {
                addresses.push_back({text, false, false});
            }
        }
        else if (address->sa_family == AF_INET6)
        {
            const in6_addr& ipv6 = reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr;
            if ((ipv6.s6_addr[0] & 0xE0) != 0x20)
            {
                return;
            }
            if (inet_ntop(AF_INET6, &ipv6, text, sizeof(text)) != nullptr)
            {
                addresses.push_back({text, true, true});
            }
        }
    };

#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer;
    ULONG result;
    do
    {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    } while (result == ERROR_BUFFER_OVERFLOW);

    if (result == NO_ERROR)
    {
        for (IP_ADAPTER_ADDRESSES* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
             adapter != nullptr; adapter = adapter->Next)
        {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            {
                continue;
            }
            for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
            {
                // temporary (privacy) IPv6 addresses change every day; share the stable one
                if (unicast->SuffixOrigin == IpSuffixOriginRandom)
                {
                    continue;
                }
                add(unicast->Address.lpSockaddr);
            }
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0)
    {
        for (ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next)
        {
            if (entry->ifa_addr != nullptr && !(entry->ifa_flags & IFF_LOOPBACK) && (entry->ifa_flags & IFF_UP))
            {
                add(entry->ifa_addr);
            }
        }
        freeifaddrs(interfaces);
    }
#endif

    enet_release();

    // IPv4 for the local network first, then global IPv6
    std::stable_sort(addresses.begin(), addresses.end(), [](const LocalAddress& a, const LocalAddress& b) {
        return a.ipv6 < b.ipv6;
    });
    return addresses;
}
