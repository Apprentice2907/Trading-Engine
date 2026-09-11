#pragma once

#include "hft/event.hpp"
#include "hft/binary_format.hpp"

#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>

namespace hft {

/**
 * @brief High-performance buffered binary recorder for OrderEvent streams (.hftlog).
 *
 * Employs a preallocated 64 KB memory buffer to coalesce writes and calculate incremental
 * CRC32 checksums, eliminating dynamic allocations and per-event filesystem syscalls.
 */
class EventRecorder {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 65536; // 64 KB (~2048 events)

    explicit EventRecorder(size_t buffer_size = DEFAULT_BUFFER_SIZE);
    ~EventRecorder();

    // Non-copyable, movable
    EventRecorder(const EventRecorder&) = delete;
    EventRecorder& operator=(const EventRecorder&) = delete;
    EventRecorder(EventRecorder&& other) noexcept;
    EventRecorder& operator=(EventRecorder&& other) noexcept;

    /**
     * @brief Opens a new binary log file for writing.
     * Overwrites existing file at path.
     *
     * @param path File system destination path
     * @return true if opened successfully, false otherwise
     */
    bool open(const std::string& path);

    /**
     * @brief Writes a single OrderEvent into the buffer.
     * Flushes to disk automatically when buffer reaches capacity.
     * Performs ZERO dynamic heap allocations.
     */
    bool write(const OrderEvent& ev) noexcept;

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
    std::vector<uint8_t> buffer_;
    size_t buffer_pos_{0};
    uint64_t events_written_{0};
    uint32_t data_crc32_{0};
    std::string file_path_;
};

} // namespace hft
