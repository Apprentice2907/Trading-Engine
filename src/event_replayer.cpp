#include "hft/event_replayer.hpp"
#include <cstring>
#include <sstream>

namespace hft {

EventReplayer::EventReplayer(size_t buffer_size)
    : buffer_(buffer_size) {}

EventReplayer::~EventReplayer() {
    close();
}

EventReplayer::EventReplayer(EventReplayer&& other) noexcept
    : file_(other.file_),
      header_(other.header_),
      buffer_(std::move(other.buffer_)),
      buffer_pos_(other.buffer_pos_),
      bytes_in_buffer_(other.bytes_in_buffer_),
      events_read_(other.events_read_),
      running_crc32_(other.running_crc32_),
      file_path_(std::move(other.file_path_)) {
    other.file_ = nullptr;
    other.buffer_pos_ = 0;
    other.bytes_in_buffer_ = 0;
    other.events_read_ = 0;
    other.running_crc32_ = 0;
}

EventReplayer& EventReplayer::operator=(EventReplayer&& other) noexcept {
    if (this != &other) {
        close();
        file_ = other.file_;
        header_ = other.header_;
        buffer_ = std::move(other.buffer_);
        buffer_pos_ = other.buffer_pos_;
        bytes_in_buffer_ = other.bytes_in_buffer_;
        events_read_ = other.events_read_;
        running_crc32_ = other.running_crc32_;
        file_path_ = std::move(other.file_path_);

        other.file_ = nullptr;
        other.buffer_pos_ = 0;
        other.bytes_in_buffer_ = 0;
        other.events_read_ = 0;
        other.running_crc32_ = 0;
    }
    return *this;
}

bool EventReplayer::open(const std::string& path, std::string* error_out) {
    close();

    auto set_error = [&](const std::string& msg) {
        if (error_out) *error_out = msg;
        close();
        return false;
    };

#if defined(_MSC_VER)
    if (fopen_s(&file_, path.c_str(), "rb") != 0 || !file_) {
        return set_error("Failed to open file: " + path);
    }
#else
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) {
        return set_error("Failed to open file: " + path);
    }
#endif

    file_path_ = path;

    // Check file length
    if (std::fseek(file_, 0, SEEK_END) != 0) {
        return set_error("Failed to seek file: " + path);
    }
    const long long file_size = std::ftell(file_);
    if (file_size < static_cast<long long>(sizeof(FileHeader))) {
        return set_error("File too small for header (" + std::to_string(file_size) + " bytes)");
    }
    std::fseek(file_, 0, SEEK_SET);

    // Read file header
    if (std::fread(&header_, sizeof(FileHeader), 1, file_) != 1) {
        return set_error("Failed to read header from " + path);
    }

    // 1. Magic number validation
    if (header_.magic != HFT_LOG_MAGIC) {
        std::stringstream ss;
        ss << "Invalid magic number: 0x" << std::hex << header_.magic
           << " (expected 0x" << HFT_LOG_MAGIC << ")";
        return set_error(ss.str());
    }

    // 2. Format version validation
    if (header_.version != HFT_LOG_VERSION) {
        return set_error("Unsupported format version: " + std::to_string(header_.version) +
                         " (expected " + std::to_string(HFT_LOG_VERSION) + ")");
    }

    // 3. Record size validation
    if (header_.record_size != HFT_LOG_RECORD_SIZE) {
        return set_error("Unsupported record size: " + std::to_string(header_.record_size) +
                         " (expected " + std::to_string(HFT_LOG_RECORD_SIZE) + ")");
    }

    // 4. Header size validation
    if (header_.header_size < sizeof(FileHeader)) {
        return set_error("Invalid header size: " + std::to_string(header_.header_size));
    }

    // 5. Header CRC32 validation
    const uint32_t expected_header_crc = compute_header_crc32(header_);
    if (header_.header_crc32 != expected_header_crc) {
        return set_error("Header CRC32 checksum mismatch (corrupted header)");
    }

    // 6. File dimensions validation
    const uint64_t expected_total_size = static_cast<uint64_t>(header_.header_size) +
                                         (header_.event_count * header_.record_size);
    if (static_cast<uint64_t>(file_size) != expected_total_size) {
        return set_error("File size mismatch: actual " + std::to_string(file_size) +
                         " bytes, expected " + std::to_string(expected_total_size) +
                         " bytes (truncated or corrupt data)");
    }

    // Seek to beginning of event payload (offset 64)
    std::fseek(file_, header_.header_size, SEEK_SET);

    buffer_pos_ = 0;
    bytes_in_buffer_ = 0;
    events_read_ = 0;
    running_crc32_ = 0;

    return true;
}

bool EventReplayer::next(OrderEvent& ev) noexcept {
    if (!file_ || events_read_ >= header_.event_count) {
        return false;
    }

    // If buffer cannot supply a full event, refill from disk
    if (buffer_pos_ + sizeof(OrderEvent) > bytes_in_buffer_) {
        if (!fill_buffer()) {
            return false;
        }
    }

    // Reconstruct event from buffer
    std::memcpy(&ev, &buffer_[buffer_pos_], sizeof(OrderEvent));
    buffer_pos_ += sizeof(OrderEvent);
    ++events_read_;

    return true;
}

bool EventReplayer::fill_buffer() {
    if (!file_) {
        return false;
    }

    // Preserve any partial trailing bytes at the beginning of the buffer
    const size_t unread = bytes_in_buffer_ - buffer_pos_;
    if (unread > 0 && buffer_pos_ > 0) {
        std::memmove(buffer_.data(), &buffer_[buffer_pos_], unread);
    }
    buffer_pos_ = 0;
    bytes_in_buffer_ = unread;

    // Read contiguous block from disk
    const size_t space_available = buffer_.size() - bytes_in_buffer_;
    const size_t bytes_read = std::fread(buffer_.data() + bytes_in_buffer_, 1, space_available, file_);

    if (bytes_read > 0) {
        running_crc32_ = crc32(running_crc32_, buffer_.data() + bytes_in_buffer_, bytes_read);
        bytes_in_buffer_ += bytes_read;
    }

    return bytes_in_buffer_ >= sizeof(OrderEvent);
}

bool EventReplayer::validate_full_checksum(std::string* error_out) {
    if (!file_) {
        if (error_out) *error_out = "File is not open";
        return false;
    }

    // If already read to EOF, running_crc32_ contains the full payload CRC32
    if (events_read_ == header_.event_count && (bytes_in_buffer_ == buffer_pos_)) {
        if (running_crc32_ != header_.data_crc32) {
            if (error_out) {
                *error_out = "Payload CRC32 mismatch: computed 0x" + std::to_string(running_crc32_) +
                             ", expected 0x" + std::to_string(header_.data_crc32);
            }
            return false;
        }
        return true;
    }

    // Otherwise, perform independent validation scan across file payload
    const long current_pos = std::ftell(file_);
    std::fseek(file_, header_.header_size, SEEK_SET);

    std::vector<uint8_t> scan_buf(65536);
    uint32_t computed_crc = 0;
    uint64_t total_payload_bytes = header_.event_count * header_.record_size;
    uint64_t bytes_remaining = total_payload_bytes;

    while (bytes_remaining > 0) {
        const size_t to_read = static_cast<size_t>(std::min<uint64_t>(scan_buf.size(), bytes_remaining));
        const size_t r = std::fread(scan_buf.data(), 1, to_read, file_);
        if (r == 0) {
            std::fseek(file_, current_pos, SEEK_SET);
            if (error_out) *error_out = "Premature EOF while validating payload checksum";
            return false;
        }
        computed_crc = crc32(computed_crc, scan_buf.data(), r);
        bytes_remaining -= r;
    }

    // Restore file position
    std::fseek(file_, current_pos, SEEK_SET);

    if (computed_crc != header_.data_crc32) {
        if (error_out) {
            *error_out = "Payload CRC32 mismatch: computed 0x" + std::to_string(computed_crc) +
                         ", expected 0x" + std::to_string(header_.data_crc32);
        }
        return false;
    }

    return true;
}

bool EventReplayer::rewind() {
    if (!file_) {
        return false;
    }

    std::fseek(file_, header_.header_size, SEEK_SET);
    buffer_pos_ = 0;
    bytes_in_buffer_ = 0;
    events_read_ = 0;
    running_crc32_ = 0;
    return true;
}

void EventReplayer::close() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    events_read_ = 0;
    buffer_pos_ = 0;
    bytes_in_buffer_ = 0;
    running_crc32_ = 0;
}

} // namespace hft
