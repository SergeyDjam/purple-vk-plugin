#pragma once

#include <connection.h>

#include "common.h"

// Data, associated with account. It contains all information, required for connecting and executing
// API calls.
class VkConnData
{
public:
    VkConnData(const string& email, const string& password);

    using AuthSuccessCb = std::function<void(void)>;
    void authenticate(PurpleConnection* gc, const AuthSuccessCb& success_cb, const ErrorCb& error_cb);

    const string& access_token() const
    {
        return m_access_token;
    }

    const string& uid() const
    {
        return m_uid;
    }

private:
    string m_email;
    string m_password;
    string m_access_token;
    string m_uid;
};

// Data, associated with one buddy. See vk.com for documentation on each field.
struct VkBuddyData
{
    string uid;

    string activity;
    string bdate;
    string education;
    string photo_max;
    string mobile_phone;
    string domain;
    bool is_mobile;
};
