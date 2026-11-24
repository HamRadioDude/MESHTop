// Radio-only mode: the map, My Nodes and the live traffic panel come from your own MeshCore radio
// instead of CoreScope. These run only while CoreScope is switched off, and the model is emptied when
// switching in or out, so nothing your radio knows ever sits beside a CoreScope request (the network
// layer also refuses all CoreScope traffic in this mode: see net.cpp and tools/privacy_check.py).
#pragma once
#include "core/companion.h"
#include "core/model.h"

namespace st {

// Copy the radio's contacts (and the radio itself) into the model's node list. Cheap to call often: it
// only touches the model when the contact list changed (or when forced). Returns true if it did.
bool bridgeContacts(Model &m, Companion &r, bool force = false);
// One packet your radio heard -> a row in the live traffic panel and a streak on the map.
void bridgeHeard(Model &m, Companion &r, const HeardPkt &h);
// My Nodes cards from what your radio knows (when each was last heard); CoreScope's stats stay empty.
void radioOnlyCards(Model &m);
// Forget everything from the other data source (nodes, live traffic, chatter, observers, cards).
void clearForSourceSwitch(Model &m);

}  // namespace st
