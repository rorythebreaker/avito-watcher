#include "ui/icon.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "ui/theme.h"

namespace ui {
namespace {

struct Pixel {
    unsigned char blue, green, red, alpha;
};

// Blends a colour onto a pixel with the given coverage, 0..1.
void blend(Pixel& pixel, COLORREF color, double coverage) {
    if (coverage <= 0.0) return;
    if (coverage > 1.0) coverage = 1.0;
    double source_alpha = coverage;
    double destination_alpha = pixel.alpha / 255.0;
    double out_alpha = source_alpha + destination_alpha * (1.0 - source_alpha);

    auto mix = [&](unsigned char destination, int source) {
        double value = (source * source_alpha +
                        destination * destination_alpha * (1.0 - source_alpha));
        return static_cast<unsigned char>(out_alpha > 0 ? value / out_alpha : 0);
    };

    pixel.red = mix(pixel.red, GetRValue(color));
    pixel.green = mix(pixel.green, GetGValue(color));
    pixel.blue = mix(pixel.blue, GetBValue(color));
    pixel.alpha = static_cast<unsigned char>(out_alpha * 255.0 + 0.5);
}

double clamp01(double value) {
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}

// Coverage of a rounded square, computed with a signed distance so the edges
// come out smooth without any antialiasing support from GDI.
double rounded_square_coverage(double x, double y, double left, double top,
                               double right, double bottom, double radius) {
    double center_x = (left + right) / 2.0;
    double center_y = (top + bottom) / 2.0;
    double half_width = (right - left) / 2.0 - radius;
    double half_height = (bottom - top) / 2.0 - radius;

    double dx = std::abs(x - center_x) - half_width;
    double dy = std::abs(y - center_y) - half_height;
    double outside = std::hypot(std::max(dx, 0.0), std::max(dy, 0.0));
    double distance = outside + std::min(std::max(dx, dy), 0.0) - radius;
    return clamp01(0.5 - distance);
}

// Coverage of a ring of the given thickness.
double ring_coverage(double x, double y, double center_x, double center_y, double radius,
                     double thickness) {
    double distance = std::hypot(x - center_x, y - center_y) - radius;
    return clamp01(0.5 + thickness / 2.0 - std::abs(distance));
}

// Coverage of a thick line segment.
double segment_coverage(double x, double y, double x1, double y1, double x2, double y2,
                        double thickness) {
    double dx = x2 - x1;
    double dy = y2 - y1;
    double length_squared = dx * dx + dy * dy;
    double t = length_squared > 0 ? ((x - x1) * dx + (y - y1) * dy) / length_squared : 0.0;
    t = clamp01(t);
    double distance = std::hypot(x - (x1 + t * dx), y - (y1 + t * dy));
    return clamp01(0.5 + thickness / 2.0 - distance);
}

double disc_coverage(double x, double y, double center_x, double center_y, double radius) {
    return clamp01(0.5 + radius - std::hypot(x - center_x, y - center_y));
}

HICON render(int size, bool alert) {
    std::vector<Pixel> pixels(static_cast<size_t>(size) * size, Pixel{0, 0, 0, 0});
    const double s = size;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            Pixel& pixel = pixels[static_cast<size_t>(y) * size + x];
            double px = x + 0.5;
            double py = y + 0.5;

            // Body: a rounded square with a diagonal gradient.
            double body = rounded_square_coverage(px, py, s * 0.04, s * 0.04, s * 0.96,
                                                  s * 0.96, s * 0.24);
            if (body > 0.0) {
                double ratio = clamp01((px + py) / (2.0 * s));
                COLORREF top = color::kAccent;
                COLORREF bottom = color::kAccentDark;
                COLORREF mixed = RGB(
                    static_cast<int>(GetRValue(top) + (GetRValue(bottom) - GetRValue(top)) * ratio),
                    static_cast<int>(GetGValue(top) + (GetGValue(bottom) - GetGValue(top)) * ratio),
                    static_cast<int>(GetBValue(top) + (GetBValue(bottom) - GetBValue(top)) * ratio));
                blend(pixel, mixed, body);
            }

            // Magnifier: the app is looking for things.
            double thickness = s * 0.085;
            double lens_x = s * 0.45;
            double lens_y = s * 0.43;
            double lens_radius = s * 0.21;
            double glass = ring_coverage(px, py, lens_x, lens_y, lens_radius, thickness);
            double handle = segment_coverage(px, py,
                                             lens_x + lens_radius * 0.72,
                                             lens_y + lens_radius * 0.72,
                                             s * 0.76, s * 0.76, thickness);
            blend(pixel, RGB(0xff, 0xff, 0xff), std::max(glass, handle));

            if (alert) {
                double dot_radius = s * 0.17;
                double dot = disc_coverage(px, py, s - dot_radius * 1.05, dot_radius * 1.05,
                                           dot_radius);
                blend(pixel, RGB(0xff, 0x5a, 0x3c), dot);
            }
        }
    }

    // Premultiply for the 32-bit alpha icon format.
    for (Pixel& pixel : pixels) {
        pixel.red = static_cast<unsigned char>(pixel.red * pixel.alpha / 255);
        pixel.green = static_cast<unsigned char>(pixel.green * pixel.alpha / 255);
        pixel.blue = static_cast<unsigned char>(pixel.blue * pixel.alpha / 255);
    }

    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;  // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color_bitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                            DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color_bitmap || !bits) return nullptr;
    std::memcpy(bits, pixels.data(), pixels.size() * sizeof(Pixel));

    HBITMAP mask_bitmap = CreateBitmap(size, size, 1, 1, nullptr);

    ICONINFO info = {};
    info.fIcon = TRUE;
    info.hbmColor = color_bitmap;
    info.hbmMask = mask_bitmap;
    HICON icon = CreateIconIndirect(&info);

    DeleteObject(color_bitmap);
    DeleteObject(mask_bitmap);
    return icon;
}

HICON cached(int size, bool alert) {
    static std::map<std::pair<int, bool>, HICON> cache;
    auto key = std::make_pair(size, alert);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    HICON icon = render(size, alert);
    cache[key] = icon;
    return icon;
}

}  // namespace

HICON app_icon(bool alert) {
    return cached(GetSystemMetrics(SM_CXICON), alert);
}

HICON app_icon_small(bool alert) {
    return cached(GetSystemMetrics(SM_CXSMICON), alert);
}

}  // namespace ui
