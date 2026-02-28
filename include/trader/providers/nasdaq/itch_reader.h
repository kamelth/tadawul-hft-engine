#ifndef TRADER_PROVIDERS_NASDAQ_ITCH_READER_H
#define TRADER_PROVIDERS_NASDAQ_ITCH_READER_H

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include <zlib.h>

namespace Trader {
namespace Providers {
namespace NASDAQ {

/**
 * ITCH File Reader
 *
 * Reads NASDAQ ITCH binary files (compressed or uncompressed).
 * Handles gzip decompression automatically.
 */
class ITCHReader {
public:
    ITCHReader() : gz_file_(nullptr), file_(), buffer_size_(65536), eof_(false) {}

    ~ITCHReader() {
        close();
    }

    /**
     * Open ITCH file (auto-detects gzip compression)
     */
    bool open(const std::string& filename) {
        // Check if file is gzipped (by extension)
        bool is_gzipped = (filename.size() > 3 &&
                          filename.substr(filename.size() - 3) == ".gz");

        if (is_gzipped) {
            // Open gzipped file
            gz_file_ = gzopen(filename.c_str(), "rb");
            if (!gz_file_) {
                return false;
            }
            // Set buffer size for better performance
            gzbuffer(gz_file_, buffer_size_);
        } else {
            // Open regular file
            file_.open(filename, std::ios::binary);
            if (!file_.is_open()) {
                return false;
            }
        }

        eof_ = false;
        return true;
    }

    /**
     * Close the file
     */
    void close() {
        if (gz_file_) {
            gzclose(gz_file_);
            gz_file_ = nullptr;
        }
        if (file_.is_open()) {
            file_.close();
        }
        eof_ = true;
    }

    /**
     * Read next message from file
     * Returns message length (excluding 2-byte length header), or 0 on EOF/error
     */
    size_t read_message(uint8_t* buffer, size_t max_size) {
        if (eof_) {
            return 0;
        }

        // Read 2-byte message length (big-endian)
        uint8_t length_bytes[2];
        if (!read_bytes(length_bytes, 2)) {
            eof_ = true;
            return 0;
        }

        // Parse length
        uint16_t message_length = (static_cast<uint16_t>(length_bytes[0]) << 8) |
                                  static_cast<uint16_t>(length_bytes[1]);

        if (message_length == 0 || message_length > max_size) {
            eof_ = true;
            return 0;
        }

        // Read message body
        if (!read_bytes(buffer, message_length)) {
            eof_ = true;
            return 0;
        }

        return message_length;
    }

    /**
     * Check if end of file reached
     */
    bool eof() const {
        return eof_;
    }

    /**
     * Get current file position (approximate for gzipped files)
     */
    uint64_t tell() {
        if (gz_file_) {
            return gztell(gz_file_);
        } else if (file_.is_open()) {
            return file_.tellg();
        }
        return 0;
    }

    /**
     * Seek to position (not supported for gzipped files)
     */
    bool seek(uint64_t pos) {
        if (gz_file_) {
            // gzseek only supports forward seeks
            return false;
        } else if (file_.is_open()) {
            file_.seekg(pos);
            return file_.good();
        }
        return false;
    }

private:
    gzFile gz_file_;              // Gzipped file handle
    std::ifstream file_;          // Regular file stream
    size_t buffer_size_;          // Read buffer size
    bool eof_;                    // End of file flag

    /**
     * Read raw bytes from file (handles both compressed and uncompressed)
     */
    bool read_bytes(uint8_t* buffer, size_t length) {
        if (gz_file_) {
            // Read from gzipped file
            int bytes_read = gzread(gz_file_, buffer, length);
            if (bytes_read != static_cast<int>(length)) {
                // Check if it's EOF or error
                int gz_error;
                const char* error_msg = gzerror(gz_file_, &gz_error);
                if (gz_error == Z_OK || gz_error == Z_STREAM_END) {
                    eof_ = true;
                }
                return false;
            }
            return true;
        } else if (file_.is_open()) {
            // Read from regular file
            file_.read(reinterpret_cast<char*>(buffer), length);
            if (!file_ || file_.gcount() != static_cast<std::streamsize>(length)) {
                return false;
            }
            return true;
        }
        return false;
    }
};

} // namespace NASDAQ
} // namespace Providers
} // namespace Trader

#endif // TRADER_PROVIDERS_NASDAQ_ITCH_READER_H
