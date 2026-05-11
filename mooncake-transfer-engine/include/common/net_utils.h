// Copyright 2025 KVCache.AI
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

#ifndef NET_UTILS_H_
#define NET_UTILS_H_

#include <cstdint>
#include <string>

namespace mooncake {

// Parse "10.100.0.0/16" into (network, mask) in network byte order.
// Returns false on malformed input (bad address or prefix out of [0, 32]).
bool parseIpv4CidrConfig(const std::string& cidr,
                         uint32_t* out_net, uint32_t* out_mask);

// Returns true for link-local IPv6 GIDs (fe80::/64). `gid_raw` must point
// to 16 bytes of GID data.
bool isLinkLocalGid(const uint8_t* gid_raw);

// Returns true if the IPv4 in a v4-mapped GID falls in the configured CIDR.
// When `set` is false, any GID matches.
bool gidMatchesSubnetPref(const uint8_t* gid_raw,
                          uint32_t net, uint32_t mask, bool set);

}  // namespace mooncake

#endif  // NET_UTILS_H_
