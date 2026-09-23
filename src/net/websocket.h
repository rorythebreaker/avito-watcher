// Just enough of RFC 6455 to talk to Chrome DevTools over ws://127.0.0.1.
//
// The connection is always to the local browser, so there is no TLS here and no
// support for extensions or compression - only masked client text frames and
// unmasked server frames.
#pragma once

#include <string>

#include "net/tls.h"

namespace net {

class WebSocket {
public:
    WebSocket() = default;
    ~WebSocket();

    // url looks like "ws://127.0.0.1:1234/devtools/page/ABC".
    bool connect(const std::string& url, int timeout_seconds);

    bool send_text(const std::string& text);
    // Blocks until a whole text message arrives, reassembling continuation
    // frames and answering pings along the way.
    bool receive_text(std::string& out);

    void close();
    bool good() const { return connected_; }
    const std::string& error() const { return error_; }

private:
    bool read_exact(size_t count, std::string& out);

    TcpStream socket_;
    std::string buffer_;   // bytes read from the socket but not consumed yet
    bool connected_ = false;
    std::string error_;
};

}  // namespace net
