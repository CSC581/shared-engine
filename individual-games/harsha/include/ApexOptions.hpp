// Apex Ascent command line: offline, client-server, or peer-to-peer.

#pragma once

#include "Multiplayer.hpp"

struct ApexOptions {
    Multiplayer::Config network;
    bool online = false;
    bool hostAuthority = false;
    bool help = false;
};

void printUsage();

// False on --help (options.help set) or on a bad argument (message printed).
bool parseOptions(int argc, char* argv[], ApexOptions& options);
