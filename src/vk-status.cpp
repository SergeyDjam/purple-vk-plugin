#include <debug.h>

#include "vk-api.h"

#include "vk-status.h"


void vk_update_status(PurpleConnection* gc)
{
    PurpleStatus* status = purple_account_get_active_status(purple_connection_get_account(gc));
    PurpleStatusPrimitive primitive_status = purple_status_type_get_primitive(purple_status_get_type(status));
    if (primitive_status == PURPLE_STATUS_OFFLINE) {
        purple_debug_info("prpl-vkcom", "Status is Offline, setting offline\n");
        vk_set_offline(gc);
    }
    else if(primitive_status == PURPLE_STATUS_INVISIBLE) {
        purple_debug_info("prpl-vkcom", "Status is Invisible, setting offline\n");
        vk_set_offline(gc);
    }
    else {
        purple_debug_info("prpl-vkcom", "Status is Available, setting online\n");
        vk_call_api(gc, "account.setOnline", {});
    }
}

void vk_set_offline(PurpleConnection* gc)
{
    vk_call_api(gc, "account.setOffline", {});
}
