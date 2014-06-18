#include <debug.h>

#include "vk-api.h"

#include "vk-status.h"


void update_status(PurpleConnection* gc)
{
    PurpleStatus* status = purple_account_get_active_status(purple_connection_get_account(gc));
    PurpleStatusPrimitive primitive_status = purple_status_type_get_primitive(purple_status_get_type(status));
    switch (primitive_status) {
    case PURPLE_STATUS_AVAILABLE:
        vkcom_debug_info("Status is Available, setting online\n");
        set_online(gc);
        break;
    case PURPLE_STATUS_AWAY:
        vkcom_debug_info("Status is Away, setting offline\n");
        set_offline(gc);
        break;
    case PURPLE_STATUS_INVISIBLE:
        vkcom_debug_info("Status is Invisible, setting offline\n");
        set_offline(gc);
        break;
    case PURPLE_STATUS_OFFLINE:
        vkcom_debug_info("Status is Offline, setting offline\n");
        set_offline(gc);
        break;
    default:
        vkcom_debug_error("Unknown primitive status %d\n", primitive_status);
        break;
    }
}

void set_online(PurpleConnection* gc)
{
    vk_call_api(gc, "account.setOnline", CallParams(), nullptr, nullptr);
}

void set_offline(PurpleConnection* gc)
{
    vk_call_api(gc, "account.setOffline", CallParams(), nullptr, nullptr);
}
