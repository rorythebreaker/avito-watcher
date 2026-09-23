#include "net/websocket.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstring>

#include "util/strings.h"

namespace net {
namespace {

// Client frames must be masked with four random bytes (RFC 6455, 5.3).
void random_bytes(unsigned char* out, size_t size) {
    HCRYPTPROV provider = 0;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        CryptGenRandom(provider, static_cast<DWORD>(size), out);
        CryptReleaseContext(provider, 0);
        return;
    }
    for (size_t i = 0; i < size; ++i) out[i] = static_cast<unsigned char>(rand() & 0xFF);
}

}  // namespace

WebSocket::~WebSocket() {
    close();
}

bool WebSocket::connect(const std::string& url, int timeout_seconds) {
    close();

    std::string rest = url;
    size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);

    size_t slash = rest.find('/');
    std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);

    std::string host = host_port;
    unsigned short port = 80;
    size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        port = static_cast<unsigned short>(std::atoi(host_port.c_str() + colon + 1));
    }

    if (!socket_.connect(host, port, timeout_seconds)) {
        error_ = socket_.error();
        return false;
    }

    unsigned char nonce[16];
    random_bytes(nonce, sizeof(nonce));
    std::string key = util::base64_encode(nonce, sizeof(nonce));

    std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host_port + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";

    if (!socket_.write(request.data(), static_cast<int>(request.size()))) {
        error_ = "Не удалось отправить запрос на подключение";
        return false;
    }

    // Read until the end of the HTTP response headers.
    std::string header;
    while (header.find("\r\n\r\n") == std::string::npos) {
        char chunk[2048];
        int received = socket_.read(chunk, sizeof(chunk));
        if (received <= 0) {
            error_ = "Браузер закрыл отладочное соединение";
            return false;
        }
        header.append(chunk, received);
        if (header.size() > 64 * 1024) {
            error_ = "Некорректный ответ браузера";
            return false;
        }
    }

    size_t split = header.find("\r\n\r\n");
    std::string status_line = header.substr(0, header.find("\r\n"));
    if (status_line.find(" 101") == std::string::npos) {
        error_ = "Браузер не принял отладочное соединение: " + status_line;
        return false;
    }

    buffer_ = header.substr(split + 4);
    connected_ = true;
    error_.clear();
    return true;
}

bool WebSocket::read_exact(size_t count, std::string& out) {
    while (buffer_.size() < count) {
        char chunk[16384];
        int received = socket_.read(chunk, sizeof(chunk));
        if (received <= 0) {
            error_ = "Браузер закрыл отладочное соединение";
            connected_ = false;
            return false;
        }
        buffer_.append(chunk, received);
    }
    out.assign(buffer_, 0, count);
    buffer_.erase(0, count);
    return true;
}

bool WebSocket::send_text(const std::string& text) {
    if (!connected_) return false;

    std::string frame;
    frame.push_back(static_cast<char>(0x81));  // FIN + text opcode

    size_t size = text.size();
    if (size < 126) {
        frame.push_back(static_cast<char>(0x80 | size));
    } else if (size < 65536) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((size >> 8) & 0xFF));
        frame.push_back(static_cast<char>(size & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((size >> shift) & 0xFF));
        }
    }

    unsigned char mask[4];
    random_bytes(mask, sizeof(mask));
    frame.append(reinterpret_cast<char*>(mask), 4);

    size_t payload_start = frame.size();
    frame.append(text);
    for (size_t i = 0; i < size; ++i) {
        frame[payload_start + i] = static_cast<char>(frame[payload_start + i] ^ mask[i % 4]);
    }

    if (!socket_.write(frame.data(), static_cast<int>(frame.size()))) {
        error_ = "Не удалось отправить команду браузеру";
        connected_ = false;
        return false;
    }
    return true;
}

bool WebSocket::receive_text(std::string& out) {
    out.clear();
    if (!connected_) return false;

    std::string message;
    while (true) {
        std::string head;
        if (!read_exact(2, head)) return false;

        unsigned char first = static_cast<unsigned char>(head[0]);
        unsigned char second = static_cast<unsigned char>(head[1]);
        bool fin = (first & 0x80) != 0;
        int opcode = first & 0x0F;
        bool masked = (second & 0x80) != 0;
        unsigned long long length = second & 0x7F;

        if (length == 126) {
            std::string extended;
            if (!read_exact(2, extended)) return false;
            length = (static_cast<unsigned char>(extended[0]) << 8) |
                     static_cast<unsigned char>(extended[1]);
        } else if (length == 127) {
            std::string extended;
            if (!read_exact(8, extended)) return false;
            length = 0;
            for (int i = 0; i < 8; ++i) {
                length = (length << 8) | static_cast<unsigned char>(extended[i]);
            }
        }

        // A DevTools page can be megabytes; anything past this is not sane.
        if (length > 256ull * 1024 * 1024) {
            error_ = "Слишком большой ответ браузера";
            connected_ = false;
            return false;
        }

        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked) {
            std::string mask_bytes;
            if (!read_exact(4, mask_bytes)) return false;
            std::memcpy(mask, mask_bytes.data(), 4);
        }

        std::string payload;
        if (length > 0 && !read_exact(static_cast<size_t>(length), payload)) return false;
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
            }
        }

        if (opcode == 0x8) {  // close
            connected_ = false;
            error_ = "Браузер разорвал отладочное соединение";
            return false;
        }
        if (opcode == 0x9) {  // ping -> pong
            std::string pong;
            pong.push_back(static_cast<char>(0x8A));
            pong.push_back(static_cast<char>(0x80 | payload.size()));
            unsigned char pong_mask[4];
            random_bytes(pong_mask, sizeof(pong_mask));
            pong.append(reinterpret_cast<char*>(pong_mask), 4);
            size_t start = pong.size();
            pong.append(payload);
            for (size_t i = 0; i < payload.size(); ++i) {
                pong[start + i] = static_cast<char>(pong[start + i] ^ pong_mask[i % 4]);
            }
            socket_.write(pong.data(), static_cast<int>(pong.size()));
            continue;
        }
        if (opcode == 0xA) continue;  // pong, nothing to do

        message.append(payload);
        if (fin) {
            out.swap(message);
            return true;
        }
    }
}

void WebSocket::close() {
    if (connected_) {
        // Best-effort close frame; the browser is going away anyway.
        const unsigned char frame[6] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
        socket_.write(reinterpret_cast<const char*>(frame), sizeof(frame));
    }
    socket_.close();
    buffer_.clear();
    connected_ = false;
}

}  // namespace net
