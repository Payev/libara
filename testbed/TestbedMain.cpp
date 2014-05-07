/*
 * $FU-Copyright$
 */

#include "CLibs.h"
#include "Testbed.h"
#include "TestbedAddress.h"
#include "TestbedARAClient.h"
#include "BasicConfiguration.h"
#include "TestbedRoutingTable.h"
#include "TestbedPacketDispatcher.h"
#include "TestbedCommandLineInterface.h"
#include "LinearPathReinforcementPolicy.h"
#include "BestPheromoneForwardingPolicy.h"

typedef u_char ara_address_t[ETHER_ADDR_LEN];

using namespace std;

std::shared_ptr<ARA::testbed::TestbedARAClient> client;

/**
 *
 */
ARA::BasicConfiguration createConfiguration(double deltaPhi, double initialPhi) {
    int maxTTL = 15;
    ARA::testbed::TestbedRoutingTable* routingTable = new ARA::testbed::TestbedRoutingTable();
    ARA::PacketFactory* packetFactory = new ARA::PacketFactory(maxTTL);
    ARA::EvaporationPolicy* evaporationPolicy = new ARA::ExponentialEvaporationPolicy(2.0, 100, 15.0);
    ARA::PathReinforcementPolicy* reinforcementPolicy = new ARA::LinearPathReinforcementPolicy(deltaPhi);
    ARA::ForwardingPolicy* forwardingPolicy = new ARA::BestPheromoneForwardingPolicy(routingTable);
    return ARA::BasicConfiguration(routingTable, packetFactory, evaporationPolicy, reinforcementPolicy, forwardingPolicy, initialPhi);
}

void dumpDessertMessage(dessert_msg_t* message){
    ether_header* header = ARA::testbed::extractEthernetHeader(message);
    ARA::testbed::TestbedAddress source(header->ether_shost);
    ARA::testbed::TestbedAddress destination(header->ether_dhost);
    cout << "[dumpDessertMessage] source: " << source.toString() << " destination: " << destination.toString() << std::endl;
    cout << "[dumpDessertMessage] type: " << header->ether_type << std::endl;
}

_dessert_cb_results tapPacketFilter(dessert_msg_t* messageReceived, uint32_t length, dessert_msg_proc_t *processingFlags, dessert_sysif_t *interface, dessert_frameid_t id) {
    ether_header* ethernetFrame = ARA::testbed::extractEthernetHeader(messageReceived);
    if(ethernetFrame->ether_dhost[0] == 51) {
        return DESSERT_MSG_DROP;
    } else {
        return DESSERT_MSG_KEEP;
    }
}

_dessert_cb_results messageFromTapInterfaceDispatcher(dessert_msg_t* messageReceived, uint32_t length, dessert_msg_proc_t *processingFlags, dessert_sysif_t *interface, dessert_frameid_t id) {
    std::cout << "[messageFromTapInterfaceDispatcher] heyho i've got a packet" << std::endl;
    dumpDessertMessage(messageReceived);
    client->sendPacket(ARA::testbed::tapMessageToPacket(messageReceived, client));
    return DESSERT_MSG_DROP;
}

int main(int argc, char** argv) {
     FILE* cfg = dessert_cli_get_cfg(argc, argv);

     dessert_init("ARA", 0x01, DESSERT_OPT_NODAEMONIZE);
     dessert_logcfg(DESSERT_LOG_STDERR | DESSERT_LOG_GZ); 

     cli_register_command(dessert_cli, dessert_cli_cfg_iface, const_cast<char*>("sys"), dessert_cli_cmd_addsysif, PRIVILEGE_PRIVILEGED, MODE_CONFIG, const_cast<char*>("initialize tap interface"));
     cli_register_command(dessert_cli, dessert_cli_cfg_iface, const_cast<char*>("mesh"), dessert_cli_cmd_addmeshif, PRIVILEGE_PRIVILEGED, MODE_CONFIG, const_cast<char*>("initialize mesh interface"));
     cli_register_command(dessert_cli, dessert_cli_show, const_cast<char*>("testSendMesh"), ARA::testbed::cli_test_send_mesh, PRIVILEGE_UNPRIVILEGED, MODE_ANY, const_cast<char*>("send a test packet to mesh interface"));

     dessert_cb_result (*fromTAP)(dessert_msg_t*, uint32_t, dessert_msg_proc_t*, dessert_sysif_t*, dessert_frameid_t) = &messageFromTapInterfaceDispatcher;
     _dessert_cb_results (*fromMesh)(dessert_msg_t*, uint32_t, dessert_msg_proc_t*, dessert_meshif_t*, dessert_frameid_t) = &ARA::testbed::messageFromMeshInterfaceDispatcher;

     dessert_debug("applying configuration");
     cli_file(dessert_cli, cfg, PRIVILEGE_PRIVILEGED, MODE_CONFIG);
     dessert_debug("configuration applied");

     ARA::BasicConfiguration config = createConfiguration(5.0, 5.0);
     client = std::make_shared<ARA::testbed::TestbedARAClient>(config);

     dessert_sysrxcb_add(&tapPacketFilter, 5);
     dessert_sysrxcb_add(fromTAP, 15);
     dessert_meshrxcb_add(dessert_msg_ifaceflags_cb, 15);
     dessert_meshrxcb_add(fromMesh, 30);

     dessert_cli_run();
     dessert_run();

     return 0;
}
