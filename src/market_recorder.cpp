#define _CRT_SECURE_NO_WARNINGS

#include "hft/market_recorder.hpp"
#include <cstring>
#include <cassert>

namespace hft {

MarketEventRecorder::MarketEventRecorder(size_t buffer_size)
    : buffer_(buffer_size) {
    assert(buffer_size >= sizeof(MarketEvent) && "Buffer must hold at least one MarketEvent");
}

MarketEventRecorder::~MarketEventRecorder() {
    close();
}

MarketEventRecorder::MarketEventRecorder(MarketEventRecorder&& other) noexcept
    : file_(other.file_),
      path_(std::move(other.path_)),
      buffer_(std::move(other.buffer_)),
      buffer_offset_(other.buffer_offset_),
      events_written_(other.events_written_),
      data_crc32_(other.data_crc32_),
      header_(other.header_) {
    other.file_ = nullptr;
    other.buffer_offset_ = 0;
    other.events_written_ = 0;
    other.data_crc32_ = 0;
}

MarketEventRecorder& MarketEventRecorder::operator=(MarketEventRecorder&& other) noexcept {
    if (this != &other) {
        close();
        file_ = other.file_;
        path_ = std::move(other.path_);
        buffer_ = std::move(other.buffer_);
        buffer_offset_ = other.buffer_offset_;
        events_written_ = other.events_written_;
        data_crc32_ = other.data_crc32_;
        header_ = other.header_;

        other.file_ = nullptr;
        other.buffer_offset_ = 0;
        other.events_written_ = 0;
        other.data_crc32_ = 0;
    }
    return *this;
}

bool MarketEventRecorder::open(const std::string& path) {
    close();

    path_ = path;
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        return false;
    }

    events_written_ = 0;
    data_crc32_ = 0;
    buffer_offset_ = 0;

    // Initialize clean header
    header_ = MarketFileHeader{};
    header_.magic = MKT_LOG_MAGIC;
    header_.version = MKT_LOG_VERSION;
    header_.record_size = MKT_LOG_RECORD_SIZE;
    header_.header_size = sizeof(MarketFileHeader);
    header_.header_crc32 = compute_market_header_crc32(header_);
    header_.event_count = 0;
    header_.data_crc32 = 0;

    // Write preliminary 64-byte header placeholder
    size_t written = std::fwrite(&header_, 1, sizeof(header_), file_);
    if (written != sizeof(header_)) {
        close();
        return false;
    }

    return true;
}

bool MarketEventRecorder::write(const MarketEvent& ev) noexcept {
    if (!file_) {
        return false;
    }

    // Flush buffer if not enough space for one MarketEvent
    if (buffer_offset_ + sizeof(MarketEvent) > buffer_.size()) {
        flush_internal();
    }

    std::memcpy(buffer_.data() + buffer_offset_, &ev, sizeof(MarketEvent));
    buffer_offset_ += sizeof(MarketEvent);

    data_crc32_ = crc32(data_crc32_, &ev, sizeof(MarketEvent));
    ++events_written_;

    return true;
}

void MarketEventRecorder::flush_internal() {
    if (file_ && buffer_offset_ > 0) {
        std::fwrite(buffer_.data(), 1, buffer_offset_, file_);
        buffer_offset_ = 0;
    }
}

void MarketEventRecorder::flush() {
    flush_internal();
    if (file_) {
        std::fflush(file_);
    }
}

void MarketEventRecorder::close() {
    if (!file_) {
        return;
    }

    flush_internal();

    // Finalize header
    header_.event_count = events_written_;
    header_.data_crc32 = data_crc32_;
    header_.header_crc32 = compute_market_header_crc32(header_);

    // Seek back to start of file and write finalized header
    std::fseek(file_, 0, SEEK_SET);
    std::fwrite(&header_, 1, sizeof(header_), file_);
    std::fflush(file_);

    std::fclose(file_);
    file_ = nullptr;
}

} // namespace hft
