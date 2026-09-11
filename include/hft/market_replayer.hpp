#pragma once

#include "hft/market_event.hpp"
#include "hft/market_format.hpp"

#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>

namespace hft {

/**
 * @brief High-performance buffered binary replayer for .mktlog market data journals.
 *
 * Validates header magic, version, record size, file integrity, and CRC32 checksums.
 * Streams MarketEvents in their exact recorded sequence with zero dynamic heap allocations.
 */
class MarketEventReplayer {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 65536; // 64 KB (~1024 MarketEvents)

    explicit MarketEventReplayer(size_t buffer_size = DEFAULT_BUFFER_SIZE);
    ~MarketEventReplayer();

    // Non-copyable, movable
    MarketEventReplayer(const MarketEventReplayer&) = delete;
    MarketEventReplayer& operator=(const MarketEventReplayer&) = delete;
    MarketEventReplayer(MarketEventReplayer&& other) noexcept;
    MarketEventReplayer& operator=(MarketEventReplayer&& other) noexcept;

    /**
     * @brief Opens and validates an .mktlog binary file.
     *
     * @param path Path to .mktlog file
     * @param error_out Optional string receiving validation failure reason
     * @return true if valid and open, false otherwise
     */
    bool open(const std::string& path, std::string* error_out = nullptr);

    /**
     * @brief Reads the next MarketEvent sequentially from the stream.
     * Performs ZERO dynamic heap allocations.
     *
     * @param ev Output reference populated with next MarketEvent
     * @return true if event was read, false on EOF or error
     */
    bool next(MarketEvent& ev) noexcept;

    /**
     * @brief Verifies the entire payload CRC32 checksum against header.data_crc32.
     */
    bool validate_full_checksum(std::string* error_out = nullptr);

    void close();

    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] const MarketFileHeader& header() const noexcept { return header_; }
    [[nodiscard]] uint64_t events_read() const noexcept { return events_read_; }

private:
    FILE* file_{nullptr};
    std::string path_;
    std::vector<uint8_t> buffer_;
    size_t buffer_offset_{0};
    size_t buffer_valid_bytes_{0};
    uint64_t events_read_{0};
    MarketFileHeader header_{};
};

} // namespace hft
