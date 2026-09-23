// Byte streams used by the SMTP client and the DevTools WebSocket.
//
// TcpStream is a thin wrapper over Winsock. TlsStream adds encryption through
// Schannel, the TLS provider built into Windows, so sending mail needs no
// third-party crypto library.
#pragma once

#include <string>
#include <vector>

namespace net {

class Stream {
public:
    virtual ~Stream() = default;
    // Returns the number of bytes read, 0 on a clean close, -1 on error.
    virtual int read(char* buffer, int size) = 0;
    virtual bool write(const char* data, int size) = 0;
    virtual void close() = 0;
    virtual bool good() const = 0;
    const std::string& error() const { return error_; }

protected:
    std::string error_;
};

class TcpStream : public Stream {
public:
    TcpStream() = default;
    ~TcpStream() override;

    bool connect(const std::string& host, unsigned short port, int timeout_seconds);

    int read(char* buffer, int size) override;
    bool write(const char* data, int size) override;
    void close() override;
    bool good() const override { return socket_ != static_cast<unsigned long long>(-1); }

    unsigned long long raw() const { return socket_; }

private:
    unsigned long long socket_ = static_cast<unsigned long long>(-1);
};

class TlsStream : public Stream {
public:
    TlsStream() = default;
    ~TlsStream() override;

    // Wraps an already connected TCP stream in TLS (implicit TLS, port 465) or
    // upgrades a plain connection after STARTTLS.
    bool handshake(TcpStream* transport, const std::string& host);

    int read(char* buffer, int size) override;
    bool write(const char* data, int size) override;
    void close() override;
    bool good() const override { return established_; }

private:
    bool send_all(const char* data, int size);
    bool fill_from_transport();

    TcpStream* transport_ = nullptr;
    bool established_ = false;

    // Opaque Schannel handles, kept as raw storage so the header stays free of
    // <security.h>, which cannot be included without SECURITY_WIN32 defined.
    unsigned long long credentials_[2] = {0, 0};
    unsigned long long context_[2] = {0, 0};
    bool has_credentials_ = false;
    bool has_context_ = false;

    unsigned long header_size_ = 0;
    unsigned long trailer_size_ = 0;
    unsigned long max_message_ = 0;

    std::string incoming_;   // raw bytes from the socket, not yet decrypted
    std::string decrypted_;  // plaintext ready for the caller
};

// Resolves and connects, then performs the TLS handshake. Convenience used by
// the mail client for implicit TLS connections.
bool tcp_connect(TcpStream& stream, const std::string& host, unsigned short port,
                 int timeout_seconds, std::string& error);

}  // namespace net
