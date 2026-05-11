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

#include "common/net_utils.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdlib>
#include <cstring>

namespace mooncake {

bool parseIpv4CidrConfig(const std::string& cidr,
                         uint32_t* out_net, uint32_t* out_mask) {
    auto slash = cidr.find('/');
    if (slash == std::string::npos) return false;
    int prefix_len = std::atoi(cidr.c_str() + slash + 1);
    if (prefix_len < 0 || prefix_len > 32) return false;

    struct in_addr addr;
    if (inet_pton(AF_INET, cidr.substr(0, slash).c_str(), &addr) != 1)
        return false;

    uint32_t mask = (prefix_len == 0)
                        ? 0
                        : htonl(0xffffffffu << (32 - prefix_len));
    *out_net = addr.s_addr & mask;
    *out_mask = mask;
    return true;
}

bool isLinkLocalGid(const uint8_t* gid_raw) {
    uint32_t hi0, hi1;
    std::memcpy(&hi0, gid_raw + 0, sizeof(hi0));
    std::memcpy(&hi1, gid_raw + 4, sizeof(hi1));
    return hi0 == htonl(0xfe800000) && hi1 == 0;
}

bool gidMatchesSubnetPref(const uint8_t* gid_raw,
                          uint32_t net, uint32_t mask, bool set) {
    if (!set) return true;
    uint32_t addr_net;
    std::memcpy(&addr_net, gid_raw + 12, sizeof(addr_net));
    return (addr_net & mask) == net;
}

}  // namespace mooncake
