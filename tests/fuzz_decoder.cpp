// fuzz_decoder.cpp — libFuzzer entry point for AngelDecoder::decode().
//
// Build with: cmake -DHFT_ENABLE_FUZZING=ON (requires Clang + libFuzzer)
//
// Example run:
//   ./fuzz_decoder tests/corpus/ -max_len=512 -jobs=4 -workers=4
//
// Fuzzing targets:
//   1. Length-0 and length-1 inputs (null-termination edge cases)
//   2. All mode bytes: 0, 1 (LTP), 2 (Quote), 3 (SnapQuote), 4, 255
//   3. Token field (bytes [2..26]): all-9s (overflow), all-0s, mixed
//   4. Timestamp field (bytes [35..42]): INT64_MIN, INT64_MAX, negative
//   5. Exact-boundary lengths (PACKET_SIZE_LTP-1, PACKET_SIZE_LTP, etc.)
//   6. Arbitrary random payloads up to 512 bytes

#include "hft/market_data.hpp"

#include <cstdint>
#include <cstddef>
#include <cstdlib>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define FUZZER_TRAP() __debugbreak()
#else
#define FUZZER_TRAP() __builtin_trap()
#endif

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    hft::MarketEvent event{};

    // The decode function must never crash, trigger UB, or access out of bounds.
    // It returns false for malformed input; we only care that it doesn't crash.
    (void)hft::broker::AngelDecoder::decode(data, size, event, 0);

    // Additional semantic invariants for valid packets:
    // - exchange_type must be a known value (0 = unknown but not a crash)
    // - last_price, best_bid_price, best_ask_price may be any int64 (no overflow check needed here)
    // - instrument_token must be <= 999999999 (parse_token cap at 9 digits)
    if (event.instrument_token > 999999999u) {
        // This should never happen given our 9-digit cap.
        // If it does, record this input as an issue.
        FUZZER_TRAP();
    }

    return 0;
}

#if defined(FUZZ_STANDALONE_DRIVER)
#include <iostream>
#include <fstream>
#include <vector>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: fuzz_decoder <corpus_dir_or_file>\n";
        return 0;
    }

    size_t files_tested = 0;
    for (int i = 1; i < argc; ++i) {
        std::filesystem::path p(argv[i]);
        if (std::filesystem::is_directory(p)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(p)) {
                if (entry.is_regular_file() && entry.path().extension() == ".bin") {
                    std::ifstream f(entry.path(), std::ios::binary | std::ios::ate);
                    if (f) {
                        const auto size = f.tellg();
                        f.seekg(0, std::ios::beg);
                        std::vector<uint8_t> buf(static_cast<size_t>(size));
                        if (size == 0 || f.read(reinterpret_cast<char*>(buf.data()), size)) {
                            LLVMFuzzerTestOneInput(buf.data(), buf.size());
                            ++files_tested;
                        }
                    }
                }
            }
        } else if (std::filesystem::is_regular_file(p)) {
            std::ifstream f(p, std::ios::binary | std::ios::ate);
            if (f) {
                const auto size = f.tellg();
                f.seekg(0, std::ios::beg);
                std::vector<uint8_t> buf(static_cast<size_t>(size));
                if (size == 0 || f.read(reinterpret_cast<char*>(buf.data()), size)) {
                    LLVMFuzzerTestOneInput(buf.data(), buf.size());
                    ++files_tested;
                }
            }
        }
    }

    std::cout << "Fuzz standalone driver executed " << files_tested << " corpus files successfully.\n";
    return 0;
}
#endif
