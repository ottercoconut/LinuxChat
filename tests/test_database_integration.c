#include <assert.h>
#include <ctype.h>
#include <mysql.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main linuxchat_server_main
#include "../c-native/server/server.c"
#undef main

static const char *required_env(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        fprintf(stderr, "Missing required environment variable: %s\n", name);
        exit(2);
    }
    return value;
}

static int contains_test_token(const char *name) {
    char lower[256];
    size_t len = strlen(name);

    if (len >= sizeof(lower)) {
        return 0;
    }

    for (size_t i = 0; i <= len; i++) {
        lower[i] = (char)tolower((unsigned char)name[i]);
    }

    return strstr(lower, "test") != NULL;
}

static void exec_sql(const char *sql) {
    if (mysql_query(db_conn, sql) != 0) {
        fprintf(stderr, "SQL failed: %s\nError: %s\n", sql, mysql_error(db_conn));
        exit(3);
    }
}

static long scalar_long(const char *sql) {
    MYSQL_RES *res;
    MYSQL_ROW row;
    long value;

    if (mysql_query(db_conn, sql) != 0) {
        fprintf(stderr, "SQL failed: %s\nError: %s\n", sql, mysql_error(db_conn));
        exit(3);
    }

    res = mysql_store_result(db_conn);
    assert(res != NULL);

    row = mysql_fetch_row(res);
    assert(row != NULL && row[0] != NULL);
    value = atol(row[0]);

    mysql_free_result(res);
    return value;
}

static void scalar_text(const char *sql, char *out, size_t out_size) {
    MYSQL_RES *res;
    MYSQL_ROW row;

    if (out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (mysql_query(db_conn, sql) != 0) {
        fprintf(stderr, "SQL failed: %s\nError: %s\n", sql, mysql_error(db_conn));
        exit(3);
    }

    res = mysql_store_result(db_conn);
    assert(res != NULL);

    row = mysql_fetch_row(res);
    assert(row != NULL && row[0] != NULL);
    snprintf(out, out_size, "%s", row[0]);

    mysql_free_result(res);
}

static void reset_schema(void) {
    exec_sql("SET FOREIGN_KEY_CHECKS = 0");
    exec_sql("DROP TABLE IF EXISTS group_message_deliveries");
    exec_sql("DROP TABLE IF EXISTS group_messages");
    exec_sql("DROP TABLE IF EXISTS group_members");
    exec_sql("DROP TABLE IF EXISTS chat_groups");
    exec_sql("DROP TABLE IF EXISTS friend_blocks");
    exec_sql("DROP TABLE IF EXISTS friends");
    exec_sql("DROP TABLE IF EXISTS messages");
    exec_sql("DROP TABLE IF EXISTS users");
    exec_sql("SET FOREIGN_KEY_CHECKS = 1");
}

static void test_registration_and_friend_constraints(void) {
    int alice = register_user("alice_test", "pw", "Alice");
    int bob = register_user("bob_test", "pw", "Bob");
    char stored_password[128];
    char nickname[50];
    int login_id = 0;

    assert(alice > 0);
    assert(bob > 0);
    assert(register_user("alice_test", "pw2", "Duplicate") == -1);
    assert(scalar_long("SELECT COUNT(*) FROM users WHERE username='alice_test'") == 1);

    scalar_text("SELECT password FROM users WHERE username='alice_test'", stored_password, sizeof(stored_password));
    assert(strcmp(stored_password, "pw") != 0);
    assert(strlen(stored_password) == 64);
    assert(strspn(stored_password, "0123456789abcdefABCDEF") == 64);

    assert(login_user("alice_test", "pw", nickname, &login_id) == 0);
    assert(login_id == alice);
    assert(strcmp(nickname, "Alice") == 0);
    assert(login_user("alice_test", "wrong", nickname, &login_id) == -1);
    assert(login_user("alice_test' OR '1'='1", "pw", nickname, &login_id) == -1);

    assert(register_user("unsafe,user", "pw", "Unsafe") == -1);
    assert(register_user("unsafe_colon", "pw:bad", "Unsafe") == -1);
    assert(register_user("unsafe_semicolon", "pw", "Bad;Name") == -1);

    int quoted = register_user("quoted_user' OR '1'='1", "quoted_pw", "Quoted");
    assert(quoted > 0);
    assert(login_user("quoted_user' OR '1'='1", "quoted_pw", nickname, &login_id) == 0);
    assert(login_id == quoted);

    assert(add_friend(alice, 999999) == -1);
    assert(scalar_long("SELECT COUNT(*) FROM friends") == 0);

    assert(add_friend(alice, bob) == 0);
    assert(scalar_long("SELECT COUNT(*) FROM friends WHERE "
                       "(user_id = 1 AND friend_id = 2) OR (user_id = 2 AND friend_id = 1)") == 2);
    assert(are_friends(alice, bob) == 1);
    assert(are_friends(alice, quoted) == 0);
    assert(are_friends(alice, alice) == 0);

    assert(add_friend(alice, bob) == -1);
    assert(scalar_long("SELECT COUNT(*) FROM friends WHERE "
                       "(user_id = 1 AND friend_id = 2) OR (user_id = 2 AND friend_id = 1)") == 2);

    assert(block_user(bob, alice) == 0);
    assert(has_block_between(alice, bob) == 1);
    assert(can_send_private_message(alice, bob) == 0);
    assert(block_user(bob, alice) == -1);
    assert(scalar_long("SELECT COUNT(*) FROM friend_blocks WHERE blocker_id = 2 AND blocked_id = 1") == 1);
    assert(unblock_user(bob, alice) == 0);
    assert(has_block_between(alice, bob) == 0);
    assert(can_send_private_message(alice, bob) == 1);
}

static void test_transaction_rollback_for_half_friendship(void) {
    exec_sql("DELETE FROM friends");
    exec_sql("INSERT INTO friends (user_id, friend_id, status) VALUES (2, 1, 1)");

    assert(add_friend(1, 2) == -1);
    assert(scalar_long("SELECT COUNT(*) FROM friends WHERE user_id = 1 AND friend_id = 2") == 0);
    assert(scalar_long("SELECT COUNT(*) FROM friends WHERE user_id = 2 AND friend_id = 1") == 1);
}

static void test_messages_timestamp_and_cascade(void) {
    char messages[BUFFER_SIZE] = "";

    exec_sql("DELETE FROM messages");
    exec_sql("INSERT INTO messages (sender_id, receiver_id, content, timestamp) VALUES "
             "(1, 2, 'older message', '2026-06-23 09:00:01'),"
             "(2, 1, 'newer message', '2026-06-23 09:01:02')");

    get_messages(1, 2, messages, sizeof(messages));
    assert(strstr(messages, "older message:2026-06-23 09-00-01:Alice;") != NULL);
    assert(strstr(messages, "newer message:2026-06-23 09-01-02:Bob;") != NULL);
    assert(strstr(messages, "09:00:01") == NULL);
    assert(strstr(messages, "older message") < strstr(messages, "newer message"));

    long before = scalar_long("SELECT COUNT(*) FROM messages");
    assert(save_message(1, 2, "prepared quote ' and comma, ok") == 0);
    assert(scalar_long("SELECT COUNT(*) FROM messages") == before + 1);

    before = scalar_long("SELECT COUNT(*) FROM messages");
    assert(save_message(1, 2, "invalid:delimiter") == -1);
    assert(scalar_long("SELECT COUNT(*) FROM messages") == before);

    assert(save_message(999999, 2, "invalid sender should not persist") == -1);
    assert(scalar_long("SELECT COUNT(*) FROM messages") == before);

    exec_sql("DELETE FROM users WHERE id = 1");
    assert(scalar_long("SELECT COUNT(*) FROM messages WHERE sender_id = 1 OR receiver_id = 1") == 0);
    assert(scalar_long("SELECT COUNT(*) FROM friends WHERE user_id = 1 OR friend_id = 1") == 0);
    assert(scalar_long("SELECT COUNT(*) FROM chat_groups WHERE owner_id = 1") == 0);
    assert(scalar_long("SELECT COUNT(*) FROM group_messages WHERE group_id = 1") == 0);
    assert(scalar_long("SELECT COUNT(*) FROM group_message_deliveries") == 0);
}

static void test_group_chat_and_offline_consistency(void) {
    int message_id = -1;
    int dana = register_user("dana_test", "pw", "Dana");
    int group_id;
    char initial_members[64];
    char group_offline_record[64];
    char groups[BUFFER_SIZE] = "";
    char members[BUFFER_SIZE] = "";
    char messages[BUFFER_SIZE] = "";
    char offline[BUFFER_SIZE] = "";

    assert(dana > 0);

    snprintf(initial_members, sizeof(initial_members), "2,2,%ld",
             scalar_long("SELECT id FROM users WHERE username='quoted_user'' OR ''1''=''1'"));
    group_id = create_group(1, "Study Group", initial_members);
    assert(group_id > 0);
    assert(scalar_long("SELECT COUNT(*) FROM group_members WHERE group_id = 1") == 3);
    assert(create_group(1, "Unsafe:Group", "2") == -1);
    assert(create_group(1, "Bad Members", "2,bad") == -1);
    assert(create_group(1, "Missing Member", "999999") == -1);
    assert(scalar_long("SELECT COUNT(*) FROM chat_groups WHERE name = 'Bad Members'") == 0);
    assert(scalar_long("SELECT COUNT(*) FROM chat_groups WHERE name = 'Missing Member'") == 0);

    get_groups(1, groups, sizeof(groups));
    assert(strstr(groups, "Study Group") != NULL);
    assert(is_group_member(1, group_id) == 1);
    assert(is_group_member(dana, group_id) == 0);

    assert(get_group_members(1, group_id, members, sizeof(members)) == 0);
    assert(strstr(members, "alice_test:Alice;") != NULL);
    assert(strstr(members, "bob_test:Bob;") != NULL);
    assert(get_group_members(dana, group_id, members, sizeof(members)) == -1);

    assert(add_group_member(1, group_id, 2) == -1);
    assert(add_group_member(dana, group_id, 1) == -1);
    assert(add_group_member(1, group_id, dana) == 0);
    assert(scalar_long("SELECT COUNT(*) FROM group_members WHERE group_id = 1") == 4);

    assert(save_group_message(dana, group_id, "hello group", &message_id) == 0);
    assert(message_id > 0);
    assert(scalar_long("SELECT COUNT(*) FROM group_messages WHERE group_id = 1") == 1);
    assert(scalar_long("SELECT COUNT(*) FROM group_message_deliveries WHERE message_id = 1") == 4);
    assert(save_group_message(999999, group_id, "not a member", NULL) == -1);
    assert(save_group_message(dana, group_id, "bad:delimiter", NULL) == -1);

    get_offline_messages(1, offline, sizeof(offline));
    snprintf(group_offline_record, sizeof(group_offline_record), "GROUP:%d:1;", group_id);
    assert(strstr(offline, group_offline_record) != NULL);
    assert(get_group_messages(1, group_id, messages, sizeof(messages)) == 0);
    assert(strstr(messages, "hello group") != NULL);
    assert(strstr(messages, "Dana;") != NULL);
    offline[0] = '\0';
    get_offline_messages(1, offline, sizeof(offline));
    assert(strstr(offline, group_offline_record) == NULL);

    assert(save_message(1, 2, "offline private") == 0);
    get_offline_messages(2, offline, sizeof(offline));
    assert(strstr(offline, "PRIVATE:1:1;") != NULL);
    get_messages(2, 1, messages, sizeof(messages));
    offline[0] = '\0';
    get_offline_messages(2, offline, sizeof(offline));
    assert(strstr(offline, "PRIVATE:1:1;") == NULL);

}

int main(void) {
    const char *host = getenv("LINUXCHAT_TEST_DB_HOST");
    const char *password = getenv("LINUXCHAT_TEST_DB_PASSWORD");
    const char *port_env = getenv("LINUXCHAT_TEST_DB_PORT");
    const char *user = required_env("LINUXCHAT_TEST_DB_USER");
    const char *database = required_env("LINUXCHAT_TEST_DB_NAME");
    unsigned int port = 0;

    if (host == NULL || host[0] == '\0') {
        host = "localhost";
    }
    if (password == NULL) {
        password = "";
    }
    if (port_env != NULL && port_env[0] != '\0') {
        port = (unsigned int)atoi(port_env);
    }
    if (!contains_test_token(database)) {
        fprintf(stderr, "Refusing to run destructive tests against database '%s'. Name must contain 'test'.\n",
                database);
        return 2;
    }

    db_conn = mysql_init(NULL);
    assert(db_conn != NULL);

    if (!mysql_real_connect(db_conn, host, user, password, database, port, NULL, 0)) {
        fprintf(stderr, "MySQL connection failed: %s\n", mysql_error(db_conn));
        return 2;
    }

    mysql_set_character_set(db_conn, "utf8mb4");
    pthread_mutex_init(&clients_mutex, NULL);
    pthread_mutex_init(&db_mutex, NULL);

    reset_schema();
    create_tables();

    test_registration_and_friend_constraints();
    test_transaction_rollback_for_half_friendship();
    test_group_chat_and_offline_consistency();
    test_messages_timestamp_and_cascade();

    reset_schema();
    pthread_mutex_destroy(&db_mutex);
    pthread_mutex_destroy(&clients_mutex);
    mysql_close(db_conn);

    puts("database integration tests passed");
    return 0;
}
