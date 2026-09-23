#include "ui/images.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

#include <algorithm>

#include "net/http.h"
#include "util/paths.h"
#include "util/strings.h"

namespace ui {
namespace {

constexpr size_t kMaxMemoryEntries = 600;

std::wstring cache_file_for(const std::string& url) {
    return util::image_cache_dir() + L"\\" + util::widen(util::sha1_hex(url)) + L".img";
}

}  // namespace

ImageLoader::ImageLoader(HWND window, UINT ready_message, int thumb_width,
                         int thumb_height)
    : window_(window),
      ready_message_(ready_message),
      thumb_width_(thumb_width > 0 ? thumb_width : 128),
      thumb_height_(thumb_height > 0 ? thumb_height : 96) {
    for (int i = 0; i < 3; ++i) threads_.emplace_back(&ImageLoader::worker, this);
}

ImageLoader::~ImageLoader() {
    shutdown();
}

void ImageLoader::set_proxy(const std::string& proxy) {
    std::lock_guard<std::mutex> lock(mutex_);
    proxy_ = proxy;
}

void ImageLoader::shutdown() {
    if (stop_.exchange(true)) return;
    wake_.notify_all();
    for (std::thread& thread : threads_) {
        if (thread.joinable()) thread.join();
    }
    threads_.clear();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [url, bitmap] : ready_) {
        if (bitmap) DeleteObject(bitmap);
    }
    ready_.clear();
}

HBITMAP ImageLoader::get(const std::string& url) {
    if (url.empty() || stop_.load()) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ready_.find(url);
    if (it != ready_.end()) return it->second;

    if (std::find(pending_.begin(), pending_.end(), url) == pending_.end()) {
        pending_.push_back(url);
        queue_.push_back(url);
        wake_.notify_one();
    }
    return nullptr;
}

void ImageLoader::worker() {
    while (!stop_.load()) {
        std::string url;
        std::string proxy;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });
            if (stop_.load()) return;
            url = queue_.front();
            queue_.pop_front();
            proxy = proxy_;
        }

        std::string bytes;
        const std::wstring cache_path = cache_file_for(url);
        if (!util::read_file(cache_path, bytes) || bytes.empty()) {
            net::HttpClient client;
            client.set_proxy(proxy);
            client.set_timeout_seconds(20);
            client.set_cookies_enabled(false);
            net::Response response = client.get(url);
            if (response.ok && response.status == 200 && !response.body.empty()) {
                bytes = std::move(response.body);
                util::write_file(cache_path, bytes.data(), bytes.size());
            }
        }

        HBITMAP bitmap = bytes.empty() ? nullptr : decode(bytes);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.erase(std::remove(pending_.begin(), pending_.end(), url), pending_.end());
            if (bitmap) {
                if (ready_.size() > kMaxMemoryEntries) {
                    for (auto& [key, value] : ready_) {
                        if (value) DeleteObject(value);
                    }
                    ready_.clear();
                }
                ready_[url] = bitmap;
            }
        }

        if (bitmap && window_ && !stop_.load()) {
            PostMessageW(window_, ready_message_, 0, 0);
        }
    }
}

HBITMAP ImageLoader::decode(const std::string& bytes) const {
    IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()),
                                        static_cast<UINT>(bytes.size()));
    if (!stream) return nullptr;

    HBITMAP result = nullptr;
    {
        Gdiplus::Bitmap source(stream, FALSE);
        if (source.GetLastStatus() == Gdiplus::Ok && source.GetWidth() > 0 &&
            source.GetHeight() > 0) {
            HDC screen = GetDC(nullptr);
            HDC memory = CreateCompatibleDC(screen);

            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = thumb_width_;
            info.bmiHeader.biHeight = -thumb_height_;  // top-down
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bitmap) {
                HGDIOBJ old = SelectObject(memory, bitmap);
                {
                    Gdiplus::Graphics graphics(memory);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                    graphics.Clear(Gdiplus::Color(255, 42, 47, 54));

                    // Cover the whole thumbnail, cropping the overflow evenly.
                    double factor = std::max(
                        static_cast<double>(thumb_width_) / source.GetWidth(),
                        static_cast<double>(thumb_height_) / source.GetHeight());
                    int width = static_cast<int>(source.GetWidth() * factor + 0.5);
                    int height = static_cast<int>(source.GetHeight() * factor + 0.5);
                    int x = (thumb_width_ - width) / 2;
                    int y = (thumb_height_ - height) / 2;
                    graphics.DrawImage(&source, x, y, width, height);
                }
                SelectObject(memory, old);
                result = bitmap;
            }
            DeleteDC(memory);
            ReleaseDC(nullptr, screen);
        }
    }
    stream->Release();
    return result;
}

}  // namespace ui
