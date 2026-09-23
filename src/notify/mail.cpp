#include "notify/notify.h"

#include <windows.h>
#include <objbase.h>

#include <cstdio>

#include "net/tls.h"
#include "util/strings.h"

namespace notify {
namespace mail {
namespace {

// One SMTP conversation. The stream pointer swaps from plain to encrypted when
// STARTTLS upgrades the connection.
class Session {
public:
    bool open(const core::Settings& settings, std::string& error) {
        const unsigned short port = static_cast<unsigned short>(settings.smtp_port);
        if (!tcp_.connect(settings.smtp_host, port, 30)) {
            error = tcp_.error();
            return false;
        }
        stream_ = &tcp_;

        if (settings.smtp_ssl) {
            if (!tls_.handshake(&tcp_, settings.smtp_host)) {
                error = tls_.error();
                return false;
            }
            stream_ = &tls_;
        }

        int code = 0;
        if (!read_reply(code, error) || code != 220) {
            if (error.empty()) error = "Сервер не поздоровался (код " + std::to_string(code) + ")";
            return false;
        }

        if (!ehlo(error)) return false;

        if (!settings.smtp_ssl) {
            if (!command("STARTTLS", code, error)) return false;
            if (code != 220) {
                error = "Сервер отказал в STARTTLS (код " + std::to_string(code) + ")";
                return false;
            }
            if (!tls_.handshake(&tcp_, settings.smtp_host)) {
                error = tls_.error();
                return false;
            }
            stream_ = &tls_;
            if (!ehlo(error)) return false;  // capabilities are re-read after the upgrade
        }

        if (!settings.smtp_user.empty() && !login(settings, error)) return false;
        return true;
    }

    bool send(const core::Settings& settings, const std::vector<std::string>& recipients,
              const std::string& message, std::string& error) {
        int code = 0;
        if (!command("MAIL FROM:<" + settings.smtp_user + ">", code, error)) return false;
        if (code != 250) {
            error = "Сервер не принял отправителя (код " + std::to_string(code) + ")";
            return false;
        }

        for (const std::string& address : recipients) {
            if (!command("RCPT TO:<" + address + ">", code, error)) return false;
            if (code != 250 && code != 251) {
                error = "Сервер не принял получателя " + address +
                        " (код " + std::to_string(code) + ")";
                return false;
            }
        }

        if (!command("DATA", code, error)) return false;
        if (code != 354) {
            error = "Сервер не готов принять письмо (код " + std::to_string(code) + ")";
            return false;
        }

        if (!write_raw(message) || !write_raw("\r\n.\r\n")) {
            error = "Не удалось передать письмо";
            return false;
        }
        if (!read_reply(code, error)) return false;
        if (code != 250) {
            error = "Сервер отклонил письмо (код " + std::to_string(code) + ")";
            return false;
        }
        return true;
    }

    void close() {
        int code = 0;
        std::string ignored;
        if (stream_ && stream_->good()) command("QUIT", code, ignored);
        tls_.close();
        tcp_.close();
        stream_ = nullptr;
    }

private:
    bool ehlo(std::string& error) {
        int code = 0;
        if (!command("EHLO avito-watcher", code, error)) return false;
        if (code == 250) return true;
        // Very old servers only speak HELO.
        if (!command("HELO avito-watcher", code, error)) return false;
        if (code != 250) {
            error = "Сервер не принял приветствие (код " + std::to_string(code) + ")";
            return false;
        }
        return true;
    }

    bool login(const core::Settings& settings, std::string& error) {
        int code = 0;
        if (!command("AUTH LOGIN", code, error)) return false;
        if (code != 334) {
            error = "Сервер не предложил вход по паролю (код " + std::to_string(code) + ")";
            return false;
        }

        const std::string user = util::base64_encode(
            reinterpret_cast<const unsigned char*>(settings.smtp_user.data()),
            settings.smtp_user.size());
        if (!command(user, code, error)) return false;
        if (code != 334) {
            error = "Сервер не принял логин (код " + std::to_string(code) + ")";
            return false;
        }

        const std::string password = util::base64_encode(
            reinterpret_cast<const unsigned char*>(settings.smtp_password.data()),
            settings.smtp_password.size());
        if (!command(password, code, error)) return false;
        if (code != 235) {
            error =
                "Сервер отклонил пароль. Для Gmail, Яндекса и Mail.ru нужен "
                "отдельный пароль приложения, а не обычный пароль от почты.";
            return false;
        }
        return true;
    }

    bool write_raw(const std::string& data) {
        return stream_ && stream_->write(data.data(), static_cast<int>(data.size()));
    }

    bool command(const std::string& line, int& code, std::string& error) {
        if (!write_raw(line + "\r\n")) {
            error = "Обрыв связи с почтовым сервером";
            return false;
        }
        return read_reply(code, error);
    }

    // SMTP replies may span several lines; the last one has a space after the
    // number instead of a hyphen.
    bool read_reply(int& code, std::string& error) {
        code = 0;
        while (true) {
            std::string line;
            if (!read_line(line)) {
                error = "Почтовый сервер закрыл соединение";
                return false;
            }
            if (line.size() >= 3) {
                code = std::atoi(line.substr(0, 3).c_str());
                if (line.size() == 3 || line[3] != '-') return true;
            }
            if (buffer_.size() > 1024 * 1024) {
                error = "Почтовый сервер прислал слишком длинный ответ";
                return false;
            }
        }
    }

    bool read_line(std::string& line) {
        while (true) {
            size_t newline = buffer_.find("\r\n");
            if (newline != std::string::npos) {
                line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 2);
                return true;
            }
            char chunk[4096];
            int received = stream_ ? stream_->read(chunk, sizeof(chunk)) : -1;
            if (received <= 0) return false;
            buffer_.append(chunk, received);
        }
    }

    net::TcpStream tcp_;
    net::TlsStream tls_;
    net::Stream* stream_ = nullptr;
    std::string buffer_;
};

std::vector<std::string> recipients_of(const core::Settings& settings) {
    std::string raw = util::replace_all(settings.email_to, ";", ",");
    std::vector<std::string> result;
    for (const std::string& part : util::split(raw, ',')) {
        std::string address = util::trim(part);
        if (!address.empty()) result.push_back(address);
    }
    return result;
}

// RFC 2047 encoding so Russian subjects survive every mail client.
std::string encode_header(const std::string& text) {
    bool ascii = true;
    for (unsigned char c : text) {
        if (c > 127) { ascii = false; break; }
    }
    if (ascii) return text;
    return "=?UTF-8?B?" +
           util::base64_encode(reinterpret_cast<const unsigned char*>(text.data()), text.size()) +
           "?=";
}

std::string wrap_base64(const std::string& data) {
    std::string encoded =
        util::base64_encode(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    std::string wrapped;
    for (size_t i = 0; i < encoded.size(); i += 76) {
        wrapped += encoded.substr(i, 76);
        wrapped += "\r\n";
    }
    return wrapped;
}

std::string message_id() {
    GUID guid = {};
    CoCreateGuid(&guid);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%08lx%04x%04x", guid.Data1, guid.Data2, guid.Data3);
    return std::string(buffer) + "@avito-watcher";
}

std::string card_html(const core::Listing& listing, const std::string& task_name) {
    std::string image;
    if (!listing.image_url.empty()) {
        image = "<td style=\"padding-right:14px;vertical-align:top;\">"
                "<img src=\"" + util::html_escape(listing.image_url) + "\" width=\"120\" "
                "style=\"border-radius:8px;display:block;\" alt=\"\"></td>";
    }

    std::string meta;
    for (const std::string& part : {listing.location, listing.date_text, listing.seller}) {
        if (part.empty()) continue;
        if (!meta.empty()) meta += " · ";
        meta += util::html_escape(part);
    }

    std::string score;
    if (listing.score < 100) {
        score = "<div style=\"color:#7a7a7a;\">Похожесть: " + std::to_string(listing.score) +
                "%</div>";
    }

    return
        "<table style=\"width:100%;border-collapse:collapse;margin:0 0 18px;"
        "border-bottom:1px solid #e8e8e8;padding-bottom:18px;\"><tr>" + image +
        "<td style=\"vertical-align:top;font-family:Arial,sans-serif;font-size:14px;color:#1a1a1a;\">"
        "<div style=\"font-size:16px;font-weight:bold;margin-bottom:4px;\">"
        "<a href=\"" + util::html_escape(listing.url) +
        "\" style=\"color:#1a1a1a;text-decoration:none;\">" +
        util::html_escape(listing.title.empty() ? "Без названия" : listing.title) + "</a></div>"
        "<div style=\"font-size:18px;font-weight:bold;color:#0a7d33;margin-bottom:4px;\">" +
        util::html_escape(listing.price_label()) + "</div>"
        "<div style=\"color:#7a7a7a;\">" + meta + "</div>" + score +
        "<div style=\"color:#7a7a7a;margin-top:4px;\">Задача: " +
        util::html_escape(task_name) + "</div>"
        "<div style=\"margin-top:8px;\"><a href=\"" + util::html_escape(listing.url) +
        "\" style=\"background:#0af;color:#fff;padding:8px 16px;border-radius:6px;"
        "text-decoration:none;display:inline-block;\">Открыть на Авито</a></div>"
        "</td></tr></table>";
}

std::string build_message(const core::Settings& settings,
                          const std::vector<core::Listing>& listings,
                          const std::string& task_name, const std::string& subject) {
    const std::string boundary = "----avito-watcher-" + message_id().substr(0, 16);
    const std::vector<std::string> to = recipients_of(settings);

    std::string plain;
    std::string cards;
    if (listings.empty()) {
        plain = "Проверка связи. Если вы читаете это письмо, уведомления о новых "
                "объявлениях будут приходить сюда.";
        cards = "<div style=\"font-family:Arial,sans-serif;font-size:14px;\">" +
                util::html_escape(plain) + "</div>";
    } else {
        plain = "Новых объявлений: " + std::to_string(listings.size()) +
                " (задача «" + task_name + "»)\r\n\r\n";
        for (const core::Listing& listing : listings) {
            plain += listing.title + " — " + listing.price_label() + "\r\n";
            std::string where = listing.location;
            if (!listing.date_text.empty()) where += " " + listing.date_text;
            where = util::trim(where);
            if (!where.empty()) plain += "  " + where + "\r\n";
            plain += "  " + listing.url + "\r\n\r\n";
            cards += card_html(listing, task_name);
        }
    }

    std::string html =
        "<html><body style=\"background:#f5f5f5;margin:0;padding:24px;\">"
        "<div style=\"max-width:640px;margin:0 auto;background:#fff;border-radius:12px;"
        "padding:24px;\">";
    if (!listings.empty()) {
        html += "<div style=\"font-family:Arial,sans-serif;font-size:20px;font-weight:bold;"
                "margin-bottom:20px;color:#1a1a1a;\">Новых объявлений: " +
                std::to_string(listings.size()) + "</div>";
    }
    html += cards +
            "<div style=\"font-family:Arial,sans-serif;font-size:12px;color:#9a9a9a;\">"
            "Письмо отправлено приложением Avito Watcher.</div></div></body></html>";

    std::string message;
    message += "From: " + encode_header("Avito Watcher") + " <" + settings.smtp_user + ">\r\n";
    message += "To: " + util::join(to, ", ") + "\r\n";
    message += "Subject: " + encode_header(subject) + "\r\n";
    message += "Message-ID: <" + message_id() + ">\r\n";
    message += "MIME-Version: 1.0\r\n";
    message += "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n\r\n";

    message += "--" + boundary + "\r\n";
    message += "Content-Type: text/plain; charset=UTF-8\r\n";
    message += "Content-Transfer-Encoding: base64\r\n\r\n";
    message += wrap_base64(plain);

    message += "\r\n--" + boundary + "\r\n";
    message += "Content-Type: text/html; charset=UTF-8\r\n";
    message += "Content-Transfer-Encoding: base64\r\n\r\n";
    message += wrap_base64(html);

    message += "\r\n--" + boundary + "--\r\n";
    return message;
}

bool deliver(const core::Settings& settings, const std::string& message, std::string& error) {
    if (util::trim(settings.smtp_host).empty()) {
        error = "Не указан SMTP-сервер";
        return false;
    }
    std::vector<std::string> to = recipients_of(settings);
    if (to.empty()) {
        error = "Не указан адрес получателя";
        return false;
    }

    Session session;
    if (!session.open(settings, error)) {
        session.close();
        return false;
    }
    bool ok = session.send(settings, to, message, error);
    session.close();
    return ok;
}

}  // namespace

bool send_batch(const core::Settings& settings, const std::vector<core::Listing>& listings,
                const std::string& task_name, std::string& error) {
    if (listings.empty()) return true;
    const std::string subject = "Avito Watcher: " + std::to_string(listings.size()) +
                                " новых по задаче «" + task_name + "»";
    return deliver(settings, build_message(settings, listings, task_name, subject), error);
}

bool check(const core::Settings& settings, std::string& info, std::string& error) {
    const std::string subject = "Avito Watcher: проверка почты";
    if (!deliver(settings, build_message(settings, {}, {}, subject), error)) return false;
    info = util::join(recipients_of(settings), ", ");
    return true;
}

}  // namespace mail
}  // namespace notify
