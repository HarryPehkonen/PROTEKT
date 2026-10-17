#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>

#include "protekt/sim/replay.hpp"

// The fuzz entry point, shared by the libFuzzer binary and the test suite (which feeds it the
// checked-in corpus plus generated inputs on every gate run -- a target nothing runs is
// decoration). Any input at all must be survived: parse either rejects it or accepts it, and an
// ACCEPTED replay must survive its own round trip.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span bytes{data, size};
    const std::string text(bytes.begin(), bytes.end());
    std::string error;
    const auto replay = protekt::sim::parse(text, &error);
    if (!replay.has_value()) {
        return 0; // a rejection is a perfectly good outcome
    }

    // Round trip: the canonical form of what we just read must read back to the same thing, byte
    // for byte. This is the oracle -- it turns "did not crash" into "did not silently corrupt".
    const std::string canonical = protekt::sim::to_text(*replay);
    std::string error2;
    const auto again = protekt::sim::parse(canonical, &error2);
    if (!again.has_value()) {
        std::cerr << "re-parse of canonical form failed: " << error2 << '\n' << canonical << '\n';
        return 1;
    }
    if (protekt::sim::to_text(*again) != canonical) {
        std::cerr << "canonical form is not stable:\n" << canonical << '\n';
        return 2;
    }
    return 0;
}
