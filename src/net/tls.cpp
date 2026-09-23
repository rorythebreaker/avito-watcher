#include "net/tls.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <sspi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "util/strings.h"

namespace net {
namespace {

constexpr int kMaxTlsRecord = 32 * 1024;

void ensure_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

std::string socket_error(const std::string& stage) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), " (ошибка сети %d)", WSAGetLastError());
    return stage + buffer;
}

std::string sspi_error(const char* stage, long status) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%s (TLS 0x%08lx)", stage,
                  static_cast<unsigned long>(status));
    return buffer;
}

CredHandle* as_cred(unsigned long long* storage) {
    return reinterpret_cast<CredHandle*>(storage);
}

CtxtHandle* as_ctxt(unsigned long long* storage) {
    return reinterpret_cast<CtxtHandle*>(storage);
}

}  // namespace

// --- TcpStream ------------------------------------------------------------

TcpStream::~TcpStream() {
    close();
}

bool TcpStream::connect(const std::string& host, unsigned short port, int timeout_seconds) {
    ensure_winsock();
    close();

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    std::string port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results) != 0 || !results) {
        error_ = "Не удалось разрешить имя " + host;
        return false;
    }

    SOCKET handle = INVALID_SOCKET;
    for (addrinfo* it = results; it; it = it->ai_next) {
        handle = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (handle == INVALID_SOCKET) continue;
        if (::connect(handle, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) break;
        closesocket(handle);
        handle = INVALID_SOCKET;
    }
    freeaddrinfo(results);

    if (handle == INVALID_SOCKET) {
        error_ = socket_error("Не удалось подключиться к " + host);
        return false;
    }

    DWORD timeout = static_cast<DWORD>(timeout_seconds > 0 ? timeout_seconds * 1000 : 30000);
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));

    socket_ = static_cast<unsigned long long>(handle);
    error_.clear();
    return true;
}

int TcpStream::read(char* buffer, int size) {
    if (!good()) return -1;
    int received = ::recv(static_cast<SOCKET>(socket_), buffer, size, 0);
    if (received < 0) error_ = socket_error("Обрыв чтения");
    return received;
}

bool TcpStream::write(const char* data, int size) {
    if (!good()) return false;
    int sent_total = 0;
    while (sent_total < size) {
        int sent = ::send(static_cast<SOCKET>(socket_), data + sent_total, size - sent_total, 0);
        if (sent <= 0) {
            error_ = socket_error("Обрыв записи");
            return false;
        }
        sent_total += sent;
    }
    return true;
}

void TcpStream::close() {
    if (socket_ != static_cast<unsigned long long>(-1)) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = static_cast<unsigned long long>(-1);
    }
}

bool tcp_connect(TcpStream& stream, const std::string& host, unsigned short port,
                 int timeout_seconds, std::string& error) {
    if (stream.connect(host, port, timeout_seconds)) return true;
    error = stream.error();
    return false;
}

// --- TlsStream ------------------------------------------------------------

TlsStream::~TlsStream() {
    close();
}

bool TlsStream::handshake(TcpStream* transport, const std::string& host) {
    transport_ = transport;
    if (!transport_ || !transport_->good()) {
        error_ = "Нет соединения для TLS";
        return false;
    }

    SCHANNEL_CRED credentials = {};
    credentials.dwVersion = SCHANNEL_CRED_VERSION;
    credentials.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS |
                          SCH_USE_STRONG_CRYPTO;

    SECURITY_STATUS status = AcquireCredentialsHandleW(
        nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr,
        &credentials, nullptr, nullptr, as_cred(credentials_), nullptr);
    if (status != SEC_E_OK) {
        error_ = sspi_error("Не удалось подготовить TLS", status);
        return false;
    }
    has_credentials_ = true;

    std::wstring target = util::widen(host);
    DWORD request_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                          ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                          ISC_REQ_STREAM;
    DWORD result_flags = 0;

    // First token: no input yet.
    SecBuffer out_buffer = {};
    out_buffer.BufferType = SECBUFFER_TOKEN;
    SecBufferDesc out_desc = {SECBUFFER_VERSION, 1, &out_buffer};

    status = InitializeSecurityContextW(as_cred(credentials_), nullptr,
                                        const_cast<SEC_WCHAR*>(target.c_str()), request_flags, 0,
                                        0, nullptr, 0, as_ctxt(context_), &out_desc,
                                        &result_flags, nullptr);
    if (status != SEC_I_CONTINUE_NEEDED) {
        error_ = sspi_error("TLS не начался", status);
        return false;
    }
    has_context_ = true;

    if (out_buffer.cbBuffer > 0 && out_buffer.pvBuffer) {
        bool sent = transport_->write(static_cast<char*>(out_buffer.pvBuffer),
                                      static_cast<int>(out_buffer.cbBuffer));
        FreeContextBuffer(out_buffer.pvBuffer);
        if (!sent) {
            error_ = "Не удалось отправить приветствие TLS";
            return false;
        }
    }

    std::string buffer;
    for (int guard = 0; guard < 64; ++guard) {
        char chunk[8192];
        int received = transport_->read(chunk, sizeof(chunk));
        if (received <= 0) {
            error_ = "Сервер закрыл соединение во время TLS";
            return false;
        }
        buffer.append(chunk, received);

        SecBuffer in_buffers[2] = {};
        in_buffers[0].BufferType = SECBUFFER_TOKEN;
        in_buffers[0].pvBuffer = buffer.data();
        in_buffers[0].cbBuffer = static_cast<unsigned long>(buffer.size());
        in_buffers[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc in_desc = {SECBUFFER_VERSION, 2, in_buffers};

        SecBuffer token = {};
        token.BufferType = SECBUFFER_TOKEN;
        SecBufferDesc token_desc = {SECBUFFER_VERSION, 1, &token};

        result_flags = 0;
        status = InitializeSecurityContextW(as_cred(credentials_), as_ctxt(context_),
                                            const_cast<SEC_WCHAR*>(target.c_str()),
                                            request_flags, 0, 0, &in_desc, 0, nullptr,
                                            &token_desc, &result_flags, nullptr);

        if (token.cbBuffer > 0 && token.pvBuffer) {
            bool sent = transport_->write(static_cast<char*>(token.pvBuffer),
                                          static_cast<int>(token.cbBuffer));
            FreeContextBuffer(token.pvBuffer);
            if (!sent) {
                error_ = "Не удалось продолжить TLS";
                return false;
            }
        }

        if (status == SEC_E_INCOMPLETE_MESSAGE) continue;  // need more bytes

        if (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_OK) {
            // Anything the server already sent past the handshake is payload.
            if (in_buffers[1].BufferType == SECBUFFER_EXTRA && in_buffers[1].cbBuffer > 0) {
                std::string extra(buffer.end() - in_buffers[1].cbBuffer, buffer.end());
                buffer.swap(extra);
            } else {
                buffer.clear();
            }
            if (status == SEC_E_OK) break;
            continue;
        }

        error_ = sspi_error("Рукопожатие TLS не удалось", status);
        return false;
    }

    if (status != SEC_E_OK) {
        error_ = sspi_error("Рукопожатие TLS не завершилось", status);
        return false;
    }

    SecPkgContext_StreamSizes sizes = {};
    status = QueryContextAttributesW(as_ctxt(context_), SECPKG_ATTR_STREAM_SIZES, &sizes);
    if (status != SEC_E_OK) {
        error_ = sspi_error("Не удалось узнать параметры TLS", status);
        return false;
    }
    header_size_ = sizes.cbHeader;
    trailer_size_ = sizes.cbTrailer;
    max_message_ = sizes.cbMaximumMessage;

    incoming_ = buffer;
    established_ = true;
    return true;
}

bool TlsStream::send_all(const char* data, int size) {
    return transport_ && transport_->write(data, size);
}

bool TlsStream::write(const char* data, int size) {
    if (!established_) return false;

    int offset = 0;
    while (offset < size) {
        unsigned long chunk = static_cast<unsigned long>(
            std::min<long long>(max_message_, static_cast<long long>(size - offset)));

        std::vector<char> message(header_size_ + chunk + trailer_size_);
        std::memcpy(message.data() + header_size_, data + offset, chunk);

        SecBuffer buffers[3] = {};
        buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
        buffers[0].pvBuffer = message.data();
        buffers[0].cbBuffer = header_size_;
        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer = message.data() + header_size_;
        buffers[1].cbBuffer = chunk;
        buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
        buffers[2].pvBuffer = message.data() + header_size_ + chunk;
        buffers[2].cbBuffer = trailer_size_;
        SecBufferDesc desc = {SECBUFFER_VERSION, 3, buffers};

        SECURITY_STATUS status = EncryptMessage(as_ctxt(context_), 0, &desc, 0);
        if (status != SEC_E_OK) {
            error_ = sspi_error("Не удалось зашифровать данные", status);
            return false;
        }

        unsigned long total = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
        if (!send_all(message.data(), static_cast<int>(total))) {
            error_ = "Не удалось отправить зашифрованные данные";
            return false;
        }
        offset += static_cast<int>(chunk);
    }
    return true;
}

bool TlsStream::fill_from_transport() {
    char chunk[16384];
    int received = transport_->read(chunk, sizeof(chunk));
    if (received <= 0) return false;
    incoming_.append(chunk, received);
    return true;
}

int TlsStream::read(char* buffer, int size) {
    if (!established_) return -1;

    while (decrypted_.empty()) {
        if (incoming_.empty() && !fill_from_transport()) return 0;

        SecBuffer buffers[4] = {};
        buffers[0].BufferType = SECBUFFER_DATA;
        buffers[0].pvBuffer = incoming_.data();
        buffers[0].cbBuffer = static_cast<unsigned long>(incoming_.size());
        buffers[1].BufferType = SECBUFFER_EMPTY;
        buffers[2].BufferType = SECBUFFER_EMPTY;
        buffers[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc = {SECBUFFER_VERSION, 4, buffers};

        SECURITY_STATUS status = DecryptMessage(as_ctxt(context_), &desc, 0, nullptr);

        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            if (incoming_.size() > static_cast<size_t>(kMaxTlsRecord) * 4) {
                error_ = "Повреждённый поток TLS";
                return -1;
            }
            if (!fill_from_transport()) return 0;
            continue;
        }
        if (status == SEC_I_CONTEXT_EXPIRED) return 0;  // server closed politely
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) {
            error_ = sspi_error("Не удалось расшифровать ответ", status);
            return -1;
        }

        std::string extra;
        for (const SecBuffer& part : buffers) {
            if (part.BufferType == SECBUFFER_DATA && part.cbBuffer > 0) {
                decrypted_.append(static_cast<const char*>(part.pvBuffer), part.cbBuffer);
            } else if (part.BufferType == SECBUFFER_EXTRA && part.cbBuffer > 0) {
                extra.assign(static_cast<const char*>(part.pvBuffer), part.cbBuffer);
            }
        }
        incoming_.swap(extra);
    }

    int count = static_cast<int>(std::min<size_t>(static_cast<size_t>(size), decrypted_.size()));
    std::memcpy(buffer, decrypted_.data(), count);
    decrypted_.erase(0, count);
    return count;
}

void TlsStream::close() {
    if (has_context_) {
        DeleteSecurityContext(as_ctxt(context_));
        has_context_ = false;
    }
    if (has_credentials_) {
        FreeCredentialsHandle(as_cred(credentials_));
        has_credentials_ = false;
    }
    established_ = false;
    transport_ = nullptr;
    incoming_.clear();
    decrypted_.clear();
}

}  // namespace net
