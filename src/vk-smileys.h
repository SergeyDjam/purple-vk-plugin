// Handling of smileys (aka emoticons, emojis).

// NOTES:
//  1) Vk.com sends you only Unicode smileys (😊 instead of :-), 😃 instead of :-D etc),
//  2) Vk.com accepts both Unicode smileys and their text variants, but we convert all to Unicode
//     just to be sure.
//  3) Most smiley themes for Pidgin only support basic text smileys and the support for Unicode
//     smileys in fonts is rather limited (limited number of Unicode smileys supported, still
//     no colored support).

#pragma once

#include "common.h"

#include <conversation.h>

// Initializes Vk.com smileys theme (it is used to replace Unicode smileys with their text
// variants even when the Vk.com smiley theme is not activated).
void initialize_smileys();

// Converts smileys in outgoing messages. Must be called before passing the text to messages.send.
void convert_outgoing_smileys(string& message);

// Converts smileys in incoming messages. Must be called after receiving the messages from longpoll
// or from messages.get.
void convert_incoming_smileys(string& message);


// Adds custom smileys to the conversation, based on the smileys present in the message. This is
// used so that even if the user did not enable the smiley theme, smileys are still shown to him.
void add_custom_smileys(PurpleConversation* conv, const char* message);
