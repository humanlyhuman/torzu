/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/ir/location_descriptor.h"

#include <fmt/format.h>

namespace Dynarmic::IR {

std::string ToString(const LocationDescriptor& descriptor) {
    return fmt::format("{{{:016x}}}", descriptor.Value());
}

void LocationDescriptor::Serialize(std::vector<uint16_t>& fres) const {
    u64 work_value = value;
    fres.push_back(static_cast<uint16_t>(work_value));
    work_value >>= 16;
    fres.push_back(static_cast<uint16_t>(work_value));
    work_value >>= 16;
    fres.push_back(static_cast<uint16_t>(work_value));
    work_value >>= 16;
    fres.push_back(static_cast<uint16_t>(work_value));
}

LocationDescriptor LocationDescriptor::Deserialize(std::vector<uint16_t>::iterator& it) {
    u64 value = *(it++);
    value |= static_cast<u64>(*(it++)) << 16;
    value |= static_cast<u64>(*(it++)) << 32;
    value |= static_cast<u64>(*(it++)) << 48;
    return LocationDescriptor(value);
}

}  // namespace Dynarmic::IR
