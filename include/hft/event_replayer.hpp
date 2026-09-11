#pragma once

#include "hft/event.hpp"
#include "hft/binary_format.hpp"

#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>

namespace hft {

/**
 * @brief High-performance buffered binary replayer for .hftlog files.
 *
 * Validates header magic, version, record size, file integrity, and CRC32 checksums.
 * Reconstructs OrderEvents in their exact recorded sequence with zero dynamic heap allocations.
 */
class EventReplayer {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 65536; // 64 KB (~2048 events)

    explicit EventReplayer(size_t buffer_size = DEFAULT_BUFFER_SIZE);
    ~EventReplayer();

    // Non-copyable, movable
    EventReplayer(const EventReplayer&) = delete;
    EventReplayer& operator=(const EventReplayer&) = delete;
    EventReplayer(EventReplayer&& other) noexcept;
    EventReplayer& operator=(EventReplayer&& other) noexcept;

    /**
     * @brief Opens and validates an .hftlog binary file.
     *
     * Validates magic number, version, record size, header size, header CRC32,
     * and file dimensions. Fails cleanly on any corruption without throwing or crashing.
     *
     * @param path Path to .hftlog file
     * @param error_out Optional pointer to string receiving detailed validation failure reason
     * @return true if valid and open, false otherwise
     */
    bool open(const std::string& path, std::string* error_out = nullptr);

    /**
     * @brief Reads the next OrderEvent sequentially from the file stream.
     * Performs ZERO dynamic heap allocations.
     *
     * @param ev Output reference populated with the next OrderEvent
     * @return true if an event was read, false on EOF or read error
     */
    bool next(OrderEvent& ev) noexcept;

    /**
     * @brief Verifies the entire payload's CRC32 checksum against header.data_crc32.
     * Can be called after all events have been read via next(), or independently.
     *
     * @param error_out Optional string receiving mismatch details
     * @return true if CRC32 matches, false if corrupted
     */
    bool validate_full_checksum(std::string* error_out = nullptr);

    /**
     * @brief Resets stream position to the beginning of the event records (offset 64).
     */
    bool rewind();

    /**
     * @brief Closes the open file handle and releases resources.
     */
    void close();

    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] bool eof() const noexcept { return events_read_ >= header_.event_count; }
    [[nodiscard]] const FileHeader& header() const noexcept { return header_; }
    [[nodiscard]] uint64_t events_read() const noexcept { return events_read_; }
    [[nodiscard]] uint32_t running_crc32() const noexcept { return running_crc32_; }

private:
    bool fill_buffer();

    FILE* file_{nullptr};
    FileHeader header_{};
    std::vector<uint8_t> buffer_;
    size_t buffer_pos_{0};
    size_t bytes_in_buffer_{0};
    uint64_t events_read_{0};
    uint32_t running_crc32_{0};
    std::string file_path_;
};

} // namespace hft
