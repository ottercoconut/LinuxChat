#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <mysql.h>

#define PORT 8888
#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024
#define REGISTER_ERROR_SERVER -1
#define REGISTER_ERROR_DUPLICATE -2
#define REGISTER_ERROR_INVALID_INPUT -3
#define GROUP_MEMBER_ADD_ALREADY_MEMBER -2
#define GROUP_MEMBER_ADD_BLOCKED -3
#define UNBLOCK_USER_NOT_BLOCKED -2
#define UNBLOCK_USER_BLOCKED_BY_TARGET -3
#define HAS_PREFIX(text, prefix) (strncmp((text), (prefix), strlen(prefix)) == 0)
#define PREFIX_PAYLOAD(text, prefix) ((text) + strlen(prefix))

MYSQL *db_conn;
pthread_mutex_t db_mutex;

typedef struct {
    int sockfd;
    struct sockaddr_in addr;
    int user_id;
    char username[50];
} Client;

Client clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex;

int client_count = 0;
volatile sig_atomic_t server_running = 1;

void handle_shutdown_signal(int signo) {
    (void)signo;
    server_running = 0;
}

int install_signal_handlers(void) {
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_shutdown_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }

    return 0;
}

int is_user_online(int user_id) {
    int online = 0;

    if (user_id <= 0) {
        return 0;
    }

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].user_id == user_id) {
            online = 1;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    return online;
}

int is_protocol_safe_text(const char *text, int allow_comma) {
    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    for (const unsigned char *ptr = (const unsigned char *)text; *ptr != '\0'; ptr++) {
        if (*ptr < 32 || *ptr == ':' || *ptr == ';' || (!allow_comma && *ptr == ',')) {
            return 0;
        }
    }

    return 1;
}

static void bind_string_param(MYSQL_BIND *bind, const char *value, unsigned long *length) {
    *length = (unsigned long)strlen(value);
    bind->buffer_type = MYSQL_TYPE_STRING;
    bind->buffer = (char *)value;
    bind->buffer_length = *length;
    bind->length = length;
}

static void bind_int_param(MYSQL_BIND *bind, int *value) {
    bind->buffer_type = MYSQL_TYPE_LONG;
    bind->buffer = value;
}

int append_protocol_record(char *result, size_t result_size,
                           const char *field1, const char *field2, const char *field3) {
    if (result_size == 0) {
        return -1;
    }

    size_t used = strlen(result);

    if (used >= result_size) {
        return -1;
    }

    int written = snprintf(result + used, result_size - used, "%s:%s:%s;",
                           field1 ? field1 : "", field2 ? field2 : "", field3 ? field3 : "");
    if (written < 0 || (size_t)written >= result_size - used) {
        result[result_size - 1] = '\0';
        return -1;
    }

    return 0;
}

static int user_exists_locked(int user_id);

static int format_local_protocol_timestamp(char *timestamp, size_t timestamp_size) {
    time_t now;
    struct tm tm_info;

    if (timestamp == NULL || timestamp_size == 0) {
        return -1;
    }

    timestamp[0] = '\0';
    now = time(NULL);
    if (localtime_r(&now, &tm_info) == NULL) {
        return -1;
    }

    return strftime(timestamp, timestamp_size, "%Y-%m-%d %H-%M-%S", &tm_info) > 0 ? 0 : -1;
}

static int fetch_formatted_timestamp_locked(const char *statement, int record_id,
                                            char *timestamp, size_t timestamp_size) {
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[1];
    MYSQL_BIND results[1];
    unsigned long timestamp_length = 0;
    int bound_record_id = record_id;
    int fetch_status;
    int status = -1;

    if (statement == NULL || record_id <= 0 || timestamp == NULL || timestamp_size < 2) {
        return -1;
    }
    timestamp[0] = '\0';

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_record_id);

    memset(results, 0, sizeof(results));
    results[0].buffer_type = MYSQL_TYPE_STRING;
    results[0].buffer = timestamp;
    results[0].buffer_length = timestamp_size - 1;
    results[0].length = &timestamp_length;

    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化时间戳查询失败: %s\n", mysql_error(db_conn));
        return -1;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, results) != 0) {
        fprintf(stderr, "时间戳查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    fetch_status = mysql_stmt_fetch(stmt);
    if (fetch_status == 0 || fetch_status == MYSQL_DATA_TRUNCATED) {
        timestamp[timestamp_length < timestamp_size ? timestamp_length : timestamp_size - 1] = '\0';
        status = 0;
    }

cleanup:
    mysql_stmt_close(stmt);
    return status;
}

void update_client_session(int sockfd, int user_id, const char *username) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].sockfd == sockfd) {
            clients[i].user_id = user_id;
            strncpy(clients[i].username, username, sizeof(clients[i].username) - 1);
            clients[i].username[sizeof(clients[i].username) - 1] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void init_database(void) {
    db_conn = mysql_init(NULL);
    if (!mysql_real_connect(db_conn, "localhost", "chat_user", "chat_password", "chat_db", 0, NULL, 0)) {
        fprintf(stderr, "数据库连接失败: %s\n", mysql_error(db_conn));
        exit(1);
    }
    printf("数据库连接成功\n");
}

void create_tables(void) {
    char *create_users = "CREATE TABLE IF NOT EXISTS users ("
                         "id INT PRIMARY KEY,"
                         "username VARCHAR(50) UNIQUE NOT NULL,"
                         "password VARCHAR(100) NOT NULL,"
                         "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)";

    char *create_messages = "CREATE TABLE IF NOT EXISTS messages ("
                            "id INT AUTO_INCREMENT PRIMARY KEY,"
                            "sender_id INT NOT NULL,"
                            "receiver_id INT NOT NULL,"
                            "content TEXT NOT NULL,"
                            "delivered TINYINT(1) DEFAULT 0,"
                            "timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                            "FOREIGN KEY(sender_id) REFERENCES users(id) ON DELETE CASCADE,"
                            "FOREIGN KEY(receiver_id) REFERENCES users(id) ON DELETE CASCADE)";

    char *create_friends = "CREATE TABLE IF NOT EXISTS friends ("
                           "id INT AUTO_INCREMENT PRIMARY KEY,"
                           "user_id INT NOT NULL,"
                           "friend_id INT NOT NULL,"
                           "status INT DEFAULT 0,"
                           "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                           "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
                           "FOREIGN KEY(friend_id) REFERENCES users(id) ON DELETE CASCADE,"
                           "UNIQUE KEY unique_friend (user_id, friend_id))";

    char *create_friend_blocks = "CREATE TABLE IF NOT EXISTS friend_blocks ("
                                 "id INT AUTO_INCREMENT PRIMARY KEY,"
                                 "blocker_id INT NOT NULL,"
                                 "blocked_id INT NOT NULL,"
                                 "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                                 "FOREIGN KEY(blocker_id) REFERENCES users(id) ON DELETE CASCADE,"
                                 "FOREIGN KEY(blocked_id) REFERENCES users(id) ON DELETE CASCADE,"
                                 "UNIQUE KEY unique_block (blocker_id, blocked_id))";

    char *create_groups = "CREATE TABLE IF NOT EXISTS chat_groups ("
                          "id INT AUTO_INCREMENT PRIMARY KEY,"
                          "name VARCHAR(80) NOT NULL,"
                          "owner_id INT NOT NULL,"
                          "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                          "FOREIGN KEY(owner_id) REFERENCES users(id) ON DELETE CASCADE)";

    char *create_group_members = "CREATE TABLE IF NOT EXISTS group_members ("
                                 "id INT AUTO_INCREMENT PRIMARY KEY,"
                                 "group_id INT NOT NULL,"
                                 "user_id INT NOT NULL,"
                                 "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                                 "FOREIGN KEY(group_id) REFERENCES chat_groups(id) ON DELETE CASCADE,"
                                 "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
                                 "UNIQUE KEY unique_group_member (group_id, user_id))";

    char *create_group_messages = "CREATE TABLE IF NOT EXISTS group_messages ("
                                  "id INT AUTO_INCREMENT PRIMARY KEY,"
                                  "group_id INT NOT NULL,"
                                  "sender_id INT NOT NULL,"
                                  "content TEXT NOT NULL,"
                                  "timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                                  "FOREIGN KEY(group_id) REFERENCES chat_groups(id) ON DELETE CASCADE,"
                                  "FOREIGN KEY(sender_id) REFERENCES users(id) ON DELETE CASCADE)";

    char *create_group_message_deliveries = "CREATE TABLE IF NOT EXISTS group_message_deliveries ("
                                            "id INT AUTO_INCREMENT PRIMARY KEY,"
                                            "message_id INT NOT NULL,"
                                            "user_id INT NOT NULL,"
                                            "delivered TINYINT(1) DEFAULT 0,"
                                            "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                                            "FOREIGN KEY(message_id) REFERENCES group_messages(id) ON DELETE CASCADE,"
                                            "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
                                            "UNIQUE KEY unique_group_delivery (message_id, user_id))";

    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, create_users) != 0) {
        fprintf(stderr, "创建users表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_messages) != 0) {
        fprintf(stderr, "创建messages表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_friends) != 0) {
        fprintf(stderr, "创建friends表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, "ALTER TABLE messages ADD COLUMN delivered TINYINT(1) DEFAULT 0") != 0 &&
        mysql_errno(db_conn) != 1060) {
        fprintf(stderr, "扩展messages表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_friend_blocks) != 0) {
        fprintf(stderr, "创建friend_blocks表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_groups) != 0) {
        fprintf(stderr, "创建chat_groups表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_group_members) != 0) {
        fprintf(stderr, "创建group_members表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_group_messages) != 0) {
        fprintf(stderr, "创建group_messages表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_group_message_deliveries) != 0) {
        fprintf(stderr, "创建group_message_deliveries表失败: %s\n", mysql_error(db_conn));
    }
    pthread_mutex_unlock(&db_mutex);
    printf("数据库表初始化完成\n");
}

static int generate_candidate_user_id(void) {
    static int random_seeded = 0;

    if (!random_seeded) {
        srand((unsigned int)(time(NULL) ^ getpid()));
        random_seeded = 1;
    }

    return 10000000 + rand() % 90000000;
}

static int generate_unique_user_id_locked(void) {
    for (int attempt = 0; attempt < 100; attempt++) {
        int candidate = generate_candidate_user_id();
        if (!user_exists_locked(candidate)) {
            return candidate;
        }
    }

    return -1;
}

int register_user(const char *username, const char *password) {
    const char *statement =
        "INSERT INTO users (id, username, password) VALUES (?, ?, SHA2(?, 256))";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[3];
    unsigned long lengths[2];
    int user_id = REGISTER_ERROR_SERVER;

    if (username == NULL ||
        password == NULL ||
        strlen(username) >= 50 ||
        strlen(password) >= 50 ||
        !is_protocol_safe_text(username, 0) ||
        !is_protocol_safe_text(password, 0)) {
        return REGISTER_ERROR_INVALID_INPUT;
    }

    pthread_mutex_lock(&db_mutex);
    user_id = generate_unique_user_id_locked();
    if (user_id <= 0) {
        user_id = REGISTER_ERROR_SERVER;
        goto cleanup;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &user_id);
    bind_string_param(&params[1], username, &lengths[0]);
    bind_string_param(&params[2], password, &lengths[1]);

    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化注册语句失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "注册失败: %s\n", mysql_stmt_error(stmt));
        if (mysql_stmt_errno(stmt) == 1062) {
            user_id = REGISTER_ERROR_DUPLICATE;
        }
        goto cleanup;
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return user_id;
}

int login_user(const char *username, const char *password, int *user_id) {
    const char *statement =
        "SELECT id FROM users WHERE username = ? AND password = SHA2(?, 256)";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[2];
    MYSQL_BIND results[1];
    unsigned long param_lengths[2];
    int found_user_id = 0;
    int status = -1;

    if (!is_protocol_safe_text(username, 0) ||
        !is_protocol_safe_text(password, 0) ||
        user_id == NULL) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_string_param(&params[0], username, &param_lengths[0]);
    bind_string_param(&params[1], password, &param_lengths[1]);

    memset(results, 0, sizeof(results));
    results[0].buffer_type = MYSQL_TYPE_LONG;
    results[0].buffer = &found_user_id;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化登录语句失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, results) != 0) {
        fprintf(stderr, "登录查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    int fetch_status = mysql_stmt_fetch(stmt);
    if (fetch_status == 0 || fetch_status == MYSQL_DATA_TRUNCATED) {
        *user_id = found_user_id;
        status = 0;
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

int get_user_id_by_username(const char *username, int *user_id) {
    const char *statement = "SELECT id FROM users WHERE username = ?";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[1];
    MYSQL_BIND results[1];
    unsigned long username_length;
    int found_user_id = 0;
    int status = -1;

    if (user_id == NULL ||
        username == NULL ||
        strlen(username) >= 50 ||
        !is_protocol_safe_text(username, 0)) {
        return -1;
    }

    *user_id = -1;

    memset(params, 0, sizeof(params));
    bind_string_param(&params[0], username, &username_length);

    memset(results, 0, sizeof(results));
    results[0].buffer_type = MYSQL_TYPE_LONG;
    results[0].buffer = &found_user_id;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化用户名查询失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, results) != 0) {
        fprintf(stderr, "用户名查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    if (mysql_stmt_fetch(stmt) == 0) {
        *user_id = found_user_id;
        status = 0;
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

int count_friend_relation_rows(int user_id, int friend_id) {
    const char *statement =
        "SELECT COUNT(*) FROM friends "
        "WHERE (user_id = ? AND friend_id = ?) OR (user_id = ? AND friend_id = ?)";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[4];
    MYSQL_BIND result_bind[1];
    int bound_user_id = user_id;
    int bound_friend_id = friend_id;
    long long count = 0;
    int result = -1;

    if (user_id <= 0 || friend_id <= 0 || user_id == friend_id) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_user_id);
    bind_int_param(&params[1], &bound_friend_id);
    bind_int_param(&params[2], &bound_friend_id);
    bind_int_param(&params[3], &bound_user_id);

    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[0].buffer = &count;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化好友关系计数失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, result_bind) != 0) {
        fprintf(stderr, "好友关系计数失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    if (mysql_stmt_fetch(stmt) == 0) {
        result = (int)count;
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return result;
}

int add_friend(int user_id, int friend_id) {
    char query[500];
    int relation_rows;

    if (user_id <= 0 || friend_id <= 0 || user_id == friend_id) {
        return -1;
    }

    relation_rows = count_friend_relation_rows(user_id, friend_id);
    if (relation_rows == 2) {
        return 0;
    }
    if (relation_rows != 0) {
        return -1;
    }

    snprintf(query, sizeof(query), "INSERT INTO friends (user_id, friend_id, status) VALUES (%d, %d, 1)",
             user_id, friend_id);

    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, "START TRANSACTION") != 0) {
        fprintf(stderr, "开始好友事务失败: %s\n", mysql_error(db_conn));
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }

    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "添加好友失败: %s\n", mysql_error(db_conn));
        mysql_query(db_conn, "ROLLBACK");
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }

    snprintf(query, sizeof(query), "INSERT INTO friends (user_id, friend_id, status) VALUES (%d, %d, 1)",
             friend_id, user_id);

    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "添加好友失败: %s\n", mysql_error(db_conn));
        mysql_query(db_conn, "ROLLBACK");
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }

    if (mysql_query(db_conn, "COMMIT") != 0) {
        fprintf(stderr, "提交好友事务失败: %s\n", mysql_error(db_conn));
        mysql_query(db_conn, "ROLLBACK");
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }

    pthread_mutex_unlock(&db_mutex);
    return 0;
}

int add_friend_by_username(int user_id, const char *friend_username) {
    int friend_id;

    if (user_id <= 0 ||
        friend_username == NULL ||
        strlen(friend_username) >= 50 ||
        !is_protocol_safe_text(friend_username, 0) ||
        get_user_id_by_username(friend_username, &friend_id) != 0) {
        return -1;
    }

    return add_friend(user_id, friend_id);
}

void get_friends(int user_id, char *result, size_t result_size) {
    char query[500];
    snprintf(query, sizeof(query), "SELECT u.id, u.username, u.username FROM friends f "
                                   "JOIN users u ON f.friend_id = u.id WHERE f.user_id = %d", user_id);

    if (result_size == 0) {
        return;
    }
    result[0] = '\0';
    if (user_id <= 0) {
        return;
    }

    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "获取好友列表失败: %s\n", mysql_error(db_conn));
        pthread_mutex_unlock(&db_mutex);
        return;
    }

    MYSQL_RES *res = mysql_store_result(db_conn);
    if (res == NULL) {
        pthread_mutex_unlock(&db_mutex);
        return;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL) {
        if (append_protocol_record(result, result_size, row[0], row[1], row[2]) != 0) {
            fprintf(stderr, "好友列表结果过长，已截断\n");
            break;
        }
    }
    mysql_free_result(res);
    pthread_mutex_unlock(&db_mutex);
}

int are_friends(int user_id, int friend_id) {
    char query[300];
    int result = 0;

    if (user_id <= 0 || friend_id <= 0 || user_id == friend_id) {
        return 0;
    }

    snprintf(query, sizeof(query),
             "SELECT COUNT(*) FROM friends WHERE user_id = %d AND friend_id = %d AND status = 1",
             user_id, friend_id);

    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "好友关系查询失败: %s\n", mysql_error(db_conn));
        pthread_mutex_unlock(&db_mutex);
        return 0;
    }

    MYSQL_RES *res = mysql_store_result(db_conn);
    if (res != NULL) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row != NULL && row[0] != NULL) {
            result = atoi(row[0]) > 0;
        }
        mysql_free_result(res);
    }

    pthread_mutex_unlock(&db_mutex);
    return result;
}

int has_block_between(int user_id, int other_id) {
    const char *statement =
        "SELECT COUNT(*) FROM friend_blocks "
        "WHERE (blocker_id = ? AND blocked_id = ?) OR (blocker_id = ? AND blocked_id = ?)";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[4];
    MYSQL_BIND result_bind[1];
    int bound_user_id = user_id;
    int bound_other_id = other_id;
    long long count = 0;
    int blocked = 0;

    if (user_id <= 0 || other_id <= 0 || user_id == other_id) {
        return 0;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_user_id);
    bind_int_param(&params[1], &bound_other_id);
    bind_int_param(&params[2], &bound_other_id);
    bind_int_param(&params[3], &bound_user_id);

    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[0].buffer = &count;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化屏蔽查询失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, result_bind) != 0) {
        fprintf(stderr, "屏蔽关系查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    if (mysql_stmt_fetch(stmt) == 0 && count > 0) {
        blocked = 1;
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return blocked;
}

int can_send_private_message(int sender_id, int receiver_id) {
    return are_friends(sender_id, receiver_id) && !has_block_between(sender_id, receiver_id);
}

int has_direct_block(int blocker_id, int blocked_id) {
    const char *statement =
        "SELECT COUNT(*) FROM friend_blocks WHERE blocker_id = ? AND blocked_id = ?";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[2];
    MYSQL_BIND result_bind[1];
    int bound_blocker_id = blocker_id;
    int bound_blocked_id = blocked_id;
    long long count = 0;
    int blocked = -1;

    if (blocker_id <= 0 || blocked_id <= 0 || blocker_id == blocked_id) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_blocker_id);
    bind_int_param(&params[1], &bound_blocked_id);

    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[0].buffer = &count;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化精确屏蔽查询失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, result_bind) != 0) {
        fprintf(stderr, "精确屏蔽关系查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    if (mysql_stmt_fetch(stmt) == 0) {
        blocked = count > 0 ? 1 : 0;
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return blocked;
}

int block_user(int blocker_id, int blocked_id) {
    const char *statement =
        "INSERT INTO friend_blocks (blocker_id, blocked_id) VALUES (?, ?)";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[2];
    int bound_blocker_id = blocker_id;
    int bound_blocked_id = blocked_id;
    int direct_block;
    int status = -1;

    if (blocker_id <= 0 || blocked_id <= 0 || blocker_id == blocked_id) {
        return -1;
    }

    direct_block = has_direct_block(blocker_id, blocked_id);
    if (direct_block == 1) {
        return 0;
    }
    if (direct_block < 0) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_blocker_id);
    bind_int_param(&params[1], &bound_blocked_id);

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化屏蔽语句失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "屏蔽用户失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    status = 0;

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

int unblock_user(int blocker_id, int blocked_id) {
    const char *statement =
        "DELETE FROM friend_blocks WHERE blocker_id = ? AND blocked_id = ?";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[2];
    int bound_blocker_id = blocker_id;
    int bound_blocked_id = blocked_id;
    int direct_block;
    int status = -1;

    if (blocker_id <= 0 || blocked_id <= 0 || blocker_id == blocked_id) {
        return -1;
    }

    direct_block = has_direct_block(blocker_id, blocked_id);
    if (direct_block == 0) {
        int reverse_block = has_direct_block(blocked_id, blocker_id);
        if (reverse_block == 1) {
            return UNBLOCK_USER_BLOCKED_BY_TARGET;
        }
        if (reverse_block < 0) {
            return -1;
        }
        return UNBLOCK_USER_NOT_BLOCKED;
    }
    if (direct_block < 0) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_blocker_id);
    bind_int_param(&params[1], &bound_blocked_id);

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化解除屏蔽语句失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "解除屏蔽失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    status = 0;

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

int block_user_by_username(int blocker_id, const char *blocked_username) {
    int blocked_id;

    if (blocker_id <= 0 ||
        blocked_username == NULL ||
        strlen(blocked_username) >= 50 ||
        !is_protocol_safe_text(blocked_username, 0) ||
        get_user_id_by_username(blocked_username, &blocked_id) != 0) {
        return -1;
    }

    return block_user(blocker_id, blocked_id);
}

int unblock_user_by_username(int blocker_id, const char *blocked_username) {
    int blocked_id;

    if (blocker_id <= 0 ||
        blocked_username == NULL ||
        strlen(blocked_username) >= 50 ||
        !is_protocol_safe_text(blocked_username, 0) ||
        get_user_id_by_username(blocked_username, &blocked_id) != 0) {
        return -1;
    }

    return unblock_user(blocker_id, blocked_id);
}

int is_group_member(int user_id, int group_id) {
    const char *statement =
        "SELECT COUNT(*) FROM group_members WHERE user_id = ? AND group_id = ?";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[2];
    MYSQL_BIND result_bind[1];
    int bound_user_id = user_id;
    int bound_group_id = group_id;
    long long count = 0;
    int member = 0;

    if (user_id <= 0 || group_id <= 0) {
        return 0;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_user_id);
    bind_int_param(&params[1], &bound_group_id);

    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[0].buffer = &count;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化群成员查询失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, result_bind) != 0) {
        fprintf(stderr, "群成员查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    if (mysql_stmt_fetch(stmt) == 0 && count > 0) {
        member = 1;
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return member;
}

static int insert_group_member_locked(int group_id, int user_id, int ignore_duplicate) {
    const char *insert_statement =
        "INSERT INTO group_members (group_id, user_id) VALUES (?, ?)";
    const char *insert_ignore_statement =
        "INSERT IGNORE INTO group_members (group_id, user_id) VALUES (?, ?)";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[2];
    int bound_group_id = group_id;
    int bound_user_id = user_id;
    int status = -1;

    if (group_id <= 0 || user_id <= 0) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_group_id);
    bind_int_param(&params[1], &bound_user_id);

    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化群成员写入失败: %s\n", mysql_error(db_conn));
        return -1;
    }

    if (mysql_stmt_prepare(stmt,
                           ignore_duplicate ? insert_ignore_statement : insert_statement,
                           strlen(ignore_duplicate ? insert_ignore_statement : insert_statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "写入群成员失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    status = (ignore_duplicate || mysql_stmt_affected_rows(stmt) > 0) ? 0 : -1;

cleanup:
    mysql_stmt_close(stmt);
    return status;
}

static int user_exists_locked(int user_id) {
    const char *statement = "SELECT COUNT(*) FROM users WHERE id = ?";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[1];
    MYSQL_BIND results[1];
    int bound_user_id = user_id;
    long long count = 0;
    int exists = 0;

    if (user_id <= 0) {
        return 0;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_user_id);

    memset(results, 0, sizeof(results));
    results[0].buffer_type = MYSQL_TYPE_LONGLONG;
    results[0].buffer = &count;

    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化用户存在性查询失败: %s\n", mysql_error(db_conn));
        return 0;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, results) != 0) {
        fprintf(stderr, "用户存在性查询失败: %s\n", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return 0;
    }

    if (mysql_stmt_fetch(stmt) == 0 && count > 0) {
        exists = 1;
    }

    mysql_stmt_close(stmt);
    return exists;
}

int add_group_member(int requester_id, int group_id, int new_member_id) {
    int status = -1;

    if (requester_id <= 0 || group_id <= 0 || new_member_id <= 0 ||
        !is_group_member(requester_id, group_id)) {
        return -1;
    }
    if (is_group_member(new_member_id, group_id)) {
        return GROUP_MEMBER_ADD_ALREADY_MEMBER;
    }
    if (has_block_between(requester_id, new_member_id)) {
        return GROUP_MEMBER_ADD_BLOCKED;
    }

    pthread_mutex_lock(&db_mutex);
    if (user_exists_locked(new_member_id)) {
        status = insert_group_member_locked(group_id, new_member_id, 0);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

int add_group_member_by_username(int requester_id, int group_id, const char *new_member_username) {
    int new_member_id;

    if (requester_id <= 0 ||
        group_id <= 0 ||
        new_member_username == NULL ||
        strlen(new_member_username) >= 50 ||
        !is_protocol_safe_text(new_member_username, 0) ||
        get_user_id_by_username(new_member_username, &new_member_id) != 0) {
        return -1;
    }

    return add_group_member(requester_id, group_id, new_member_id);
}

int create_group(int owner_id, const char *group_name, const char *member_csv) {
    const char *statement =
        "INSERT INTO chat_groups (name, owner_id) VALUES (?, ?)";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[2];
    unsigned long name_length;
    int bound_owner_id = owner_id;
    int group_id = -1;

    if (owner_id <= 0 ||
        !is_protocol_safe_text(group_name, 0) ||
        strlen(group_name) >= 80) {
        return -1;
    }

    if (member_csv != NULL && member_csv[0] != '\0') {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_string_param(&params[0], group_name, &name_length);
    bind_int_param(&params[1], &bound_owner_id);

    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, "START TRANSACTION") != 0) {
        fprintf(stderr, "开始创建群聊事务失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化创建群聊语句失败: %s\n", mysql_error(db_conn));
        mysql_query(db_conn, "ROLLBACK");
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "创建群聊失败: %s\n", mysql_stmt_error(stmt));
        mysql_query(db_conn, "ROLLBACK");
        goto cleanup;
    }

    group_id = (int)mysql_insert_id(db_conn);
    if (insert_group_member_locked(group_id, owner_id, 1) != 0) {
        mysql_query(db_conn, "ROLLBACK");
        group_id = -1;
        goto cleanup;
    }

    if (mysql_query(db_conn, "COMMIT") != 0) {
        fprintf(stderr, "提交创建群聊事务失败: %s\n", mysql_error(db_conn));
        mysql_query(db_conn, "ROLLBACK");
        group_id = -1;
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return group_id;
}

void get_groups(int user_id, char *result, size_t result_size) {
    const char *statement =
        "SELECT g.id, g.name, u.username FROM chat_groups g "
        "JOIN group_members gm ON g.id = gm.group_id "
        "JOIN users u ON g.owner_id = u.id "
        "WHERE gm.user_id = ? ORDER BY g.created_at, g.id";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[1];
    MYSQL_BIND results[3];
    int bound_user_id = user_id;
    int group_id = 0;
    char group_name[80] = "";
    char owner_username[50] = "";
    unsigned long group_name_length = 0;
    unsigned long owner_username_length = 0;
    char id_text[16];

    if (result_size == 0) {
        return;
    }
    result[0] = '\0';
    if (user_id <= 0) {
        return;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_user_id);

    memset(results, 0, sizeof(results));
    results[0].buffer_type = MYSQL_TYPE_LONG;
    results[0].buffer = &group_id;
    results[1].buffer_type = MYSQL_TYPE_STRING;
    results[1].buffer = group_name;
    results[1].buffer_length = sizeof(group_name) - 1;
    results[1].length = &group_name_length;
    results[2].buffer_type = MYSQL_TYPE_STRING;
    results[2].buffer = owner_username;
    results[2].buffer_length = sizeof(owner_username) - 1;
    results[2].length = &owner_username_length;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化群列表查询失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, results) != 0) {
        fprintf(stderr, "群列表查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    while (mysql_stmt_fetch(stmt) == 0) {
        group_name[group_name_length < sizeof(group_name) ? group_name_length : sizeof(group_name) - 1] = '\0';
        owner_username[owner_username_length < sizeof(owner_username) ? owner_username_length : sizeof(owner_username) - 1] = '\0';
        snprintf(id_text, sizeof(id_text), "%d", group_id);
        if (append_protocol_record(result, result_size, id_text, group_name, owner_username) != 0) {
            fprintf(stderr, "群列表结果过长，已截断\n");
            break;
        }
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
}

int get_group_members(int requester_id, int group_id, char *result, size_t result_size) {
    const char *statement =
        "SELECT u.id, u.username, u.username FROM group_members gm "
        "JOIN users u ON gm.user_id = u.id "
        "WHERE gm.group_id = ? ORDER BY gm.created_at, gm.id";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[1];
    MYSQL_BIND results[3];
    int bound_group_id = group_id;
    int member_id = 0;
    char username[50] = "";
    char display_username[50] = "";
    unsigned long username_length = 0;
    unsigned long display_username_length = 0;
    char id_text[16];
    int status = -1;

    if (result_size == 0) {
        return -1;
    }
    result[0] = '\0';
    if (!is_group_member(requester_id, group_id)) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_group_id);

    memset(results, 0, sizeof(results));
    results[0].buffer_type = MYSQL_TYPE_LONG;
    results[0].buffer = &member_id;
    results[1].buffer_type = MYSQL_TYPE_STRING;
    results[1].buffer = username;
    results[1].buffer_length = sizeof(username) - 1;
    results[1].length = &username_length;
    results[2].buffer_type = MYSQL_TYPE_STRING;
    results[2].buffer = display_username;
    results[2].buffer_length = sizeof(display_username) - 1;
    results[2].length = &display_username_length;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化群成员列表查询失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, results) != 0) {
        fprintf(stderr, "群成员列表查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    while (mysql_stmt_fetch(stmt) == 0) {
        username[username_length < sizeof(username) ? username_length : sizeof(username) - 1] = '\0';
        display_username[display_username_length < sizeof(display_username) ? display_username_length : sizeof(display_username) - 1] = '\0';
        snprintf(id_text, sizeof(id_text), "%d", member_id);
        if (append_protocol_record(result, result_size, id_text, username, display_username) != 0) {
            fprintf(stderr, "群成员列表结果过长，已截断\n");
            break;
        }
    }
    status = 0;

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

void get_messages(int user_id, int friend_id, char *result, size_t result_size) {
    char query[500];
    snprintf(query, sizeof(query), "SELECT m.content, DATE_FORMAT(m.timestamp, '%%Y-%%m-%%d %%H-%%i-%%s'), u.username FROM messages m "
                                   "JOIN users u ON m.sender_id = u.id WHERE "
                                   "(m.sender_id = %d AND m.receiver_id = %d) OR "
                                   "(m.sender_id = %d AND m.receiver_id = %d) ORDER BY m.timestamp",
             user_id, friend_id, friend_id, user_id);

    if (result_size == 0) {
        return;
    }
    result[0] = '\0';
    if (user_id <= 0 || friend_id <= 0) {
        return;
    }

    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "获取消息失败: %s\n", mysql_error(db_conn));
        pthread_mutex_unlock(&db_mutex);
        return;
    }

    MYSQL_RES *res = mysql_store_result(db_conn);
    if (res == NULL) {
        pthread_mutex_unlock(&db_mutex);
        return;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL) {
        if (append_protocol_record(result, result_size, row[0], row[1], row[2]) != 0) {
            fprintf(stderr, "消息列表结果过长，已截断\n");
            break;
        }
    }
    mysql_free_result(res);

    MYSQL_STMT *stmt = mysql_stmt_init(db_conn);
    if (stmt != NULL) {
        const char *mark_statement =
            "UPDATE messages SET delivered = 1 WHERE receiver_id = ? AND sender_id = ?";
        MYSQL_BIND params[2];
        int bound_user_id = user_id;
        int bound_friend_id = friend_id;

        memset(params, 0, sizeof(params));
        bind_int_param(&params[0], &bound_user_id);
        bind_int_param(&params[1], &bound_friend_id);

        if (mysql_stmt_prepare(stmt, mark_statement, strlen(mark_statement)) == 0 &&
            mysql_stmt_bind_param(stmt, params) == 0) {
            mysql_stmt_execute(stmt);
        }
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
}

int save_message(int sender_id, int receiver_id, const char *content,
                 char *timestamp, size_t timestamp_size) {
    const char *statement =
        "INSERT INTO messages (sender_id, receiver_id, content, delivered) VALUES (?, ?, ?, ?)";
    const char *timestamp_statement =
        "SELECT DATE_FORMAT(timestamp, '%Y-%m-%d %H-%i-%s') FROM messages WHERE id = ?";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[4];
    unsigned long content_length;
    int status = -1;
    int bound_sender_id = sender_id;
    int bound_receiver_id = receiver_id;
    int delivered = is_user_online(receiver_id) ? 1 : 0;
    int saved_message_id = -1;

    if (timestamp != NULL && timestamp_size > 0) {
        timestamp[0] = '\0';
    }

    if (sender_id <= 0 ||
        receiver_id <= 0 ||
        !is_protocol_safe_text(content, 1)) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_sender_id);
    bind_int_param(&params[1], &bound_receiver_id);
    bind_string_param(&params[2], content, &content_length);
    bind_int_param(&params[3], &delivered);

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化消息语句失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "保存消息失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    saved_message_id = (int)mysql_insert_id(db_conn);
    mysql_stmt_close(stmt);
    stmt = NULL;
    if (timestamp != NULL && timestamp_size > 0 &&
        fetch_formatted_timestamp_locked(timestamp_statement,
                                         saved_message_id,
                                         timestamp,
                                         timestamp_size) != 0) {
        format_local_protocol_timestamp(timestamp, timestamp_size);
    }
    status = 0;

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

static int fetch_group_member_ids_locked(int group_id, int *member_ids, int max_members) {
    const char *statement =
        "SELECT user_id FROM group_members WHERE group_id = ? ORDER BY id";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[1];
    MYSQL_BIND results[1];
    int bound_group_id = group_id;
    int member_id = 0;
    int count = 0;

    if (group_id <= 0 || member_ids == NULL || max_members <= 0) {
        return 0;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_group_id);

    memset(results, 0, sizeof(results));
    results[0].buffer_type = MYSQL_TYPE_LONG;
    results[0].buffer = &member_id;

    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化群成员ID查询失败: %s\n", mysql_error(db_conn));
        return 0;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, results) != 0) {
        fprintf(stderr, "群成员ID查询失败: %s\n", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return 0;
    }

    while (count < max_members && mysql_stmt_fetch(stmt) == 0) {
        member_ids[count++] = member_id;
    }

    mysql_stmt_close(stmt);
    return count;
}

int save_group_message(int sender_id, int group_id, const char *content, int *message_id,
                       char *timestamp, size_t timestamp_size) {
    const char *insert_message =
        "INSERT INTO group_messages (group_id, sender_id, content) VALUES (?, ?, ?)";
    const char *insert_delivery =
        "INSERT INTO group_message_deliveries (message_id, user_id, delivered) VALUES (?, ?, ?)";
    const char *timestamp_statement =
        "SELECT DATE_FORMAT(timestamp, '%Y-%m-%d %H-%i-%s') FROM group_messages WHERE id = ?";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[3];
    unsigned long content_length;
    int bound_sender_id = sender_id;
    int bound_group_id = group_id;
    int saved_message_id = -1;
    int member_ids[MAX_CLIENTS];
    int member_count;
    int status = -1;

    if (message_id != NULL) {
        *message_id = -1;
    }
    if (timestamp != NULL && timestamp_size > 0) {
        timestamp[0] = '\0';
    }
    if (sender_id <= 0 ||
        group_id <= 0 ||
        !is_protocol_safe_text(content, 1) ||
        !is_group_member(sender_id, group_id)) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_group_id);
    bind_int_param(&params[1], &bound_sender_id);
    bind_string_param(&params[2], content, &content_length);

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化群消息语句失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, insert_message, strlen(insert_message)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "保存群消息失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    saved_message_id = (int)mysql_insert_id(db_conn);
    mysql_stmt_close(stmt);
    stmt = NULL;
    if (timestamp != NULL && timestamp_size > 0 &&
        fetch_formatted_timestamp_locked(timestamp_statement,
                                         saved_message_id,
                                         timestamp,
                                         timestamp_size) != 0) {
        format_local_protocol_timestamp(timestamp, timestamp_size);
    }

    member_count = fetch_group_member_ids_locked(group_id, member_ids, MAX_CLIENTS);
    for (int i = 0; i < member_count; i++) {
        MYSQL_BIND delivery_params[3];
        int delivered = (member_ids[i] == sender_id || is_user_online(member_ids[i])) ? 1 : 0;
        int bound_message_id = saved_message_id;
        int bound_user_id = member_ids[i];

        memset(delivery_params, 0, sizeof(delivery_params));
        bind_int_param(&delivery_params[0], &bound_message_id);
        bind_int_param(&delivery_params[1], &bound_user_id);
        bind_int_param(&delivery_params[2], &delivered);

        stmt = mysql_stmt_init(db_conn);
        if (stmt == NULL ||
            mysql_stmt_prepare(stmt, insert_delivery, strlen(insert_delivery)) != 0 ||
            mysql_stmt_bind_param(stmt, delivery_params) != 0 ||
            mysql_stmt_execute(stmt) != 0) {
            fprintf(stderr, "保存群消息投递状态失败: %s\n",
                    stmt != NULL ? mysql_stmt_error(stmt) : mysql_error(db_conn));
            if (stmt != NULL) {
                mysql_stmt_close(stmt);
                stmt = NULL;
            }
            goto cleanup;
        }
        mysql_stmt_close(stmt);
        stmt = NULL;
    }

    if (message_id != NULL) {
        *message_id = saved_message_id;
    }
    status = 0;

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

int get_group_messages(int requester_id, int group_id, char *result, size_t result_size) {
    const char *statement =
        "SELECT gm.content, DATE_FORMAT(gm.timestamp, '%Y-%m-%d %H-%i-%s'), u.username "
        "FROM group_messages gm JOIN users u ON gm.sender_id = u.id "
        "WHERE gm.group_id = ? ORDER BY gm.timestamp, gm.id";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[1];
    MYSQL_BIND results[3];
    int bound_group_id = group_id;
    char content[BUFFER_SIZE] = "";
    char timestamp[50] = "";
    char username[50] = "";
    unsigned long content_length = 0;
    unsigned long timestamp_length = 0;
    unsigned long username_length = 0;
    int status = -1;

    if (result_size == 0) {
        return -1;
    }
    result[0] = '\0';
    if (!is_group_member(requester_id, group_id)) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_group_id);

    memset(results, 0, sizeof(results));
    results[0].buffer_type = MYSQL_TYPE_STRING;
    results[0].buffer = content;
    results[0].buffer_length = sizeof(content) - 1;
    results[0].length = &content_length;
    results[1].buffer_type = MYSQL_TYPE_STRING;
    results[1].buffer = timestamp;
    results[1].buffer_length = sizeof(timestamp) - 1;
    results[1].length = &timestamp_length;
    results[2].buffer_type = MYSQL_TYPE_STRING;
    results[2].buffer = username;
    results[2].buffer_length = sizeof(username) - 1;
    results[2].length = &username_length;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化群消息查询失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, results) != 0) {
        fprintf(stderr, "群消息查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    while (mysql_stmt_fetch(stmt) == 0) {
        content[content_length < sizeof(content) ? content_length : sizeof(content) - 1] = '\0';
        timestamp[timestamp_length < sizeof(timestamp) ? timestamp_length : sizeof(timestamp) - 1] = '\0';
        username[username_length < sizeof(username) ? username_length : sizeof(username) - 1] = '\0';
        if (append_protocol_record(result, result_size, content, timestamp, username) != 0) {
            fprintf(stderr, "群消息列表结果过长，已截断\n");
            break;
        }
    }

    mysql_stmt_close(stmt);
    stmt = NULL;

    const char *mark_statement =
        "UPDATE group_message_deliveries d "
        "JOIN group_messages gm ON d.message_id = gm.id "
        "SET d.delivered = 1 WHERE d.user_id = ? AND gm.group_id = ?";
    MYSQL_BIND mark_params[2];
    int bound_requester_id = requester_id;

    memset(mark_params, 0, sizeof(mark_params));
    bind_int_param(&mark_params[0], &bound_requester_id);
    bind_int_param(&mark_params[1], &bound_group_id);

    stmt = mysql_stmt_init(db_conn);
    if (stmt != NULL &&
        mysql_stmt_prepare(stmt, mark_statement, strlen(mark_statement)) == 0 &&
        mysql_stmt_bind_param(stmt, mark_params) == 0) {
        mysql_stmt_execute(stmt);
    }
    status = 0;

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

void get_offline_messages(int user_id, char *result, size_t result_size) {
    const char *private_statement =
        "SELECT sender_id, COUNT(*) FROM messages WHERE receiver_id = ? AND delivered = 0 GROUP BY sender_id";
    const char *group_statement =
        "SELECT gm.group_id, COUNT(*) FROM group_message_deliveries d "
        "JOIN group_messages gm ON d.message_id = gm.id "
        "WHERE d.user_id = ? AND d.delivered = 0 GROUP BY gm.group_id";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[1];
    MYSQL_BIND results[2];
    int bound_user_id = user_id;
    int related_id = 0;
    long long count = 0;
    char id_text[16];
    char count_text[32];

    if (result_size == 0) {
        return;
    }
    result[0] = '\0';
    if (user_id <= 0) {
        return;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_user_id);

    pthread_mutex_lock(&db_mutex);
    for (int pass = 0; pass < 2; pass++) {
        const char *statement = pass == 0 ? private_statement : group_statement;
        const char *type = pass == 0 ? "PRIVATE" : "GROUP";

        stmt = mysql_stmt_init(db_conn);
        if (stmt == NULL) {
            fprintf(stderr, "初始化离线消息查询失败: %s\n", mysql_error(db_conn));
            break;
        }

        memset(results, 0, sizeof(results));
        results[0].buffer_type = MYSQL_TYPE_LONG;
        results[0].buffer = &related_id;
        results[1].buffer_type = MYSQL_TYPE_LONGLONG;
        results[1].buffer = &count;

        if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
            mysql_stmt_bind_param(stmt, params) != 0 ||
            mysql_stmt_execute(stmt) != 0 ||
            mysql_stmt_bind_result(stmt, results) != 0) {
            fprintf(stderr, "离线消息查询失败: %s\n", mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            stmt = NULL;
            break;
        }

        while (mysql_stmt_fetch(stmt) == 0) {
            snprintf(id_text, sizeof(id_text), "%d", related_id);
            snprintf(count_text, sizeof(count_text), "%lld", count);
            if (append_protocol_record(result, result_size, type, id_text, count_text) != 0) {
                fprintf(stderr, "离线消息结果过长，已截断\n");
                break;
            }
        }

        mysql_stmt_close(stmt);
        stmt = NULL;
    }
    pthread_mutex_unlock(&db_mutex);
}

void broadcast_message(int sender_id, const char *message) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].user_id == sender_id) {
            continue;
        }
        send(clients[i].sockfd, message, strlen(message), 0);
    }
    pthread_mutex_unlock(&clients_mutex);
}

void send_to_user(int user_id, const char *message) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].user_id == user_id) {
            send(clients[i].sockfd, message, strlen(message), 0);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

int get_group_member_ids(int group_id, int *member_ids, int max_members) {
    int count;

    pthread_mutex_lock(&db_mutex);
    count = fetch_group_member_ids_locked(group_id, member_ids, max_members);
    pthread_mutex_unlock(&db_mutex);
    return count;
}

void notify_group_members_changed(int group_id, int excluded_user_id) {
    int member_ids[MAX_CLIENTS];
    int member_count;

    if (group_id <= 0) {
        return;
    }

    member_count = get_group_member_ids(group_id, member_ids, MAX_CLIENTS);
    for (int i = 0; i < member_count; i++) {
        if (member_ids[i] != excluded_user_id) {
            send_to_user(member_ids[i], "ADD_GROUP_MEMBER_SUCCESS");
        }
    }
}

void send_to_group_members(int group_id, const char *message) {
    int member_ids[MAX_CLIENTS];
    int member_count = get_group_member_ids(group_id, member_ids, MAX_CLIENTS);

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        for (int j = 0; j < member_count; j++) {
            if (clients[i].user_id == member_ids[j]) {
                send(clients[i].sockfd, message, strlen(message), 0);
                break;
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

int get_friend_ids(int user_id, int *friend_ids, int max_friends) {
    const char *statement =
        "SELECT friend_id FROM friends WHERE user_id = ? AND status = 1 ORDER BY id";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[1];
    MYSQL_BIND results[1];
    int bound_user_id = user_id;
    int friend_id = 0;
    int count = 0;

    if (user_id <= 0 || friend_ids == NULL || max_friends <= 0) {
        return 0;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_user_id);

    memset(results, 0, sizeof(results));
    results[0].buffer_type = MYSQL_TYPE_LONG;
    results[0].buffer = &friend_id;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化好友ID查询失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, results) != 0) {
        fprintf(stderr, "好友ID查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    while (count < max_friends && mysql_stmt_fetch(stmt) == 0) {
        friend_ids[count++] = friend_id;
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return count;
}

void notify_friends_status(int user_id, const char *command, const char *username) {
    int friend_ids[MAX_CLIENTS];
    int friend_count;
    char message[BUFFER_SIZE];

    if (user_id <= 0 || command == NULL || username == NULL) {
        return;
    }

    friend_count = get_friend_ids(user_id, friend_ids, MAX_CLIENTS);
    snprintf(message, sizeof(message), "%s:%d:%s", command, user_id, username);
    for (int i = 0; i < friend_count; i++) {
        if (!has_block_between(user_id, friend_ids[i])) {
            send_to_user(friend_ids[i], message);
        }
    }
}

void *handle_client(void *arg) {
    Client *client = (Client *)arg;
    char buffer[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    printf("新客户端连接: %s:%d\n", inet_ntoa(client->addr.sin_addr), ntohs(client->addr.sin_port));

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = recv(client->sockfd, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_read <= 0) {
            printf("客户端断开连接: %s:%d\n", inet_ntoa(client->addr.sin_addr), ntohs(client->addr.sin_port));
            break;
        }

        printf("收到消息: %s\n", buffer);

        if (HAS_PREFIX(buffer, "REGISTER:")) {
            char payload[BUFFER_SIZE];
            char *first_comma;
            char *username;
            char *password;

            snprintf(payload, sizeof(payload), "%s", PREFIX_PAYLOAD(buffer, "REGISTER:"));
            first_comma = strchr(payload, ',');
            if (first_comma != NULL) {
                *first_comma = '\0';
                username = payload;
                password = first_comma + 1;

                int user_id = register_user(username, password);
                if (user_id > 0) {
                    snprintf(response, sizeof(response), "REGISTER_SUCCESS:%d", user_id);
                } else if (user_id == REGISTER_ERROR_DUPLICATE) {
                    snprintf(response, sizeof(response), "REGISTER_FAILED_DUPLICATE_USERNAME");
                } else if (user_id == REGISTER_ERROR_INVALID_INPUT) {
                    snprintf(response, sizeof(response), "REGISTER_FAILED_INVALID_INPUT");
                } else {
                    snprintf(response, sizeof(response), "REGISTER_FAILED_SERVER_ERROR");
                }
            } else {
                snprintf(response, sizeof(response), "REGISTER_FAILED_INVALID_INPUT");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "LOGIN:")) {
            char username[50], password[50];
            int user_id;
            int login_success = 0;

            if (sscanf(PREFIX_PAYLOAD(buffer, "LOGIN:"), "%49[^,],%49[^\n]", username, password) == 2 &&
                login_user(username, password, &user_id) == 0) {
                client->user_id = user_id;
                strncpy(client->username, username, sizeof(client->username) - 1);
                client->username[sizeof(client->username) - 1] = '\0';
                update_client_session(client->sockfd, user_id, username);
                snprintf(response, sizeof(response), "LOGIN_SUCCESS:%d:%s", user_id, username);
                login_success = 1;
            } else {
                snprintf(response, sizeof(response), "LOGIN_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
            if (login_success) {
                notify_friends_status(client->user_id, "FRIEND_ONLINE", client->username);
            }
        }
        else if (HAS_PREFIX(buffer, "ADDFRIEND:")) {
            int requested_user_id, friend_id;
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "ADDFRIEND:"), "%d,%d", &requested_user_id, &friend_id) == 2 &&
                add_friend(client->user_id, friend_id) == 0) {
                if (requested_user_id != client->user_id) {
                    printf("忽略客户端声明的好友操作用户ID: %d，使用登录会话ID: %d\n",
                           requested_user_id, client->user_id);
                }
                snprintf(response, sizeof(response), "ADDFRIEND_SUCCESS");
            } else {
                snprintf(response, sizeof(response), "ADDFRIEND_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
            if (strcmp(response, "ADDFRIEND_SUCCESS") == 0 && friend_id != client->user_id) {
                send_to_user(friend_id, "ADDFRIEND_SUCCESS");
            }
        }
        else if (HAS_PREFIX(buffer, "ADDFRIEND_USERNAME:")) {
            char friend_username[50];
            int friend_id = -1;
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "ADDFRIEND_USERNAME:"), "%49[^\n]", friend_username) == 1 &&
                get_user_id_by_username(friend_username, &friend_id) == 0 &&
                add_friend_by_username(client->user_id, friend_username) == 0) {
                snprintf(response, sizeof(response), "ADDFRIEND_SUCCESS");
            } else {
                snprintf(response, sizeof(response), "ADDFRIEND_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
            if (strcmp(response, "ADDFRIEND_SUCCESS") == 0 && friend_id != client->user_id) {
                send_to_user(friend_id, "ADDFRIEND_SUCCESS");
            }
        }
        else if (HAS_PREFIX(buffer, "FRIENDS:")) {
            int requested_user_id;
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "FRIENDS:"), "%d", &requested_user_id) == 1) {
                char friends[BUFFER_SIZE];
                if (requested_user_id != client->user_id) {
                    printf("忽略客户端声明的好友列表用户ID: %d，使用登录会话ID: %d\n",
                           requested_user_id, client->user_id);
                }
                get_friends(client->user_id, friends, sizeof(friends));
                snprintf(response, sizeof(response), "FRIENDS_LIST:%s", friends);
            } else {
                snprintf(response, sizeof(response), "FRIENDS_LIST:");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "MESSAGES:")) {
            int requested_user_id, friend_id;
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "MESSAGES:"), "%d,%d", &requested_user_id, &friend_id) == 2 &&
                are_friends(client->user_id, friend_id)) {
                char messages[BUFFER_SIZE];
                if (requested_user_id != client->user_id) {
                    printf("忽略客户端声明的消息查询用户ID: %d，使用登录会话ID: %d\n",
                           requested_user_id, client->user_id);
                }
                get_messages(client->user_id, friend_id, messages, sizeof(messages) - strlen("MESSAGES_LIST:"));
                snprintf(response, sizeof(response), "MESSAGES_LIST:%s", messages);
            } else {
                snprintf(response, sizeof(response), "MESSAGES_LIST:");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "SEND:")) {
            int sender_id, requested_sender_id, receiver_id;
            char content[BUFFER_SIZE];
            char timestamp[50] = "";

            if (client->user_id <= 0) {
                snprintf(response, sizeof(response), "SEND_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            if (sscanf(PREFIX_PAYLOAD(buffer, "SEND:"),
                       "%d,%d,%1023[^\n]", &requested_sender_id, &receiver_id, content) != 3) {
                snprintf(response, sizeof(response), "SEND_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            sender_id = client->user_id;
            if (requested_sender_id != sender_id) {
                printf("忽略客户端声明的发送者ID: %d，使用登录会话ID: %d\n",
                       requested_sender_id, sender_id);
            }
            if (!are_friends(sender_id, receiver_id)) {
                snprintf(response, sizeof(response), "SEND_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }
            if (has_block_between(sender_id, receiver_id)) {
                snprintf(response, sizeof(response), "SEND_FAILED_BLOCKED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }
            if (!can_send_private_message(sender_id, receiver_id) ||
                save_message(sender_id, receiver_id, content, timestamp, sizeof(timestamp)) != 0) {
                snprintf(response, sizeof(response), "SEND_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            if (timestamp[0] == '\0') {
                format_local_protocol_timestamp(timestamp, sizeof(timestamp));
            }
            snprintf(response, sizeof(response), "NEW_MESSAGE:%d:%s:%s:%s",
                     sender_id, client->username, timestamp, content);
            send_to_user(receiver_id, response);
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "CREATE_GROUP:")) {
            char group_name[80];
            int group_id = -1;

            if (client->user_id <= 0) {
                snprintf(response, sizeof(response), "CREATE_GROUP_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            snprintf(group_name, sizeof(group_name), "%s", PREFIX_PAYLOAD(buffer, "CREATE_GROUP:"));

            group_id = create_group(client->user_id, group_name, NULL);
            if (group_id > 0) {
                snprintf(response, sizeof(response), "CREATE_GROUP_SUCCESS:%d", group_id);
            } else {
                snprintf(response, sizeof(response), "CREATE_GROUP_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
            if (group_id > 0) {
                notify_group_members_changed(group_id, client->user_id);
            }
        }
        else if (HAS_PREFIX(buffer, "GROUPS:")) {
            int requested_user_id;
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "GROUPS:"), "%d", &requested_user_id) == 1) {
                char groups[BUFFER_SIZE];
                if (requested_user_id != client->user_id) {
                    printf("忽略客户端声明的群列表用户ID: %d，使用登录会话ID: %d\n",
                           requested_user_id, client->user_id);
                }
                get_groups(client->user_id, groups, sizeof(groups));
                snprintf(response, sizeof(response), "GROUPS_LIST:%s", groups);
            } else {
                snprintf(response, sizeof(response), "GROUPS_LIST:");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "GROUP_MEMBERS:")) {
            int group_id;
            char members[BUFFER_SIZE];
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "GROUP_MEMBERS:"), "%d", &group_id) == 1 &&
                get_group_members(client->user_id, group_id, members, sizeof(members)) == 0) {
                snprintf(response, sizeof(response), "GROUP_MEMBERS_LIST:%s", members);
            } else {
                snprintf(response, sizeof(response), "GROUP_MEMBERS_LIST:");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "ADD_GROUP_MEMBER:")) {
            int group_id, new_member_id;
            int member_added = 0;
            int add_status = -1;
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "ADD_GROUP_MEMBER:"), "%d,%d", &group_id, &new_member_id) == 2) {
                add_status = add_group_member(client->user_id, group_id, new_member_id);
            }
            if (add_status == 0) {
                snprintf(response, sizeof(response), "ADD_GROUP_MEMBER_SUCCESS");
                member_added = 1;
            } else if (add_status == GROUP_MEMBER_ADD_ALREADY_MEMBER) {
                snprintf(response, sizeof(response), "ADD_GROUP_MEMBER_FAILED_ALREADY_MEMBER");
            } else if (add_status == GROUP_MEMBER_ADD_BLOCKED) {
                snprintf(response, sizeof(response), "ADD_GROUP_MEMBER_FAILED_BLOCKED");
            } else {
                snprintf(response, sizeof(response), "ADD_GROUP_MEMBER_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
            if (member_added) {
                notify_group_members_changed(group_id, client->user_id);
            }
        }
        else if (HAS_PREFIX(buffer, "ADD_GROUP_MEMBER_USERNAME:")) {
            int group_id;
            int new_member_id = -1;
            int member_added = 0;
            int add_status = -1;
            char new_member_username[50];
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "ADD_GROUP_MEMBER_USERNAME:"),
                       "%d,%49[^\n]", &group_id, new_member_username) == 2 &&
                get_user_id_by_username(new_member_username, &new_member_id) == 0) {
                add_status = add_group_member(client->user_id, group_id, new_member_id);
            }
            if (add_status == 0) {
                snprintf(response, sizeof(response), "ADD_GROUP_MEMBER_SUCCESS");
                member_added = 1;
            } else if (add_status == GROUP_MEMBER_ADD_ALREADY_MEMBER) {
                snprintf(response, sizeof(response), "ADD_GROUP_MEMBER_FAILED_ALREADY_MEMBER");
            } else if (add_status == GROUP_MEMBER_ADD_BLOCKED) {
                snprintf(response, sizeof(response), "ADD_GROUP_MEMBER_FAILED_BLOCKED");
            } else {
                snprintf(response, sizeof(response), "ADD_GROUP_MEMBER_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
            if (member_added) {
                notify_group_members_changed(group_id, client->user_id);
            }
        }
        else if (HAS_PREFIX(buffer, "GROUP_MESSAGES:")) {
            int group_id;
            char messages[BUFFER_SIZE];
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "GROUP_MESSAGES:"), "%d", &group_id) == 1 &&
                get_group_messages(client->user_id, group_id, messages,
                                   sizeof(messages) - strlen("GROUP_MESSAGES_LIST:")) == 0) {
                snprintf(response, sizeof(response), "GROUP_MESSAGES_LIST:%s", messages);
            } else {
                snprintf(response, sizeof(response), "GROUP_MESSAGES_LIST:");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "SEND_GROUP:")) {
            int requested_sender_id, sender_id, group_id, message_id;
            char content[BUFFER_SIZE];
            char timestamp[50] = "";

            if (client->user_id <= 0 ||
                sscanf(PREFIX_PAYLOAD(buffer, "SEND_GROUP:"),
                       "%d,%d,%1023[^\n]", &requested_sender_id, &group_id, content) != 3) {
                snprintf(response, sizeof(response), "SEND_GROUP_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            sender_id = client->user_id;
            if (requested_sender_id != sender_id) {
                printf("忽略客户端声明的群消息发送者ID: %d，使用登录会话ID: %d\n",
                       requested_sender_id, sender_id);
            }

            if (save_group_message(sender_id, group_id, content, &message_id,
                                   timestamp, sizeof(timestamp)) != 0) {
                snprintf(response, sizeof(response), "SEND_GROUP_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            if (timestamp[0] == '\0') {
                format_local_protocol_timestamp(timestamp, sizeof(timestamp));
            }
            snprintf(response, sizeof(response), "NEW_GROUP_MESSAGE:%d:%d:%s:%s:%s",
                     group_id, sender_id, client->username, timestamp, content);
            send_to_group_members(group_id, response);
        }
        else if (HAS_PREFIX(buffer, "BLOCK_USER:")) {
            int requested_user_id, blocked_id;
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "BLOCK_USER:"), "%d,%d", &requested_user_id, &blocked_id) == 2 &&
                block_user(client->user_id, blocked_id) == 0) {
                if (requested_user_id != client->user_id) {
                    printf("忽略客户端声明的屏蔽操作用户ID: %d，使用登录会话ID: %d\n",
                           requested_user_id, client->user_id);
                }
                snprintf(response, sizeof(response), "BLOCK_USER_SUCCESS");
            } else {
                snprintf(response, sizeof(response), "BLOCK_USER_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "BLOCK_USER_USERNAME:")) {
            char blocked_username[50];
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "BLOCK_USER_USERNAME:"), "%49[^\n]", blocked_username) == 1 &&
                block_user_by_username(client->user_id, blocked_username) == 0) {
                snprintf(response, sizeof(response), "BLOCK_USER_SUCCESS");
            } else {
                snprintf(response, sizeof(response), "BLOCK_USER_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "UNBLOCK_USER:")) {
            int requested_user_id, blocked_id;
            int unblock_status = -1;
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "UNBLOCK_USER:"), "%d,%d", &requested_user_id, &blocked_id) == 2) {
                unblock_status = unblock_user(client->user_id, blocked_id);
                if (requested_user_id != client->user_id) {
                    printf("忽略客户端声明的解除屏蔽用户ID: %d，使用登录会话ID: %d\n",
                           requested_user_id, client->user_id);
                }
            }
            if (unblock_status == 0) {
                snprintf(response, sizeof(response), "UNBLOCK_USER_SUCCESS");
            } else if (unblock_status == UNBLOCK_USER_BLOCKED_BY_TARGET) {
                snprintf(response, sizeof(response), "UNBLOCK_USER_FAILED_BLOCKED_BY_TARGET");
            } else if (unblock_status == UNBLOCK_USER_NOT_BLOCKED) {
                snprintf(response, sizeof(response), "UNBLOCK_USER_FAILED_NOT_BLOCKED");
            } else {
                snprintf(response, sizeof(response), "UNBLOCK_USER_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "UNBLOCK_USER_USERNAME:")) {
            char blocked_username[50];
            int unblock_status = -1;
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "UNBLOCK_USER_USERNAME:"), "%49[^\n]", blocked_username) == 1) {
                unblock_status = unblock_user_by_username(client->user_id, blocked_username);
            }
            if (unblock_status == 0) {
                snprintf(response, sizeof(response), "UNBLOCK_USER_SUCCESS");
            } else if (unblock_status == UNBLOCK_USER_BLOCKED_BY_TARGET) {
                snprintf(response, sizeof(response), "UNBLOCK_USER_FAILED_BLOCKED_BY_TARGET");
            } else if (unblock_status == UNBLOCK_USER_NOT_BLOCKED) {
                snprintf(response, sizeof(response), "UNBLOCK_USER_FAILED_NOT_BLOCKED");
            } else {
                snprintf(response, sizeof(response), "UNBLOCK_USER_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (HAS_PREFIX(buffer, "OFFLINE_MESSAGES:")) {
            int requested_user_id;
            if (client->user_id > 0 &&
                sscanf(PREFIX_PAYLOAD(buffer, "OFFLINE_MESSAGES:"), "%d", &requested_user_id) == 1) {
                char offline[BUFFER_SIZE];
                if (requested_user_id != client->user_id) {
                    printf("忽略客户端声明的离线消息用户ID: %d，使用登录会话ID: %d\n",
                           requested_user_id, client->user_id);
                }
                get_offline_messages(client->user_id, offline, sizeof(offline));
                snprintf(response, sizeof(response), "OFFLINE_MESSAGES_LIST:%s", offline);
            } else {
                snprintf(response, sizeof(response), "OFFLINE_MESSAGES_LIST:");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strcmp(buffer, "QUIT") == 0) {
            break;
        }
    }

    int disconnected_user_id = client->user_id;
    char disconnected_username[50];
    snprintf(disconnected_username, sizeof(disconnected_username), "%s", client->username);

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].sockfd == client->sockfd) {
            for (int j = i; j < client_count - 1; j++) {
                clients[j] = clients[j + 1];
            }
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (disconnected_user_id > 0 && !is_user_online(disconnected_user_id)) {
        notify_friends_status(disconnected_user_id, "FRIEND_OFFLINE", disconnected_username);
    }

    close(client->sockfd);
    free(client);
    return NULL;
}

int main(void) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    pthread_mutex_init(&clients_mutex, NULL);
    pthread_mutex_init(&db_mutex, NULL);

    if (install_signal_handlers() != 0) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    init_database();
    create_tables();

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }

#ifdef SO_REUSEPORT
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEPORT");
        exit(EXIT_FAILURE);
    }
#endif

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("服务器启动成功，监听端口 %d... 按 Ctrl+C 可停止服务器\n", PORT);

    while (server_running) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            if (!server_running || errno == EINTR) {
                break;
            }
            perror("accept");
            continue;
        }

        if (client_count >= MAX_CLIENTS) {
            printf("客户端数量已达上限\n");
            close(new_socket);
            continue;
        }

        Client *client = (Client *)malloc(sizeof(Client));
        client->sockfd = new_socket;
        client->addr = address;
        client->user_id = -1;
        client->username[0] = '\0';

        pthread_mutex_lock(&clients_mutex);
        clients[client_count++] = *client;
        pthread_mutex_unlock(&clients_mutex);

        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, client) != 0) {
            perror("pthread_create");
            free(client);
        }
        pthread_detach(thread);
    }

    printf("\n正在关闭服务器...\n");
    close(server_fd);
    mysql_close(db_conn);
    pthread_mutex_destroy(&db_mutex);
    pthread_mutex_destroy(&clients_mutex);
    return 0;
}
