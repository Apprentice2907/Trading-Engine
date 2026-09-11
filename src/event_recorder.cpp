#include "hft/event_recorder.hpp"
#include <cstring>

namespace hft {

EventRecorder::EventRecorder(size_t buffer_size)
    : buffer_(buffer_size) {}

EventRecorder::~EventRecorder() {
    close();
}

EventRecorder::EventRecorder(EventRecorder&& other) noexcept
    : file_(other.file_),
      buffer_(std::move(other.buffer_)),
      buffer_pos_(other.buffer_pos_),
      events_written_(other.events_written_),
      data_crc32_(other.data_crc32_),
      file_path_(std::move(other.file_path_)) {
    other.file_ = nullptr;
    other.buffer_pos_ = 0;
    other.events_written_ = 0;
    other.data_crc32_ = 0;
}

EventRecorder& EventRecorder::operator=(EventRecorder&& other) noexcept {
    if (this != &other) {
        close();
        file_ = other.file_;
        buffer_ = std::move(other.buffer_);
        buffer_pos_ = other.buffer_pos_;
        events_written_ = other.events_written_;
        data_crc32_ = other.data_crc32_;
        file_path_ = std::move(other.file_path_);

        other.file_ = nullptr;
        other.buffer_pos_ = 0;
        other.events_written_ = 0;
        other.data_crc32_ = 0;
    }
    return *this;
}

bool EventRecorder::open(const std::string& path) {
    close();

#if defined(_MSC_VER)
    if (fopen_s(&file_, path.c_str(), "wb") != 0 || !file_) {
        file_ = nullptr;
        return false;
    }
#else
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        return false;
    }
#endif

    file_path_ = path;
    events_written_ = 0;
    buffer_pos_ = 0;
    data_crc32_ = 0;

    // Write preliminary 64-byte file header (will be finalized upon close)
    FileHeader placeholder_hdr{};
    placeholder_hdr.header_crc32 = compute_header_crc32(placeholder_hdr);

    if (std::fwrite(&placeholder_hdr, sizeof(FileHeader), 1, file_) != 1) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }

    return true;
}

bool EventRecorder::write(const OrderEvent& ev) noexcept {
    if (!file_) {
        return false;
    }

    // Copy event into preallocated memory buffer
    std::memcpy(&buffer_[buffer_pos_], &ev, sizeof(OrderEvent));
    buffer_pos_ += sizeof(OrderEvent);
    ++events_written_;

    // If buffer cannot fit another event, flush to disk
    if (buffer_pos_ + sizeof(OrderEvent) > buffer_.size()) {
        flush_internal();
    }

    return true;
}

void EventRecorder::flush() {
    flush_internal();
    if (file_) {
        std::fflush(file_);
    }
}

void EventRecorder::flush_internal() {
    if (file_ && buffer_pos_ > 0) {
        data_crc32_ = crc32(data_crc32_, buffer_.data(), buffer_pos_);
        std::fwrite(buffer_.data(), 1, buffer_pos_, file_);
        buffer_pos_ = 0;
    }
}

void EventRecorder::close() {
    if (file_) {
        flush_internal();

        // Finalize header at offset 0
        FileHeader final_hdr{};
        final_hdr.magic = HFT_LOG_MAGIC;
        final_hdr.version = HFT_LOG_VERSION;
        final_hdr.record_size = HFT_LOG_RECORD_SIZE;
        final_hdr.header_size = sizeof(FileHeader);
        final_hdr.header_crc32 = compute_header_crc32(final_hdr);
        final_hdr.event_count = events_written_;
        final_hdr.data_crc32 = data_crc32_;

        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(&final_hdr, sizeof(FileHeader), 1, file_);

        std::fclose(file_);
        file_ = nullptr;
    }
}

} // namespace hft
