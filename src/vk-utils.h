// Miscellaneous Vk.com utilities.

#pragma once

#include "common.h"

#include <connection.h>

// Finds links to photo/video/docs on vk.com and returns attachment string, describing them
// as required by message.send API call.
string parse_vkcom_attachments(const string& message);
