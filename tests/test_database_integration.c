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

static void reset_schema(void) {
    exec_sql("SET FOREIGN_KEY_CHECKS = 0");
    exec_sql("DROP TABLE IF EXISTS friends");
    exec_sql("DROP TABLE IF EXISTS messages");
    exec_sql("DROP TABLE IF EXISTS users");
    exec_sql("SET FOREIGN_KEY_CHECKS = 1");
}

static void test_registration_and_friend_constraints(void) {
    int alice = register_user("alice_test", "pw", "Alice");
    int bob = register_user("bob_test", "pw", "Bob");

    assert(alice > 0);
    assert(bob > 0);
    assert(register_user("alice_test", "pw2", "Duplicate") == -1);
    assert(scalar_long("SELECT COUNT(*) FROM users WHERE username='alice_test'") == 1);

    assert(add_friend(alice, 999999) == -1);
    assert(scalar_long("SELECT COUNT(*) FROM friends") == 0);

    assert(add_friend(alice, bob) == 0);
    assert(scalar_long("SELECT COUNT(*) FROM friends WHERE "
                       "(user_id = 1 AND friend_id = 2) OR (user_id = 2 AND friend_id = 1)") == 2);

    assert(add_friend(alice, bob) == -1);
    assert(scalar_long("SELECT COUNT(*) FROM friends WHERE "
                       "(user_id = 1 AND friend_id = 2) OR (user_id = 2 AND friend_id = 1)") == 2);
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
    save_message(999999, 2, "invalid sender should not persist");
    assert(scalar_long("SELECT COUNT(*) FROM messages") == before);

    exec_sql("DELETE FROM users WHERE id = 1");
    assert(scalar_long("SELECT COUNT(*) FROM messages WHERE sender_id = 1 OR receiver_id = 1") == 0);
    assert(scalar_long("SELECT COUNT(*) FROM friends WHERE user_id = 1 OR friend_id = 1") == 0);
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
    test_messages_timestamp_and_cascade();

    reset_schema();
    pthread_mutex_destroy(&db_mutex);
    pthread_mutex_destroy(&clients_mutex);
    mysql_close(db_conn);

    puts("database integration tests passed");
    return 0;
}
