#include "notify/notify.h"

#include "util/log.h"

namespace notify {

std::vector<std::string> dispatch(const core::Settings& settings,
                                  const std::vector<core::Listing>& listings,
                                  const std::string& task_name) {
    std::vector<std::string> errors;
    if (listings.empty()) return errors;

    if (settings.telegram_enabled && !settings.telegram_token.empty() &&
        !settings.telegram_chat_id.empty()) {
        std::string error;
        if (!telegram::send_batch(settings, listings, task_name, error)) {
            util::log_warn("telegram", error);
            errors.push_back("Telegram: " + error);
        }
    }

    if (settings.email_enabled && !settings.smtp_host.empty() && !settings.email_to.empty()) {
        std::string error;
        if (!mail::send_batch(settings, listings, task_name, error)) {
            util::log_warn("mail", error);
            errors.push_back("Почта: " + error);
        }
    }
    return errors;
}

}  // namespace notify
