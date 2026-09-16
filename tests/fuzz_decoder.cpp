// fuzz_decoder.cpp — libFuzzer entry point for YahooParser::parse().
//
// Build with: cmake -DHFT_ENABLE_FUZZING=ON (requires Clang + libFuzzer)
//
// Example run:
//   ./fuzz_decoder tests/corpus/ -max_len=1024 -jobs=4 -workers=4
//
// Fuzzing targets:
//   1. Length-0, length-1, and truncated inputs
//   2. Deeply nested JSON, malformed tokens, unclosed strings
//   3. Missing fields (no symbol, no price, no meta)
//   4. Numerical edge cases (NaN, infinity, overflow, negative, zero)
//   5. Semantic invariants: zero-bid/ask guarantee, non-zero hash

#include "hft/market_data.hpp"

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <string_view>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define FUZZER_TRAP() __debugbreak()
#else
#define FUZZER_TRAP() __builtin_trap()
#endif

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    hft::MarketEvent event{};

    std::string_view sv(reinterpret_cast<const char*>(data), size);
    bool ok = hft::YahooParser::parse(sv, event, 0);

    // Semantic invariants when parse succeeds:
    if (ok) {
        // 1. Yahoo adapter must NEVER fabricate bid/ask or quote quantity
        if (event.best_bid_price != 0 || event.best_ask_price != 0 ||
            event.best_bid_quantity != 0 || event.best_ask_quantity != 0 ||
            event.last_quantity != 0) {
            FUZZER_TRAP();
        }

        // 2. Token must be valid non-zero hash
        if (event.instrument_token == 0) {
            FUZZER_TRAP();
        }

        // 3. Price must be strictly positive
        if (event.last_price <= 0) {
            FUZZER_TRAP();
        }
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
                if (entry.is_regular_file() && (entry.path().extension() == ".bin" || entry.path().extension() == ".json")) {
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
