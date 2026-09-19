#pragma once

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace webrtc_vst {

// Each controller owns a private directory; each generated page is immutable.
// Keep a small bounded history so refreshed browser tabs cannot show another
// instance's stream. All paths deleted here were created by this object.
class PrivateQrPages {
public:
    PrivateQrPages() = default;
    PrivateQrPages(const PrivateQrPages&) = delete;
    PrivateQrPages& operator=(const PrivateQrPages&) = delete;
    ~PrivateQrPages() {
        if (!directory_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(directory_, ignored);
        }
    }
    std::string write(const std::string& html) {
        namespace fs = std::filesystem;
        try {
            if (directory_.empty()) {
                std::random_device random;
                for (int attempt = 0; attempt < 16; ++attempt) {
                    const auto candidate = fs::temp_directory_path() /
                        ("webrtc-vst-qr-" + std::to_string(random()) + "-" + std::to_string(random()));
                    if (!fs::create_directory(candidate)) continue;
                    directory_ = candidate;
                    std::error_code permissionError;
                    fs::permissions(directory_, fs::perms::owner_all, fs::perm_options::replace, permissionError);
                    if (permissionError) {
                        std::error_code ignored;
                        fs::remove(directory_, ignored);
                        directory_.clear();
                        return {};
                    }
                    break;
                }
                if (directory_.empty()) return {};
            }
            const auto file = directory_ / ("share-" + std::to_string(++sequence_) + ".html");
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            stream << html;
            stream.close();
            if (!stream) { std::error_code ignored; fs::remove(file, ignored); return {}; }
            pages_.push_back(file);
            if (pages_.size() > 8) {
                std::error_code ignored;
                fs::remove(pages_.front(), ignored);
                pages_.erase(pages_.begin());
            }
            return file.string();
        } catch (...) { return {}; }
    }
private:
    std::filesystem::path directory_;
    std::vector<std::filesystem::path> pages_;
    uint64_t sequence_{0};
};

} // namespace webrtc_vst
