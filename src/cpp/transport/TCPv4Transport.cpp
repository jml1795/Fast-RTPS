// Copyright 2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fastrtps/transport/TCPTransportInterface.h>
#include <fastrtps/transport/TCPv4Transport.h>
#include <fastrtps/transport/tcp/RTCPMessageManager.h>
#include <fastrtps/transport/timedevent/CleanTCPSocketsEvent.h>
#include <utility>
#include <cstring>
#include <algorithm>
#include <fastrtps/log/Log.h>
#include <fastrtps/rtps/messages/RTPSMessageCreator.h>
#include "asio.hpp"
#include <fastrtps/rtps/network/ReceiverResource.h>
#include <fastrtps/rtps/network/SenderResource.h>
#include <fastrtps/utils/eClock.h>
#include <fastrtps/utils/IPLocator.h>
#include <fastrtps/transport/TCPv4TransportDescriptor.h>

using namespace std;
using namespace asio;

namespace eprosima{
namespace fastrtps{
namespace rtps {

static void GetIP4s(std::vector<IPFinder::info_IP>& locNames, bool return_loopback = false)
{
    IPFinder::getIPs(&locNames, return_loopback);
    auto new_end = remove_if(locNames.begin(),
        locNames.end(),
        [](IPFinder::info_IP ip) {return ip.type != IPFinder::IP4 && ip.type != IPFinder::IP4_LOCAL; });
    locNames.erase(new_end, locNames.end());
    std::for_each(locNames.begin(), locNames.end(), [](IPFinder::info_IP& loc)
    {
        loc.locator.kind = LOCATOR_KIND_TCPv4;
    });
}

static asio::ip::address_v4::bytes_type locatorToNative(Locator_t& locator, const octet* local_wan)
{
    const octet* wan = IPLocator::getWan(locator);
    if (IPLocator::hasWan(locator) && (memcmp(local_wan, wan, 4) != 0))
    {
        return{ { wan[0], wan[1], wan[2], wan[3]} };
    }
    else
    {
        return{ { IPLocator::getIPv4(locator)[0],
            IPLocator::getIPv4(locator)[1],
            IPLocator::getIPv4(locator)[2],
            IPLocator::getIPv4(locator)[3]} };
    }
}

TCPv4Transport::TCPv4Transport(const TCPv4TransportDescriptor& descriptor)
    : mConfiguration_(descriptor)
{
    mTransportKind = LOCATOR_KIND_TCPv4;
    for (const auto& interface : descriptor.interfaceWhiteList)
    {
        mInterfaceWhiteList.emplace_back(ip::address_v4::from_string(interface));
    }

    for (uint16_t port : mConfiguration_.listening_ports)
    {
        Locator_t locator(LOCATOR_KIND_TCPv4, port);
        CreateAcceptorSocket(locator);
    }
}

TCPv4TransportDescriptor::TCPv4TransportDescriptor()
    : TCPTransportDescriptor()
{
    memset(wan_addr, 0, 4);
}

TCPv4TransportDescriptor::TCPv4TransportDescriptor(const TCPv4TransportDescriptor& t)
    : TCPTransportDescriptor(t)
{
    memcpy(wan_addr, t.wan_addr, 4);
}

TransportInterface* TCPv4TransportDescriptor::create_transport() const
{
    return new TCPv4Transport(*this);
}

TCPv4Transport::TCPv4Transport()
{
    mTransportKind = LOCATOR_KIND_TCPv4;
}

TCPv4Transport::~TCPv4Transport()
{
    Clean();
}

void TCPv4Transport::AddDefaultOutputLocator(LocatorList_t&)
{
}

const TCPTransportDescriptor* TCPv4Transport::GetConfiguration() const
{
    return &mConfiguration_;
}

TCPTransportDescriptor* TCPv4Transport::GetConfiguration()
{
    return &mConfiguration_;
}

void TCPv4Transport::GetIPs(std::vector<IPFinder::info_IP>& locNames, bool return_loopback) const
{
    GetIP4s(locNames, return_loopback);
}

uint16_t TCPv4Transport::GetLogicalPortIncrement() const
{
    return mConfiguration_.logical_port_increment;
}

uint16_t TCPv4Transport::GetLogicalPortRange() const
{
    return mConfiguration_.logical_port_range;
}

uint16_t TCPv4Transport::GetMaxLogicalPort() const
{
    return mConfiguration_.max_logical_port;
}

std::vector<std::string> TCPv4Transport::GetBindingInterfacesList()
{
    std::vector<std::string> vOutputInterfaces;
    if (IsInterfaceWhiteListEmpty())
    {
        vOutputInterfaces.push_back(s_IPv4AddressAny);
    }
    else
    {
        for (auto& ip : mInterfaceWhiteList)
        {
            vOutputInterfaces.push_back(ip.to_string());
        }
    }

    return vOutputInterfaces;
}
bool TCPv4Transport::IsInterfaceWhiteListEmpty() const
{
    return mInterfaceWhiteList.empty();
}

bool TCPv4Transport::IsInterfaceAllowed(const std::string& interface) const
{
    return IsInterfaceAllowed(asio::ip::address_v4::from_string(interface));
}

bool TCPv4Transport::IsInterfaceAllowed(const ip::address_v4& ip) const
{
    if (mInterfaceWhiteList.empty())
        return true;

    if (ip == ip::address_v4::any())
        return true;

    return find(mInterfaceWhiteList.begin(), mInterfaceWhiteList.end(), ip) != mInterfaceWhiteList.end();
}

LocatorList_t TCPv4Transport::NormalizeLocator(const Locator_t& locator)
{
    LocatorList_t list;

    if (IPLocator::isAny(locator))
    {
        std::vector<IPFinder::info_IP> locNames;
        GetIP4s(locNames);
        for (const auto& infoIP : locNames)
        {
            Locator_t newloc(locator);
            IPLocator::setIPv4(newloc, infoIP.locator);
            list.push_back(newloc);
        }
    }
    else
        list.push_back(locator);

    return list;
}

bool TCPv4Transport::is_local_locator(const Locator_t& locator) const
{
    assert(locator.kind == LOCATOR_KIND_TCPv4);

    if (IPLocator::isLocal(locator))
    {
        return true;
    }

    for (auto localInterface : mCurrentInterfaces)
    {
        if (IPLocator::compareAddress(locator, localInterface.locator))
        {
            return true;
        }
    }

    return false;
}

bool TCPv4Transport::IsLocatorAllowed(const Locator_t& locator) const
{
    if (!IsLocatorSupported(locator))
    {
        return false;
    }
    if (mInterfaceWhiteList.empty())
    {
        return true;
    }
    return IsInterfaceAllowed(IPLocator::toIPv4string(locator));
}

bool TCPv4Transport::CompareLocatorIP(const Locator_t& lh, const Locator_t& rh) const
{
    return IPLocator::compareAddress(lh, rh);
}

bool TCPv4Transport::CompareLocatorIPAndPort(const Locator_t& lh, const Locator_t& rh) const
{
    return IPLocator::compareAddressAndPhysicalPort(lh, rh);
}

void TCPv4Transport::FillLocalIp(Locator_t& loc) const
{
    IPLocator::setIPv4(loc, "127.0.0.1");
    loc.kind = LOCATOR_KIND_TCPv4;
}

ip::tcp::endpoint TCPv4Transport::GenerateEndpoint(const Locator_t& loc, uint16_t port) const
{
    asio::ip::address_v4::bytes_type remoteAddress;
    IPLocator::copyIPv4(loc, remoteAddress.data());
    return ip::tcp::endpoint(asio::ip::address_v4(remoteAddress), port);
}

ip::tcp::endpoint TCPv4Transport::GenerateLocalEndpoint(Locator_t& loc, uint16_t port) const
{
    return ip::tcp::endpoint(asio::ip::address_v4(locatorToNative(loc, mConfiguration_.wan_addr)), port);
}

ip::tcp::endpoint TCPv4Transport::GenerateEndpoint(uint16_t port) const
{
    return asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port);
}

asio::ip::tcp TCPv4Transport::GenerateProtocol() const
{
    return asio::ip::tcp::v4();
}

bool TCPv4Transport::IsInterfaceAllowed(const Locator_t& loc) const
{
    asio::ip::address_v4 ip = asio::ip::address_v4::from_string(IPLocator::toIPv4string(loc));
    return IsInterfaceAllowed(ip);
}

void TCPv4Transport::SetReceiveBufferSize(uint32_t size)
{
    mConfiguration_.receiveBufferSize = size;
}

void TCPv4Transport::SetSendBufferSize(uint32_t size)
{
    mConfiguration_.sendBufferSize = size;
}

void TCPv4Transport::EndpointToLocator(const ip::tcp::endpoint& endpoint, Locator_t& locator) const
{
    locator.kind = LOCATOR_KIND_TCPv4;
    IPLocator::setPhysicalPort(locator, endpoint.port());
    auto ipBytes = endpoint.address().to_v4().to_bytes();
    IPLocator::setIPv4(locator, ipBytes.data());
}

bool TCPv4Transport::fillMetatrafficUnicastLocator(Locator_t &locator, uint32_t metatraffic_unicast_port) const
{
    bool result = TCPTransportInterface::fillMetatrafficUnicastLocator(locator, metatraffic_unicast_port);

    IPLocator::setWan(locator,
        mConfiguration_.wan_addr[0], mConfiguration_.wan_addr[1],
        mConfiguration_.wan_addr[2], mConfiguration_.wan_addr[3]);

    return result;
}

bool TCPv4Transport::fillUnicastLocator(Locator_t &locator, uint32_t well_known_port) const
{
    bool result = TCPTransportInterface::fillUnicastLocator(locator, well_known_port);

    IPLocator::setWan(locator,
        mConfiguration_.wan_addr[0], mConfiguration_.wan_addr[1],
        mConfiguration_.wan_addr[2], mConfiguration_.wan_addr[3]);

    return result;
}

LocatorList_t TCPv4Transport::ShrinkLocatorLists(const std::vector<LocatorList_t>& locatorLists)
{
    LocatorList_t unicastResult;
    LocatorList_t connectedLocators;
    for (auto it = mChannelResources.begin(); it != mChannelResources.end(); ++it)
    {
        connectedLocators.push_back(it->first);
    }

    for (const LocatorList_t& locatorList : locatorLists)
    {
        LocatorListConstIterator it = locatorList.begin();
        LocatorList_t pendingUnicast;

        bool addLocator = true;
        while (it != locatorList.end())
        {
            assert((*it).kind == mTransportKind);
            addLocator = true;

            // Check is local interface.
            auto localInterface = mCurrentInterfaces.begin();
            for (; localInterface != mCurrentInterfaces.end(); ++localInterface)
            {
                if (CompareLocatorIP(localInterface->locator, *it))
                {
                    // Loopback locator
                    Locator_t loopbackLocator;
                    FillLocalIp(loopbackLocator);
                    IPLocator::setPhysicalPort(loopbackLocator, IPLocator::getPhysicalPort(*it));
                    IPLocator::setLogicalPort(loopbackLocator, IPLocator::getLogicalPort(*it));
                    pendingUnicast.push_back(loopbackLocator);
                    addLocator = false;
                    break;
                }
            }

            // Add localhost?
            if (localInterface == mCurrentInterfaces.end() && IPLocator::isLocal(*it))
            {
                pendingUnicast.push_back(*it);
                ++it;
                continue;
            }
            else if (!addLocator)
            {
                ++it;
                continue;
            }

            // Check Remote WAN locators.
            if (memcmp(IPLocator::getWan(*it), mConfiguration_.wan_addr, 4) != 0)
            {
                // Only allow one locator with the same WAN and physical port.
                for (auto unicastLocator = unicastResult.begin(); unicastLocator != unicastResult.end(); ++unicastLocator)
                {
                    if (memcmp(IPLocator::getWan(*unicastLocator), IPLocator::getWan(*it), 4) == 0 && unicastLocator->port == it->port)
                    {
                        addLocator = false;
                        break;
                    }
                }
            }
            else
            {
                // With the same wan than the server, only allow one locator with the same address and physical port.
                for (auto unicastLocator = unicastResult.begin(); unicastLocator != unicastResult.end(); ++unicastLocator)
                {
                    if (memcmp(IPLocator::getIPv4(*unicastLocator), IPLocator::getIPv4(*it), 4) == 0 && unicastLocator->port == it->port)
                    {
                        addLocator = false;
                        break;
                    }
                }
            }

            if (addLocator)
            {
                addLocator = false;

                // Only allow already connected locators.
                for (auto locatorIt = connectedLocators.begin(); locatorIt != connectedLocators.end(); ++locatorIt)
                {
                    if (((IPLocator::hasWan(*it) && memcmp(IPLocator::getWan(*it), IPLocator::getIPv4(*locatorIt), 4) == 0) ||
                        (!IPLocator::hasWan(*it) && memcmp(IPLocator::getIPv4(*it), IPLocator::getIPv4(*locatorIt), 4) == 0)) &&
                        IPLocator::getPhysicalPort(*locatorIt) == IPLocator::getPhysicalPort(*it))
                    {
                        addLocator = true;
                        break;
                    }
                }

                if (addLocator)
                {
                    pendingUnicast.push_back(*it);
                }
            }
            ++it;
        }

        unicastResult.push_back(pendingUnicast);
    }

    if (!IsInterfaceWhiteListEmpty() && unicastResult.size() > 0)
    {
        bool bValid = false;
        for (Locator_t loc : unicastResult)
        {
            if (IsInterfaceAllowed(IPLocator::toIPv4string(loc)))
            {
                bValid = true;
            }
        }

        if (!bValid)
        {
            logError(RTCP, "There isn't any valid TCP Address on the whitelist");
        }
    }

    LocatorList_t result(std::move(unicastResult));
    return result;
}

} // namespace rtps
} // namespace fastrtps
} // namespace eprosima
