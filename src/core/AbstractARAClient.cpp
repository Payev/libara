/*
 * $FU-Copyright$
 */

#include "AbstractARAClient.h"
#include "PacketType.h"
#include "Timer.h"
#include "Environment.h"
#include "Exception.h"

#include "sstream"
using namespace std;

ARA_NAMESPACE_BEGIN

typedef std::unordered_map<Timer*, RouteDiscoveryInfo> DiscoveryTimerInfo;
typedef std::unordered_map<Timer*, AddressPtr> DeliveryTimerInfo;

AbstractARAClient::AbstractARAClient(Configuration& configuration, RoutingTable *routingTable, PacketFactory* packetFactory) {
    initialize(configuration, routingTable, packetFactory);
}

void AbstractARAClient::initialize(Configuration& configuration, RoutingTable* routingTable, PacketFactory* packetFactory) {
    forwardingPolicy = configuration.getForwardingPolicy();
    pathReinforcementPolicy = configuration.getReinforcementPolicy();
    evaporationPolicy = configuration.getEvaporationPolicy();
    initialPheromoneValue = configuration.getInitialPheromoneValue();
    maxNrOfRouteDiscoveryRetries = configuration.getMaxNrOfRouteDiscoveryRetries();
    routeDiscoveryTimeoutInMilliSeconds = configuration.getRouteDiscoveryTimeoutInMilliSeconds();
    packetDeliveryDelayInMilliSeconds = configuration.getPacketDeliveryDelayInMilliSeconds();

    this->packetFactory = packetFactory;
    this->routingTable = routingTable;
    routingTable->setEvaporationPolicy(evaporationPolicy);

    packetTrap = new PacketTrap(routingTable);
    runningRouteDiscoveries = unordered_map<AddressPtr, Timer*, AddressHash, AddressPredicate>();
    runningRouteDiscoveryTimers = unordered_map<Timer*, RouteDiscoveryInfo>();
    runningDeliveryTimers = unordered_map<Timer*, AddressPtr>();
}

AbstractARAClient::~AbstractARAClient() {
    // delete logger if it has been set
    if(logger != nullptr) {
        delete logger;
    }

    // delete the sequence number lists of the last received packets
    for (LastReceivedPacketsMap::iterator iterator=lastReceivedPackets.begin(); iterator!=lastReceivedPackets.end(); iterator++) {
        // the addresses are disposed of automatically by shared_ptr
        delete iterator->second;
    }
    lastReceivedPackets.clear();

    // delete the known intermediate hop addresses for all sources
    for (KnownIntermediateHopsMap::iterator iterator=knownIntermediateHops.begin(); iterator!=knownIntermediateHops.end(); iterator++) {
        delete iterator->second;
    }
    knownIntermediateHops.clear();

    // delete running route discovery timers
    for (DiscoveryTimerInfo::iterator iterator=runningRouteDiscoveryTimers.begin(); iterator!=runningRouteDiscoveryTimers.end(); iterator++) {
        delete iterator->first;
    }
    runningRouteDiscoveryTimers.clear();

    // delete running delivery timers
    for (DeliveryTimerInfo::iterator iterator=runningDeliveryTimers.begin(); iterator!=runningDeliveryTimers.end(); iterator++) {
        delete iterator->first;
    }
    runningDeliveryTimers.clear();

    /* The following members may have be deleted earlier, depending on the destructor of the implementing class */
    DELETE_IF_NOT_NULL(packetFactory);
    DELETE_IF_NOT_NULL(packetTrap);
    DELETE_IF_NOT_NULL(routingTable);
    DELETE_IF_NOT_NULL(pathReinforcementPolicy);
    DELETE_IF_NOT_NULL(evaporationPolicy);
    DELETE_IF_NOT_NULL(forwardingPolicy);
}

void AbstractARAClient::setLogger(Logger* logger) {
    this->logger = logger;
}

void AbstractARAClient::logTrace(const std::string &text, ...) const {
    if(logger != nullptr) {
        va_list args;
        va_start(args, text);
        logger->logMessageWithVAList(text, Logger::LEVEL_TRACE, args);
    }
}

void AbstractARAClient::logDebug(const std::string &text, ...) const {
    if(logger != nullptr) {
        va_list args;
        va_start(args, text);
        logger->logMessageWithVAList(text, Logger::LEVEL_DEBUG, args);
    }
}

void AbstractARAClient::logInfo(const std::string &text, ...) const {
    if(logger != nullptr) {
        va_list args;
        va_start(args, text);
        logger->logMessageWithVAList(text, Logger::LEVEL_INFO, args);
    }
}

void AbstractARAClient::logWarn(const std::string &text, ...) const {
    if(logger != nullptr) {
        va_list args;
        va_start(args, text);
        logger->logMessageWithVAList(text, Logger::LEVEL_WARN, args);
    }
}

void AbstractARAClient::logError(const std::string &text, ...) const {
    if(logger != nullptr) {
        va_list args;
        va_start(args, text);
        logger->logMessageWithVAList(text, Logger::LEVEL_ERROR, args);
    }
}

void AbstractARAClient::logFatal(const std::string &text, ...) const {
    if(logger != nullptr) {
        va_list args;
        va_start(args, text);
        logger->logMessageWithVAList(text, Logger::LEVEL_FATAL, args);
    }
}

void AbstractARAClient::addNetworkInterface(NetworkInterface* newInterface) {
    interfaces.push_back(newInterface);
}

NetworkInterface* AbstractARAClient::getNetworkInterface(unsigned int index) {
    return interfaces.at(index);
}

unsigned int AbstractARAClient::getNumberOfNetworkInterfaces() {
    return interfaces.size();
}

PacketFactory* AbstractARAClient::getPacketFactory() const{
    return packetFactory;
}

void AbstractARAClient::sendPacket(Packet* packet) {
    // at first we need to trigger the evaporation (this has no effect if this has been done before in receivePacket(..) )
    routingTable->triggerEvaporation();

    if (packet->getTTL() > 0) {
        AddressPtr destination = packet->getDestination();
        if (isRouteDiscoveryRunning(destination)) {
            logTrace("Route discovery for %s is already running. Trapping packet %u", destination->toString().c_str(), packet->getSequenceNumber());
            packetTrap->trapPacket(packet);
        }
        else if (routingTable->isDeliverable(packet)) {
            NextHop* nextHop = forwardingPolicy->getNextHop(packet, routingTable);
            NetworkInterface* interface = nextHop->getInterface();
            AddressPtr nextHopAddress = nextHop->getAddress();
            packet->setPreviousHop(packet->getSender());
            packet->setSender(interface->getLocalAddress());

            float newPheromoneValue = reinforcePheromoneValue(destination, nextHopAddress, interface);
            logTrace("Forwarding DATA packet %u from %s to %s via %s (phi=%.2f)", packet->getSequenceNumber(), packet->getSourceString().c_str(), packet->getDestinationString().c_str(), nextHopAddress->toString().c_str(), newPheromoneValue);

            interface->send(packet, nextHopAddress);
        } else {
            // packet is not deliverable and no route discovery is yet running
            if(isLocalAddress(packet->getSource())) {
                logDebug("Packet %u from %s to %s is not deliverable. Starting route discovery phase", packet->getSequenceNumber(), packet->getSourceString().c_str(), destination->toString().c_str());
                packetTrap->trapPacket(packet);
                startNewRouteDiscovery(packet);
            }
            else {
                handleNonSourceRouteDiscovery(packet);
            }
        }
    }
    else {
        handlePacketWithZeroTTL(packet);
    }
}

float AbstractARAClient::reinforcePheromoneValue(AddressPtr destination, AddressPtr nextHop, NetworkInterface* interface) {
    float currentPheromoneValue = routingTable->getPheromoneValue(destination, nextHop, interface);
    float newPheromoneValue = pathReinforcementPolicy->calculateReinforcedValue(currentPheromoneValue);
    routingTable->update(destination, nextHop, interface, newPheromoneValue);
    return newPheromoneValue;
}

void AbstractARAClient::startNewRouteDiscovery(Packet* packet) {
    AddressPtr destination = packet->getDestination();
    forgetKnownIntermediateHopsFor(destination);
    startRouteDiscoveryTimer(packet);
    sendFANT(destination);
}

void AbstractARAClient::forgetKnownIntermediateHopsFor(AddressPtr destination) {
    if(knownIntermediateHops.find(destination) != knownIntermediateHops.end()) {
        std::unordered_set<AddressPtr>* seenNodesForThisDestination = knownIntermediateHops[destination];
        seenNodesForThisDestination->clear();
    }
}

void AbstractARAClient::sendFANT(AddressPtr destination) {
    unsigned int sequenceNr = getNextSequenceNumber();

    for(auto& interface: interfaces) {
        Packet* fant = packetFactory->makeFANT(interface->getLocalAddress(), destination, sequenceNr);
        interface->broadcast(fant);
    }
}

void AbstractARAClient::startRouteDiscoveryTimer(const Packet* packet) {
    Timer* timer = Environment::getClock()->getNewTimer();
    timer->addTimeoutListener(this);
    timer->run(routeDiscoveryTimeoutInMilliSeconds * 1000);

    RouteDiscoveryInfo discoveryInfo;
    discoveryInfo.nrOfRetries = 0;
    discoveryInfo.timer = timer;
    discoveryInfo.originalPacket = packet;

    AddressPtr destination = packet->getDestination();
    runningRouteDiscoveries[destination] = timer;
    runningRouteDiscoveryTimers[timer] = discoveryInfo;
}

bool AbstractARAClient::isRouteDiscoveryRunning(AddressPtr destination) {
    return runningRouteDiscoveries.find(destination) != runningRouteDiscoveries.end();
}

void AbstractARAClient::handleNonSourceRouteDiscovery(Packet* packet) {
    logWarn("Dropping packet %u from %s because no route is known (non-source RD disabled)", packet->getSequenceNumber(), packet->getSourceString().c_str());
    broadcastRouteFailure(packet->getDestination());
    delete packet;
}

void AbstractARAClient::handlePacketWithZeroTTL(Packet* packet) {
    logWarn("Dropping packet %u from %s because TTL reached zero", packet->getSequenceNumber(), packet->getSourceString().c_str());
    delete packet;
}

void AbstractARAClient::receivePacket(Packet* packet, NetworkInterface* interface) {
    updateRoutingTable(packet, interface);
    packet->decreaseTTL();

    if(hasBeenReceivedEarlier(packet)) {
        handleDuplicatePacket(packet, interface);
    }
    else {
        registerReceivedPacket(packet);
        handlePacket(packet, interface);
    }
}

void AbstractARAClient::handleDuplicatePacket(Packet* packet, NetworkInterface* interface) {
    if(packet->isDataPacket()) {
        sendDuplicateWarning(packet, interface);
    }
    else if(packet->getType() == PacketType::BANT && isDirectedToThisNode(packet)) {
        logDebug("Another BANT %u came back from %s via %s.", packet->getSequenceNumber(), packet->getSourceString().c_str(), packet->getSenderString().c_str());
    }

    delete packet;
}

void AbstractARAClient::sendDuplicateWarning(Packet* packet, NetworkInterface* interface) {
    AddressPtr sender = interface->getLocalAddress();
    logWarn("Routing loop for packet %u from %s detected. Sending duplicate warning back to %s", packet->getSequenceNumber(), packet->getSourceString().c_str(), packet->getSenderString().c_str());
    Packet* duplicateWarningPacket = packetFactory->makeDulicateWarningPacket(packet, sender, getNextSequenceNumber());
    interface->send(duplicateWarningPacket, packet->getSender());
}

void AbstractARAClient::updateRoutingTable(Packet* packet, NetworkInterface* interface) {
    // we do not want to send/reinforce routes that would send the packet back over ourselves
    if (isLocalAddress(packet->getPreviousHop()) == false) {
        // trigger the evaporation first so this does not effect the new route or update
        routingTable->triggerEvaporation();

        AddressPtr source = packet->getSource();
        AddressPtr sender = packet->getSender();
        if (routingTable->isNewRoute(source, sender, interface)) {
            createNewRouteFrom(packet, interface);
        }
        else {
            reinforcePheromoneValue(source, sender, interface);
        }

    }

}

void AbstractARAClient::createNewRouteFrom(Packet* packet, NetworkInterface* interface) {
    if(hasPreviousNodeBeenSeenBefore(packet) == false) {
        float initialPheromoneValue = calculateInitialPheromoneValue(packet->getTTL());
        routingTable->update(packet->getSource(), packet->getSender(), interface, initialPheromoneValue);
        logTrace("Created new route to %s via %s (phi=%.2f)", packet->getSourceString().c_str(), packet->getSenderString().c_str(), initialPheromoneValue);
    }
    else {
        logTrace("Did not create new route to %s via %s (prevHop %s or sender has been seen before)", packet->getSourceString().c_str(), packet->getSenderString().c_str(), packet->getPreviousHop()->toString().c_str());
    }
}

bool AbstractARAClient::hasPreviousNodeBeenSeenBefore(const Packet* packet) {
    KnownIntermediateHopsMap::const_iterator found = knownIntermediateHops.find(packet->getSource());
    if(found == knownIntermediateHops.end()) {
        // we have never seen any packet for this source address so we can not have seen this previous node before
        return false;
    }

    unordered_set<AddressPtr>* listOfKnownNodes = found->second;

    // have we seen this sender, or the previous hop before?
    bool senderHasBeenSeen = listOfKnownNodes->find(packet->getSender()) != listOfKnownNodes->end();
    bool prevHopHasBeenSeen = listOfKnownNodes->find(packet->getPreviousHop()) != listOfKnownNodes->end();

    return senderHasBeenSeen || prevHopHasBeenSeen;
}

void AbstractARAClient::handlePacket(Packet* packet, NetworkInterface* interface) {
    if (packet->isDataPacket()) {
        handleDataPacket(packet);
    }
    else if(packet->isAntPacket()) {
        handleAntPacket(packet);
    }
    else if (packet->getType() == PacketType::DUPLICATE_ERROR) {
        handleDuplicateErrorPacket(packet, interface);
    }
    else if (packet->getType() == PacketType::ROUTE_FAILURE) {
        handleRouteFailurePacket(packet, interface);
    }
    else {
        throw Exception("Can not handle packet");
    }
}

void AbstractARAClient::handleDataPacket(Packet* packet) {
    if(isDirectedToThisNode(packet)) {
        deliverToSystem(packet);
    }
    else {
        sendPacket(packet);
    }
}

void AbstractARAClient::handleAntPacket(Packet* packet) {
    if (hasBeenSentByThisNode(packet)) {
        // do not process ant packets we have sent ourselves
        delete packet;
        return;
    }

    if (isDirectedToThisNode(packet)) {
        handleAntPacketForThisNode(packet);
    }
    else if (packet->getTTL() > 0) {
        logTrace("Broadcasting %s %u from %s (via %s)", PacketType::getAsString(packet->getType()).c_str(), packet->getSequenceNumber(), packet->getSourceString().c_str(), packet->getSenderString().c_str());
        broadCast(packet);
    }
    else {
        // do not broadcast this ANT packet any further (TTL = 0)
        delete packet;
    }
}

void AbstractARAClient::handleAntPacketForThisNode(Packet* packet) {
    char packetType = packet->getType();

    if(packetType == PacketType::FANT) {
        logDebug("FANT %u from %s reached its destination. Broadcasting BANT", packet->getSequenceNumber(), packet->getSourceString().c_str());
        Packet* bant = packetFactory->makeBANT(packet, getNextSequenceNumber());
        broadCast(bant);
    }
    else if(packetType == PacketType::BANT) {
        handleBANTForThisNode(packet);
    }
    else {
        logError("Can not handle ANT packet %u from %s (unknown type %u)", packet->getSequenceNumber(), packet->getSourceString().c_str(), packetType);
    }

    delete packet;
}

void AbstractARAClient::handleBANTForThisNode(Packet* bant) {
    AddressPtr routeDiscoveryDestination = bant->getSource();
    if(packetTrap->getNumberOfTrappedPackets(routeDiscoveryDestination) == 0) {
        logWarn("Received BANT %u from %s via %s but there are no trapped packets for this destination.", bant->getSequenceNumber(), bant->getSourceString().c_str(), bant->getSenderString().c_str());
    }
    else {
        logDebug("First BANT %u came back from %s via %s. Waiting %ums until delivering the trapped packets", bant->getSequenceNumber(), bant->getSourceString().c_str(), bant->getSenderString().c_str(), packetDeliveryDelayInMilliSeconds);
        stopRouteDiscoveryTimer(routeDiscoveryDestination);
        startDeliveryTimer(routeDiscoveryDestination);
    }
}

void AbstractARAClient::stopRouteDiscoveryTimer(AddressPtr destination) {
    unordered_map<AddressPtr, Timer*, AddressHash, AddressPredicate>::const_iterator discovery;
    discovery = runningRouteDiscoveries.find(destination);

    if(discovery != runningRouteDiscoveries.end()) {
        Timer* timer = discovery->second;
        timer->interrupt();
        // the route discovery is not completely finished until the delivery timer expired.
        // only then is runningRouteDiscoveries.erase(discovery) called!
        runningRouteDiscoveryTimers.erase(timer);
        delete timer;
    }
    else {
        logError("Could not stop route discovery timer (not found for destination %s)", destination->toString().c_str());
    }
}

void AbstractARAClient::startDeliveryTimer(AddressPtr destination) {
    Timer* timer = Environment::getClock()->getNewTimer();
    timer->addTimeoutListener(this);
    timer->run(packetDeliveryDelayInMilliSeconds * 1000);
    runningDeliveryTimers[timer] = destination;
}

void AbstractARAClient::sendDeliverablePackets(AddressPtr destination) {
    PacketQueue deliverablePackets = packetTrap->untrapDeliverablePackets(destination);
    logDebug("Sending %u trapped packet(s) for destination %s", deliverablePackets.size(), destination->toString().c_str());

    for(auto& deliverablePacket : deliverablePackets) {
        sendPacket(deliverablePacket);
    }
}

void AbstractARAClient::handleDuplicateErrorPacket(Packet* duplicateErrorPacket, NetworkInterface* interface) {
    logInfo("Received DUPLICATE_ERROR from %s. Deleting route to %s via %s", duplicateErrorPacket->getSourceString().c_str(), duplicateErrorPacket->getDestinationString().c_str(), duplicateErrorPacket->getSenderString().c_str());
    deleteRoutingTableEntry(duplicateErrorPacket->getDestination(), duplicateErrorPacket->getSender(), interface);
    delete duplicateErrorPacket;
}

bool AbstractARAClient::isDirectedToThisNode(const Packet* packet) const {
    return isLocalAddress(packet->getDestination());
}

bool AbstractARAClient::isLocalAddress(AddressPtr address) const {
    for(auto& interface: interfaces) {
        if(interface->getLocalAddress()->equals(address)) {
            return true;
        }
    }
    return false;
}

bool AbstractARAClient::hasBeenSentByThisNode(const Packet* packet) const {
    return isLocalAddress(packet->getSource());
}

void AbstractARAClient::broadCast(Packet* packet) {
    for(auto& interface: interfaces) {
        Packet* packetClone = packetFactory->makeClone(packet);
        packetClone->setPreviousHop(packet->getSender());
        packetClone->setSender(interface->getLocalAddress());
        interface->broadcast(packetClone);
    }
    delete packet;
}

unsigned int AbstractARAClient::getNextSequenceNumber() {
    return nextSequenceNumber++;
}

bool AbstractARAClient::hasBeenReceivedEarlier(const Packet* packet) {
    AddressPtr source = packet->getSource();
    unsigned int sequenceNumber = packet->getSequenceNumber();

    LastReceivedPacketsMap::const_iterator receivedPacketSeqNumbersFromSource = lastReceivedPackets.find(source);
    if(receivedPacketSeqNumbersFromSource != lastReceivedPackets.end()) {
        unordered_set<unsigned int>* sequenceNumbers = receivedPacketSeqNumbersFromSource->second;
        unordered_set<unsigned int>::const_iterator got = sequenceNumbers->find(sequenceNumber);
        if(got != sequenceNumbers->end()) {
            return true;
        }
    }
    return false;
}

void AbstractARAClient::registerReceivedPacket(const Packet* packet) {
    AddressPtr source = packet->getSource();
    AddressPtr sender = packet->getSender();
    AddressPtr previousHop = packet->getPreviousHop();

    // first check the lastReceived sequence numbers for this source
    LastReceivedPacketsMap::const_iterator foundPacketSeqNumbersFromSource = lastReceivedPackets.find(source);
    unordered_set<unsigned int>* listOfSequenceNumbers;
    if(foundPacketSeqNumbersFromSource == lastReceivedPackets.end()) {
        // There is no record of any received packet from this source address ~> create new
        listOfSequenceNumbers = new unordered_set<unsigned int>();
        listOfSequenceNumbers->insert(packet->getSequenceNumber());
        lastReceivedPackets[source] = listOfSequenceNumbers;
    }
    else {
        listOfSequenceNumbers = foundPacketSeqNumbersFromSource->second;
        listOfSequenceNumbers->insert(packet->getSequenceNumber());
    }

    // now check the known intermediate hops for this destination
    KnownIntermediateHopsMap::const_iterator foundIntermediateHopsForSource = knownIntermediateHops.find(source);
    unordered_set<AddressPtr>* listOfKnownIntermediateNodes;
    if(foundIntermediateHopsForSource == knownIntermediateHops.end()) {
        // There is no record of any known intermediate node for this source address ~> create new
        listOfKnownIntermediateNodes = new unordered_set<AddressPtr>();
        listOfKnownIntermediateNodes->insert(packet->getSender());
        if(previousHop->equals(sender) == false) {
            listOfKnownIntermediateNodes->insert(previousHop);
        }
        knownIntermediateHops[source] = listOfKnownIntermediateNodes;
    }
    else {
        listOfKnownIntermediateNodes = foundIntermediateHopsForSource->second;
        listOfKnownIntermediateNodes->insert(sender);
        if(previousHop->equals(sender) == false) {
            listOfKnownIntermediateNodes->insert(previousHop);
        }
    }
}

float AbstractARAClient::calculateInitialPheromoneValue(unsigned int ttl) {
    int alpha = 1; // may change in the future implementations
    return alpha * ttl + initialPheromoneValue;
}

void AbstractARAClient::setRoutingTable(RoutingTable* newRoutingTable){
    packetTrap->setRoutingTable(newRoutingTable);

    // delete old routing table
    delete routingTable;

    // set new routing table
    routingTable = newRoutingTable;
}

void AbstractARAClient::setMaxNrOfRouteDiscoveryRetries(int maxNrOfRouteDiscoveryRetries) {
    this->maxNrOfRouteDiscoveryRetries = maxNrOfRouteDiscoveryRetries;
}

void AbstractARAClient::timerHasExpired(Timer* responsibleTimer) {
    DiscoveryTimerInfo::iterator discoveryTimerInfo = runningRouteDiscoveryTimers.find(responsibleTimer);
    if(discoveryTimerInfo != runningRouteDiscoveryTimers.end()) {
        handleExpiredRouteDiscoveryTimer(responsibleTimer, discoveryTimerInfo->second);
    }
    else {
        DeliveryTimerInfo::iterator deliveryTimerInfo = runningDeliveryTimers.find(responsibleTimer);
        if(deliveryTimerInfo != runningDeliveryTimers.end()) {
            handleExpiredDeliveryTimer(responsibleTimer, deliveryTimerInfo->second);
        }
        else {
            // if this happens its a bug in our code
            logError("Could not identify expired timer");
        }
    }
}

void AbstractARAClient::handleExpiredRouteDiscoveryTimer(Timer* routeDiscoveryTimer, RouteDiscoveryInfo discoveryInfo) {
    AddressPtr destination = discoveryInfo.originalPacket->getDestination();
    const char* destinationString = destination->toString().c_str();
    logInfo("Route discovery for destination %s timed out", destinationString);

    if(discoveryInfo.nrOfRetries < maxNrOfRouteDiscoveryRetries) {
        // restart the route discovery
        discoveryInfo.nrOfRetries++;
        logInfo("Restarting discovery for destination %s (%u/%u)", destinationString, discoveryInfo.nrOfRetries, maxNrOfRouteDiscoveryRetries);
        runningRouteDiscoveryTimers[routeDiscoveryTimer] = discoveryInfo;

        forgetKnownIntermediateHopsFor(destination);
        sendFANT(destination);
        routeDiscoveryTimer->run(routeDiscoveryTimeoutInMilliSeconds * 1000);
    }
    else {
        // delete the route discovery timer
        runningRouteDiscoveries.erase(destination);
        runningRouteDiscoveryTimers.erase(routeDiscoveryTimer);
        delete routeDiscoveryTimer;

        forgetKnownIntermediateHopsFor(destination);
        deque<Packet*> undeliverablePackets = packetTrap->removePacketsForDestination(destination);
        logWarn("Route discovery for destination %s unsuccessful. Dropping %u packet(s)", destinationString, undeliverablePackets.size());
        for(auto& packet: undeliverablePackets) {
            packetNotDeliverable(packet);
        }
    }
}

void AbstractARAClient::handleExpiredDeliveryTimer(Timer* deliveryTimer, AddressPtr destination) {
    unordered_map<AddressPtr, Timer*, AddressHash, AddressPredicate>::const_iterator discovery;
    discovery = runningRouteDiscoveries.find(destination);

    if(discovery != runningRouteDiscoveries.end()) {
        // its important to delete the discovery info first or else the client will always think the route discovery is still running and never send any packets
        runningRouteDiscoveries.erase(discovery);
        runningDeliveryTimers.erase(deliveryTimer);
        delete deliveryTimer;

        sendDeliverablePackets(destination);
    }
    else {
        logError("Could not find running route discovery object for destination %s)", destination->toString().c_str());
    }

}

void AbstractARAClient::handleBrokenLink(Packet* packet, AddressPtr nextHop, NetworkInterface* interface) {
    logInfo("Link over %s is broken", nextHop->toString().c_str());

    // delete all known routes via this next hop
    std::deque<RoutingTableEntryTupel> allRoutesOverNextHop = routingTable->getAllRoutesThatLeadOver(nextHop);
    for (auto& route: allRoutesOverNextHop) {
        deleteRoutingTableEntry(route.destination, nextHop, route.entry->getNetworkInterface());
    }

    // Try to deliver the packet on an alternative route
    if (routingTable->isDeliverable(packet)) {
        logDebug("Sending %u from %s over alternative route", packet->getSequenceNumber(), packet->getSourceString().c_str());
        sendPacket(packet);
    }
    else if(isLocalAddress(packet->getSource())) {
        packetTrap->trapPacket(packet);

        if (isRouteDiscoveryRunning(packet->getDestination())) {
            logDebug("No alternative route is available. Trapping packet %u from %s because route discovery is already running for destination %s.", packet->getSequenceNumber(), packet->getSourceString().c_str(), packet->getDestinationString().c_str());
        }
        else {
            logDebug("No alternative route is available. Starting new route discovery for packet %u from %s.", packet->getSequenceNumber(), packet->getSourceString().c_str());
            startNewRouteDiscovery(packet);
        }
    }
    else {
        delete packet;
    }
}

void AbstractARAClient::handleRouteFailurePacket(Packet* packet, NetworkInterface* interface) {
    AddressPtr destination = packet->getDestination();
    AddressPtr nextHop = packet->getSender();

    if (routingTable->exists(destination, nextHop, interface)) {
        logInfo("Received ROUTE_FAILURE from %s. Deleting route to %s via %s", packet->getSourceString().c_str(), packet->getDestinationString().c_str(), packet->getSenderString().c_str());
        deleteRoutingTableEntry(destination, nextHop, interface);
    }

    delete packet;
}

void AbstractARAClient::deleteRoutingTableEntry(AddressPtr destination, AddressPtr nextHop, NetworkInterface* interface) {
    if(routingTable->exists(destination, nextHop, interface)) {
        routingTable->removeEntry(destination, nextHop, interface);

        deque<RoutingTableEntry*> possibleNextHops = routingTable->getPossibleNextHops(destination);
        if (possibleNextHops.size() == 1) {
            logDebug("Only one last route is known to %s. Notifying last remaining neighbor with ROUTE_FAILURE packet", destination->toString().c_str());
            AddressPtr source = interface->getLocalAddress();
            unsigned int sequenceNr = getNextSequenceNumber();
            Packet* routeFailurePacket = packetFactory->makeRouteFailurePacket(source, destination, sequenceNr);
            RoutingTableEntry* lastRemainingRoute = possibleNextHops.front();
            lastRemainingRoute->getNetworkInterface()->send(routeFailurePacket, lastRemainingRoute->getAddress());
        }
        else if (possibleNextHops.empty()) {
            logInfo("All known routes to %s have collapsed. Sending ROUTE_FAILURE packet", destination->toString().c_str());
            broadcastRouteFailure(destination);
        }
    }
}

void AbstractARAClient::broadcastRouteFailure(AddressPtr destination) {
    for(auto& interface: interfaces) {
        AddressPtr source = interface->getLocalAddress();
        unsigned int sequenceNr = getNextSequenceNumber();
        Packet* routeFailurePacket = packetFactory->makeRouteFailurePacket(source, destination, sequenceNr);
        interface->broadcast(routeFailurePacket);
    }
}

ARA_NAMESPACE_END
