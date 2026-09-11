#pragma once

#include "hft/market_event.hpp"
#include "hft/market_format.hpp"

#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>

namespace hft {

/**
 * @brief High-performance buffered binary recorder for MarketEvent streams (.mktlog).
 *
 * Uses a preallocated 64 KB memory buffer to coalesce disk writes and compute incremental
 * CRC32 checksums, eliminating dynamic allocations during streaming writes.
 */
class MarketEventRecorder {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 65536; // 64 KB (1024 MarketEvents)

    explicit MarketEventRecorder(size_t buffer_size = DEFAULT_BUFFER_SIZE);
    ~MarketEventRecorder();

    // Non-copyable, movable
    MarketEventRecorder(const MarketEventRecorder&) = delete;
    MarketEventRecorder& operator=(const MarketEventRecorder&) = delete;
    MarketEventRecorder(MarketEventRecorder&& other) noexcept;
    MarketEventRecorder& operator=(MarketEventRecorder&& other) noexcept;

    /**
     * @brief Opens a new binary market log file for writing.
     * Overwrites any existing file at path.
     *
     * @param path File system destination path
     * @return true if opened successfully, false otherwise
     */
    bool open(const std::string& path);

    /**
     * @brief Writes a single MarketEvent into the staging buffer.
     * Flushes to disk automatically when the buffer is full.
     * Performs ZERO dynamic heap allocations.
     */
    bool write(const MarketEvent& ev) noexcept;

    /**
     * @brief Explicitly flushes buffered events to disk.
     */
    void flush();

    /**
     * @brief Finalizes the file header with total event count and CRC32, then closes file.
     */
    void close();

    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] uint64_t events_written() const noexcept { return events_written_; }
    [[nodiscard]] uint32_t data_crc32() const noexcept { return data_crc32_; }

private:
    void flush_internal();

    FILE* file_{nullptr};
    std::string path_;
    std::vector<uint8_t> buffer_;
    size_t buffer_offset_{0};
    uint64_t events_written_{0};
    uint32_t data_crc32_{0};
    MarketFileHeader header_{};
};

} // namespace hft
