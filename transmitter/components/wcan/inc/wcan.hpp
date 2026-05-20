#pragma once

#if defined(WCAN_TRANSPORT_MULTICAST)
#include "../../wcan_multicast/inc/Transceiver.hpp"
#elif defined(WCAN_TRANSPORT_BROADCAST)
#include "../../wcan_broadcast/inc/Transceiver.hpp"
#else
#error "WCAN transport macro missing: define WCAN_TRANSPORT_BROADCAST or WCAN_TRANSPORT_MULTICAST"
#endif
