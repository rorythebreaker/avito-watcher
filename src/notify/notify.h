// Outbound notification channels.
//
// The in-app feed and the Windows toast are drawn by the window itself, so only
// Telegram and email live here. A failing channel never stops the others and
// never aborts a check: the errors are returned for the journal.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"
#include "core/models.h"

namespace notify {

std::vector<std::string> dispatch(const core::Settings& settings,
                                  const std::vector<core::Listing>& listings,
                                  const std::string& task_name);

namespace telegram {

bool send_batch(const core::Settings& settings, const std::vector<core::Listing>& listings,
                const std::string& task_name, std::string& error);

// Sends a test message. On success `info` holds the bot name.
bool check(const core::Settings& settings, std::string& info, std::string& error);

// Reads the chat id from the bot's recent updates.
bool resolve_chat_id(const core::Settings& settings, std::string& chat_id, std::string& error);

}  // namespace telegram

namespace mail {

bool send_batch(const core::Settings& settings, const std::vector<core::Listing>& listings,
                const std::string& task_name, std::string& error);

bool check(const core::Settings& settings, std::string& info, std::string& error);

}  // namespace mail

}  // namespace notify
