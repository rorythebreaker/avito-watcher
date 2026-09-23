// Listing thumbnails: downloaded in the background, decoded with GDI+ and
// cached both in memory and on disk.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ui {

class ImageLoader {
public:
    // Posts `ready_message` to `window` whenever a new thumbnail is decoded, so
    // the feed can repaint itself. The size is in device pixels, so on a scaled
    // monitor the picture is decoded at full resolution rather than blown up.
    ImageLoader(HWND window, UINT ready_message, int thumb_width, int thumb_height);
    ~ImageLoader();

    ImageLoader(const ImageLoader&) = delete;
    ImageLoader& operator=(const ImageLoader&) = delete;

    // Returns the thumbnail if it is ready, otherwise nullptr and queues a
    // download. The bitmap stays owned by the loader.
    HBITMAP get(const std::string& url);

    int thumb_width() const { return thumb_width_; }
    int thumb_height() const { return thumb_height_; }

    void set_proxy(const std::string& proxy);
    void shutdown();

private:
    void worker();
    HBITMAP decode(const std::string& bytes) const;

    HWND window_ = nullptr;
    UINT ready_message_ = 0;
    int thumb_width_ = 128;
    int thumb_height_ = 96;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::string, HBITMAP> ready_;
    std::deque<std::string> queue_;
    std::vector<std::string> pending_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
    std::string proxy_;
};

}  // namespace ui
