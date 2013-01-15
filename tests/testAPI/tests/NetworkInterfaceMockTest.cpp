/******************************************************************************
 Copyright 2012, The DES-SERT Team, Freie Universität Berlin (FUB).
 All rights reserved.

 These sources were originally developed by Friedrich Große
 at Freie Universität Berlin (http://www.fu-berlin.de/),
 Computer Systems and Telematics / Distributed, Embedded Systems (DES) group
 (http://cst.mi.fu-berlin.de/, http://www.des-testbed.net/)
 ------------------------------------------------------------------------------
 This program is free software: you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free Software
 Foundation, either version 3 of the License, or (at your option) any later
 version.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License along with
 this program. If not, see http://www.gnu.org/licenses/ .
 ------------------------------------------------------------------------------
 For further information and questions please use the web site
 http://www.des-testbed.net/
 *******************************************************************************/

#include "CppUTest/TestHarness.h"
#include "testAPI/mocks/NetworkInterfaceMock.h"
#include "testAPI/mocks/PacketMock.h"
#include "testAPI/mocks/AddressMock.h"
#include "testAPI/mocks/ARAClientMock.h"
#include "LinkedList.h"
#include "Pair.h"

using namespace ARA;
using namespace std;

TEST_GROUP(NetworkInterfaceMockTest) {};

TEST(NetworkInterfaceMockTest, testMockRemembersSentPackets) {
    NetworkInterfaceMock mock = NetworkInterfaceMock();
    AddressMock recipient1 = AddressMock();
    AddressMock recipient2 = AddressMock();
    PacketMock packet1 = PacketMock();
    PacketMock packet2 = PacketMock();
    PacketMock packet3 = PacketMock();

    mock.send(&packet1, &recipient1);
    mock.send(&packet2, &recipient2);
    mock.send(&packet3, &recipient1);

    LinkedList<Pair<Packet, Address>>* sendPackets = mock.getSentPackets();

    CHECK(sendPackets->get(0)->getLeft()->equals(&packet1));
    CHECK(sendPackets->get(0)->getRight()->equals(&recipient1));

    CHECK(sendPackets->get(1)->getLeft()->equals(&packet2));
    CHECK(sendPackets->get(1)->getRight()->equals(&recipient2));

    CHECK(sendPackets->get(2)->getLeft()->equals(&packet3));
    CHECK(sendPackets->get(2)->getRight()->equals(&recipient1));
}

TEST(NetworkInterfaceMockTest, testHasPacketBeenBroadCasted) {
    NetworkInterfaceMock interface = NetworkInterfaceMock();
    PacketMock packet1 = PacketMock("Source", "Destination", 1);
    PacketMock packet2 = PacketMock("Earth", "Moon", 1234);
    PacketMock packet3 = PacketMock("Source", "Destination", 2);
    AddressMock viaAddress = AddressMock();

    interface.send(&packet1, &viaAddress);
    interface.broadcast(&packet2);
    interface.send(&packet3, &viaAddress);

    CHECK(interface.hasPacketBeenBroadCasted(&packet1) == false);
    CHECK(interface.hasPacketBeenBroadCasted(&packet2) == true);
    CHECK(interface.hasPacketBeenBroadCasted(&packet3) == false);
}

TEST(NetworkInterfaceMockTest, testHasPacketBeenSend) {
    NetworkInterfaceMock interface = NetworkInterfaceMock();
    PacketMock packet1 = PacketMock("Source", "Destination", 1);
    PacketMock packet2 = PacketMock("Source", "Destination", 2);
    PacketMock packet3 = PacketMock("Source", "Destination", 3);
    AddressMock address = AddressMock();

    interface.send(&packet1, &address);
    interface.broadcast(&packet2);

    CHECK(interface.hasPacketBeenSend(&packet1) == true);
    CHECK(interface.hasPacketBeenSend(&packet2) == true);
    CHECK(interface.hasPacketBeenSend(&packet3) == false);
}

TEST(NetworkInterfaceMockTest, testEquals) {
    NetworkInterfaceMock interface = NetworkInterfaceMock("eth1");
    NetworkInterfaceMock sameInterface = NetworkInterfaceMock("eth1");
    NetworkInterfaceMock otherInterface = NetworkInterfaceMock("eth2");

    CHECK(interface.equals(&interface));
    CHECK(interface.equals(&sameInterface));
    CHECK(sameInterface.equals(&interface));

    CHECK(interface.equals(&otherInterface) == false);
    CHECK(sameInterface.equals(&otherInterface) == false);
}

TEST(NetworkInterfaceMockTest, testGetNumberOfSentPackets) {
    NetworkInterfaceMock interface = NetworkInterfaceMock();
    PacketMock packet1 = PacketMock("Source", "Destination", 1);
    PacketMock packet2 = PacketMock("Source", "Destination", 2);
    AddressMock address1 = AddressMock("A");
    AddressMock address2 = AddressMock("B");

    CHECK_EQUAL(0, interface.getNumberOfSentPackets());

    interface.send(&packet1, &address1);
    CHECK_EQUAL(1, interface.getNumberOfSentPackets());

    interface.send(&packet1, &address2);
    CHECK_EQUAL(2, interface.getNumberOfSentPackets());

    interface.send(&packet2, &address1);
    CHECK_EQUAL(3, interface.getNumberOfSentPackets());
}

TEST(NetworkInterfaceMockTest, testGetLocalAddress) {
    NetworkInterfaceMock interface = NetworkInterfaceMock();
    AddressMock expectedLocalAddress = AddressMock("DEFAULT");
    shared_ptr<Address> defaultLocalAddress = interface.getLocalAddress();
    CHECK(defaultLocalAddress->equals(&expectedLocalAddress));

    interface = NetworkInterfaceMock("eth0", "192.168.0.1");
    expectedLocalAddress = AddressMock("192.168.0.1");
    defaultLocalAddress = interface.getLocalAddress();
    CHECK(defaultLocalAddress->equals(&expectedLocalAddress));
}
