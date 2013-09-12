#include "vk-common.h"

#include "vk-auth.h"

const char VK_CLIENT_ID[] = "3833170";

VkConnData::VkConnData(const string& email, const string& password)
    : m_email(email),
      m_password(password)
{
}

void VkConnData::authenticate(PurpleConnection* gc, const AuthSuccessCb& success_cb, const ErrorCb& error_cb)
{
    vk_auth_user(gc, m_email, m_password, VK_CLIENT_ID, "friends,messages",
        [=](const string& access_token, const string& uid) {
            m_access_token = access_token;
            m_uid = uid;
            success_cb();
        }, error_cb);
}
