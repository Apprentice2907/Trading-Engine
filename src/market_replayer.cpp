#define _CRT_SECURE_NO_WARNINGS

#include "hft/market_replayer.hpp"
#include <cstring>
#include <cassert>
#include <filesystem>

namespace hft {

MarketEventReplayer::MarketEventReplayer(size_t buffer_size)
    : buffer_(buffer_size) {
    assert(buffer_size >= sizeof(MarketEvent) && "Buffer must hold at least one MarketEvent");
}

MarketEventReplayer::~MarketEventReplayer() {
    close();
}

MarketEventReplayer::MarketEventReplayer(MarketEventReplayer&& other) noexcept
    : file_(other.file_),
      path_(std::move(other.path_)),
      buffer_(std::move(other.buffer_)),
      buffer_offset_(other.buffer_offset_),
      buffer_valid_bytes_(other.buffer_valid_bytes_),
      events_read_(other.events_read_),
      header_(other.header_) {
    other.file_ = nullptr;
    other.buffer_offset_ = 0;
    other.buffer_valid_bytes_ = 0;
    other.events_read_ = 0;
}

MarketEventReplayer& MarketEventReplayer::operator=(MarketEventReplayer&& other) noexcept {
    if (this != &other) {
        close();
        file_ = other.file_;
        path_ = std::move(other.path_);
        buffer_ = std::move(other.buffer_);
        buffer_offset_ = other.buffer_offset_;
        buffer_valid_bytes_ = other.buffer_valid_bytes_;
        events_read_ = other.events_read_;
        header_ = other.header_;

        other.file_ = nullptr;
        other.buffer_offset_ = 0;
        other.buffer_valid_bytes_ = 0;
        other.events_read_ = 0;
    }
    return *this;
}

bool MarketEventReplayer::open(const std::string& path, std::string* error_out) {
    close();

    auto set_err = [error_out](const std::string& msg) {
        if (error_out) *error_out = msg;
        return false;
    };

    std::error_code ec;
    uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        return set_err("File does not exist or cannot be accessed: " + path);
    }

    if (file_size < sizeof(MarketFileHeader)) {
        return set_err("Incomplete file header: file size (" + std::to_string(file_size) +
                       "B) is smaller than header size (64B)");
    }

    path_ = path;
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) {
        return set_err("Failed to open file: " + path);
    }

    size_t read_bytes = std::fread(&header_, 1, sizeof(header_), file_);
    if (read_bytes != sizeof(header_)) {
        close();
        return set_err("Failed to read header from " + path);
    }

    // 1. Magic validation
    if (header_.magic != MKT_LOG_MAGIC) {
        close();
        return set_err("Invalid magic number: expected 0x4C544B4D (MKTL), got 0x" +
                       std::to_string(header_.magic));
    }

    // 2. Version validation
    if (header_.version != MKT_LOG_VERSION) {
        close();
        return set_err("Unsupported format version: expected " + std::to_string(MKT_LOG_VERSION) +
                       ", got " + std::to_string(header_.version));
    }

    // 3. Record & Header size validation
    if (header_.record_size != MKT_LOG_RECORD_SIZE) {
        close();
        return set_err("Record size mismatch: expected " + std::to_string(MKT_LOG_RECORD_SIZE) +
                       ", got " + std::to_string(header_.record_size));
    }
    if (header_.header_size != sizeof(MarketFileHeader)) {
        close();
        return set_err("Header size mismatch: expected 64, got " +
                       std::to_string(header_.header_size));
    }

    // 4. Header CRC32 validation
    uint32_t expected_header_crc = compute_market_header_crc32(header_);
    if (header_.header_crc32 != expected_header_crc) {
        close();
        return set_err("Header CRC32 checksum mismatch: expected 0x" +
                       std::to_string(expected_header_crc) + ", header had 0x" +
                       std::to_string(header_.header_crc32));
    }

    // 5. File size dimension check
    uintmax_t expected_file_size = header_.header_size + (header_.event_count * header_.record_size);
    if (file_size != expected_file_size) {
        close();
        return set_err("File size mismatch: expected " + std::to_string(expected_file_size) +
                       " bytes, found " + std::to_string(file_size) + " bytes (truncated or corrupt)");
    }

    events_read_ = 0;
    buffer_offset_ = 0;
    buffer_valid_bytes_ = 0;

    return true;
}

bool MarketEventReplayer::next(MarketEvent& ev) noexcept {
    if (!file_ || events_read_ >= header_.event_count) {
        return false;
    }

    // Refill buffer if exhausted
    if (buffer_offset_ + sizeof(MarketEvent) > buffer_valid_bytes_) {
        buffer_valid_bytes_ = std::fread(buffer_.data(), 1, buffer_.size(), file_);
        buffer_offset_ = 0;
        if (buffer_valid_bytes_ < sizeof(MarketEvent)) {
            return false;
        }
    }

    std::memcpy(&ev, buffer_.data() + buffer_offset_, sizeof(MarketEvent));
    buffer_offset_ += sizeof(MarketEvent);
    ++events_read_;

    return true;
}

bool MarketEventReplayer::validate_full_checksum(std::string* error_out) {
    auto set_err = [error_out](const std::string& msg) {
        if (error_out) *error_out = msg;
        return false;
    };

    if (!file_) {
        return set_err("Replayer is not open");
    }

    // Save current file position
    long current_pos = std::ftell(file_);
    std::fseek(file_, sizeof(MarketFileHeader), SEEK_SET);

    std::vector<uint8_t> check_buf(buffer_.size());
    uint32_t computed_crc = 0;
    uint64_t bytes_to_read = header_.event_count * sizeof(MarketEvent);
    uint64_t total_read = 0;

    while (total_read < bytes_to_read) {
        size_t chunk = static_cast<size_t>(
            std::min(static_cast<uint64_t>(check_buf.size()), bytes_to_read - total_read));
        size_t r = std::fread(check_buf.data(), 1, chunk, file_);
        if (r == 0) {
            std::fseek(file_, current_pos, SEEK_SET);
            return set_err("Premature EOF encountered during CRC32 verification");
        }
        computed_crc = crc32(computed_crc, check_buf.data(), r);
        total_read += r;
    }

    // Restore file position
    std::fseek(file_, current_pos, SEEK_SET);

    if (computed_crc != header_.data_crc32) {
        return set_err("Payload CRC32 mismatch: expected 0x" +
                       std::to_string(header_.data_crc32) + ", computed 0x" +
                       std::to_string(computed_crc) + " (file data is corrupted)");
    }

    return true;
}

void MarketEventReplayer::close() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    path_.clear();
    buffer_offset_ = 0;
    buffer_valid_bytes_ = 0;
    events_read_ = 0;
    header_ = MarketFileHeader{};
}

} // namespace hft
