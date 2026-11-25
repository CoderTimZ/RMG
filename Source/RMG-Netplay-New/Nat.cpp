/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#endif

#include <natpmp.h>
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#include "Nat.hpp"

#include <chrono>
#include <cstring>
#include <mutex>

using namespace NetplayNew;

namespace
{
// how long a mapping lasts unless it's renewed (the session renews it)
constexpr uint32_t LifetimeSeconds = 2 * 60 * 60;

// how long to wait for a NAT-PMP answer, and for UPnP routers to answer discovery
constexpr int NatPmpTimeoutMs  = 2000;
constexpr int DiscoveryDelayMs = 2000;

constexpr const char* Description = "RMG netplay";

#ifdef _WIN32
// the libraries use Winsock, which has to be started
std::once_flag l_WinsockOnce;
void start_winsock(void)
{
    std::call_once(l_WinsockOnce, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#else
void start_winsock(void)
{
}
#endif
} // namespace

struct PortMapper::UPnP
{
    UPNPUrls urls;
    IGDdatas data;
    bool valid = false;

    ~UPnP(void)
    {
        if (this->valid)
        {
            FreeUPNPUrls(&this->urls);
        }
    }
};

PortMapper::PortMapper(void)
{
}

PortMapper::~PortMapper(void)
{
    this->Unmap();
}

PortMapper::Result PortMapper::Map(uint16_t port)
{
    start_winsock();

    // NAT-PMP answers within milliseconds when the router speaks it, so it goes first
    Result result = this->MapWithNatPmp(port, LifetimeSeconds);
    if (result.mapped)
    {
        this->port   = port;
        this->method = result.method;
        return result;
    }

    Result upnp = this->MapWithUPnP(port, LifetimeSeconds);
    if (upnp.mapped)
    {
        this->port   = port;
        this->method = upnp.method;
        return upnp;
    }

    // say why UPnP failed; it's the more common one
    Result failed;
    failed.message = upnp.message.empty() ? result.message : upnp.message;
    return failed;
}

void PortMapper::Unmap(void)
{
    if (this->port == 0)
    {
        return;
    }

    if (this->method == "NAT-PMP")
    {
        // a lifetime of 0 removes the mapping
        this->MapWithNatPmp(this->port, 0);
    }
    else if (this->method == "UPnP" && this->upnp && this->upnp->valid)
    {
        std::string text = std::to_string(this->port);
        UPNP_DeletePortMapping(this->upnp->urls.controlURL, this->upnp->data.first.servicetype,
                               text.c_str(), "UDP", nullptr);
    }

    this->port = 0;
    this->method.clear();
}

PortMapper::Result PortMapper::MapWithNatPmp(uint16_t port, uint32_t lifetime)
{
    Result result;
    natpmp_t natpmp;

    if (initnatpmp(&natpmp, 0, 0) < 0)
    {
        result.message = "Couldn't find the router";
        return result;
    }

    auto wait = [&](natpmpresp_t& response) -> int {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(NatPmpTimeoutMs);
        int r;
        do
        {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(natpmp.s, &fds);
            timeval timeout;
            getnatpmprequesttimeout(&natpmp, &timeout);
            // never wait past the deadline
            auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0)
            {
                return NATPMP_TRYAGAIN;
            }
            if (static_cast<long long>(timeout.tv_sec) * 1000000 + timeout.tv_usec > left)
            {
                timeout.tv_sec  = static_cast<long>(left / 1000000);
                timeout.tv_usec = static_cast<long>(left % 1000000);
            }
            select(static_cast<int>(natpmp.s) + 1, &fds, nullptr, nullptr, &timeout);
            r = readnatpmpresponseorretry(&natpmp, &response);
        } while (r == NATPMP_TRYAGAIN);
        return r;
    };

    natpmpresp_t response;

    // the router's public address (for showing it); a router that doesn't
    // answer this doesn't speak NAT-PMP, so don't wait for it twice
    if (lifetime != 0)
    {
        int r = sendpublicaddressrequest(&natpmp);
        if (r >= 0)
        {
            r = wait(response);
        }
        if (r == NATPMP_TRYAGAIN || r < 0)
        {
            result.message = r == NATPMP_TRYAGAIN ? "The router didn't answer NAT-PMP" : std::string("NAT-PMP: ") + strnatpmperr(r);
            closenatpmp(&natpmp);
            return result;
        }
        if (response.type == NATPMP_RESPTYPE_PUBLICADDRESS)
        {
            char text[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &response.pnu.publicaddress.addr, text, sizeof(text));
            result.publicAddress = text;
        }
    }

    int r = sendnewportmappingrequest(&natpmp, NATPMP_PROTOCOL_UDP, port, port, lifetime);
    if (r >= 0)
    {
        r = wait(response);
    }

    if (r >= 0 && response.type == NATPMP_RESPTYPE_UDPPORTMAPPING)
    {
        result.mapped = lifetime == 0 || response.pnu.newportmapping.mappedpublicport == port;
        result.method = "NAT-PMP";
        if (!result.mapped)
        {
            result.message = "The router gave out port " + std::to_string(response.pnu.newportmapping.mappedpublicport) +
                             " instead of " + std::to_string(port);
        }
    }
    else
    {
        result.message = r == NATPMP_TRYAGAIN ? "The router didn't answer NAT-PMP" : std::string("NAT-PMP: ") + strnatpmperr(r);
    }

    closenatpmp(&natpmp);
    return result;
}

PortMapper::Result PortMapper::MapWithUPnP(uint16_t port, uint32_t lifetime)
{
    Result result;
    int error = 0;

    UPNPDev* devices = upnpDiscover(DiscoveryDelayMs, nullptr, nullptr, 0, 0, 2, &error);
    if (devices == nullptr)
    {
        result.message = "No router answered UPnP (it may be turned off in the router's settings)";
        return result;
    }

    this->upnp = std::make_unique<UPnP>();
    char lanAddress[64] = {0};
    char wanAddress[64] = {0};
    int status = UPNP_GetValidIGD(devices, &this->upnp->urls, &this->upnp->data, lanAddress, sizeof(lanAddress),
                                  wanAddress, sizeof(wanAddress));
    freeUPNPDevlist(devices);

    if (status <= 0)
    {
        this->upnp.reset();
        result.message = "No router answered UPnP (it may be turned off in the router's settings)";
        return result;
    }
    this->upnp->valid = true;

    if (status == 3 || status == 4)
    {
        result.message = "The UPnP device that answered isn't a connected router";
        return result;
    }

    std::string text = std::to_string(port);
    std::string lease = std::to_string(lifetime);
    int r = UPNP_AddPortMapping(this->upnp->urls.controlURL, this->upnp->data.first.servicetype, text.c_str(),
                                text.c_str(), lanAddress, Description, "UDP", nullptr, lease.c_str());
    if (r == 725) // OnlyPermanentLeasesSupported
    {
        r = UPNP_AddPortMapping(this->upnp->urls.controlURL, this->upnp->data.first.servicetype, text.c_str(),
                                text.c_str(), lanAddress, Description, "UDP", nullptr, "0");
    }

    if (r != UPNPCOMMAND_SUCCESS)
    {
        result.message = std::string("The router refused the UPnP mapping: ") + strupnperror(r);
        return result;
    }

    result.mapped        = true;
    result.method        = "UPnP";
    result.publicAddress = wanAddress;

    // 2: the router's own outside address is private, so it's behind another
    // NAT (double NAT, or the ISP's carrier-grade NAT) and the mapping can't
    // reach the internet on its own
    if (status == 2)
    {
        result.message = "The router is itself behind another router or the ISP's NAT (its outside address, " +
                         std::string(wanAddress) + ", is private), so players outside may still not reach you";
    }

    return result;
}
