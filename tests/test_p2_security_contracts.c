#include <assert.h>
#include <stdio.h>

#define main linuxchat_server_main
#include "../c-native/server/server.c"
#undef main

int main(void) {
    assert(is_protocol_safe_text("alice", 0));
    assert(is_protocol_safe_text("hello, comma is content", 1));

    assert(!is_protocol_safe_text("", 0));
    assert(!is_protocol_safe_text(NULL, 0));
    assert(!is_protocol_safe_text("alice,bob", 0));
    assert(!is_protocol_safe_text("alice:bob", 1));
    assert(!is_protocol_safe_text("alice;bob", 1));
    assert(!is_protocol_safe_text("alice\nbob", 1));

    assert(are_friends(0, 2) == 0);
    assert(are_friends(1, 1) == 0);

    assert(save_message(0, 2, "valid content") == -1);
    assert(save_message(1, 2, "bad:content") == -1);
    assert(save_message(1, 2, "bad;content") == -1);
    assert(save_message(1, 2, "bad\ncontent") == -1);
    assert(block_user(0, 2) == -1);
    assert(unblock_user(1, 1) == -1);
    assert(create_group(1, "bad:name", "2") == -1);
    assert(create_group(1, "bad;name", "2") == -1);
    assert(save_group_message(1, 2, "bad:group", NULL) == -1);
    assert(save_group_message(1, 2, "bad;group", NULL) == -1);

    puts("P2 security contract tests passed");
    return 0;
}
