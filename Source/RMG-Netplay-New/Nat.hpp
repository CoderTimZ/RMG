/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMG_NETPLAY_NEW_NAT_HPP
#define RMG_NETPLAY_NEW_NAT_HPP

#include <cstdint>
#include <memory>
#include <string>

namespace NetplayNew
{
// Asks the router to forward a UDP port to this computer, with NAT-PMP
// (or PCP-compatible routers that still answer NAT-PMP) or else UPnP-IGD.
// Only for IPv4: IPv6 has no NAT to get through.
class PortMapper
{
  public:
    struct Result
    {
        bool mapped = false;
        // "NAT-PMP" or "UPnP"
        std::string method;
        // the router's public IPv4 address, if it said
        std::string publicAddress;
        // why it didn't work, or a warning (like a router that is itself behind a NAT)
        std::string message;
    };

    PortMapper(void);
    ~PortMapper(void);

    // blocks for up to a few seconds; call it again to renew the mapping
    Result Map(uint16_t port);

    // removes the mapping Map made, if any
    void Unmap(void);

  private:
    struct UPnP;
    std::unique_ptr<UPnP> upnp;
    uint16_t port = 0;
    std::string method;

    Result MapWithNatPmp(uint16_t port, uint32_t lifetime);
    Result MapWithUPnP(uint16_t port, uint32_t lifetime);
};
} // namespace NetplayNew

#endif // RMG_NETPLAY_NEW_NAT_HPP
