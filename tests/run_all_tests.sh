#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC_BIN="${CC:-gcc}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/linuxchat-all-tests.XXXXXX")"
RUN_DB_TESTS="${LINUXCHAT_RUN_DB_TESTS:-0}"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

if [[ "${1:-}" == "--with-db" ]]; then
    RUN_DB_TESTS=1
fi

find_mysql_config() {
    if [[ -n "${MYSQL_CONFIG:-}" && -x "$MYSQL_CONFIG" ]]; then
        printf '%s\n' "$MYSQL_CONFIG"
        return
    fi

    if command -v mysql_config >/dev/null 2>&1; then
        command -v mysql_config
        return
    fi

    if [[ -x /usr/local/mysql/bin/mysql_config ]]; then
        printf '%s\n' /usr/local/mysql/bin/mysql_config
        return
    fi

    local candidate
    candidate="$(compgen -G '/usr/local/mysql-*/bin/mysql_config' | head -n 1 || true)"
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return
    fi

    printf 'mysql_config not found. Set MYSQL_CONFIG=/path/to/mysql_config.\n' >&2
    exit 1
}

assert_contains() {
    local file="$1"
    local pattern="$2"

    if ! grep -Fq "$pattern" "$file"; then
        printf 'Expected to find pattern in %s:\n%s\n' "$file" "$pattern" >&2
        exit 1
    fi
}

assert_not_contains() {
    local file="$1"
    local pattern="$2"

    if grep -Fq "$pattern" "$file"; then
        printf 'Unexpected pattern found in %s:\n%s\n' "$file" "$pattern" >&2
        exit 1
    fi
}

assert_not_matches() {
    local file="$1"
    local pattern="$2"

    if grep -Eq "$pattern" "$file"; then
        printf 'Unexpected regex pattern found in %s:\n%s\n' "$file" "$pattern" >&2
        exit 1
    fi
}

compile_mysql_test() {
    local source_file="$1"
    local output_file="$2"

    "$CC_BIN" -Wall -Wextra -Wpedantic \
        "$source_file" \
        -o "$output_file" \
        "${MYSQL_CFLAGS[@]}" "${MYSQL_LIBS[@]}" "${MYSQL_RPATH_FLAGS[@]}" -lpthread
}

run_with_runtime_paths() {
    local binary="$1"
    local runtime_library_path

    runtime_library_path="$(IFS=:; printf '%s' "${RUNTIME_LIBRARY_DIRS[*]}")"
    DYLD_LIBRARY_PATH="${runtime_library_path}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    LD_LIBRARY_PATH="${runtime_library_path}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$binary"
}

require_db_env() {
    local missing=0

    if [[ -z "${LINUXCHAT_TEST_DB_USER:-}" ]]; then
        printf 'Missing LINUXCHAT_TEST_DB_USER for database integration tests.\n' >&2
        missing=1
    fi

    if [[ -z "${LINUXCHAT_TEST_DB_NAME:-}" ]]; then
        printf 'Missing LINUXCHAT_TEST_DB_NAME for database integration tests.\n' >&2
        missing=1
    fi

    if [[ "$missing" -ne 0 ]]; then
        printf 'Required database env: LINUXCHAT_TEST_DB_USER, LINUXCHAT_TEST_DB_NAME.\n' >&2
        printf 'Optional: LINUXCHAT_TEST_DB_PASSWORD, LINUXCHAT_TEST_DB_HOST, LINUXCHAT_TEST_DB_PORT.\n' >&2
        printf 'The database name must contain \"test\" because the test drops users/messages/friends tables.\n' >&2
        exit 1
    fi
}

MYSQL_CONFIG_BIN="$(find_mysql_config)"

# mysql_config/pkg-config return shell-style flag lists; keep word splitting intentional.
# shellcheck disable=SC2207
GTK_FLAGS=($(pkg-config --cflags --libs gtk+-3.0))
# shellcheck disable=SC2207
MYSQL_CFLAGS=($("$MYSQL_CONFIG_BIN" --cflags))
# shellcheck disable=SC2207
MYSQL_LIBS=($("$MYSQL_CONFIG_BIN" --libs))
MYSQL_LIBDIR="$("$MYSQL_CONFIG_BIN" --variable=pkglibdir 2>/dev/null || true)"
MYSQL_RPATH_FLAGS=()
RUNTIME_LIBRARY_DIRS=()

add_rpath() {
    local dir="$1"

    if [[ -d "$dir" ]]; then
        MYSQL_RPATH_FLAGS+=(-Wl,-rpath,"$dir")
        RUNTIME_LIBRARY_DIRS+=("$dir")
    fi
}

[[ -n "$MYSQL_LIBDIR" ]] && add_rpath "$MYSQL_LIBDIR"
add_rpath /opt/homebrew/lib
add_rpath /opt/homebrew/opt/openssl@3/lib
add_rpath /usr/local/lib
add_rpath /usr/local/opt/openssl@3/lib

printf '== Compile checks ==\n'
"$CC_BIN" -Wall -Wextra -Wpedantic \
    "$ROOT_DIR/client/client.c" \
    -o "$TMP_DIR/client" \
    "${GTK_FLAGS[@]}" -lpthread

"$CC_BIN" -Wall -Wextra -Wpedantic \
    "$ROOT_DIR/server/server.c" \
    -o "$TMP_DIR/server" \
    "${MYSQL_CFLAGS[@]}" "${MYSQL_LIBS[@]}" "${MYSQL_RPATH_FLAGS[@]}" -lpthread

printf '== Unit and contract tests ==\n'
compile_mysql_test "$ROOT_DIR/tests/test_server_session.c" "$TMP_DIR/test_server_session"
run_with_runtime_paths "$TMP_DIR/test_server_session"

compile_mysql_test "$ROOT_DIR/tests/test_p1_stability.c" "$TMP_DIR/test_p1_stability"
run_with_runtime_paths "$TMP_DIR/test_p1_stability"

compile_mysql_test "$ROOT_DIR/tests/test_server_core.c" "$TMP_DIR/test_server_core"
run_with_runtime_paths "$TMP_DIR/test_server_core"

compile_mysql_test "$ROOT_DIR/tests/test_p2_security_contracts.c" "$TMP_DIR/test_p2_security_contracts"
run_with_runtime_paths "$TMP_DIR/test_p2_security_contracts"

CLIENT_SOURCE="$ROOT_DIR/client/client.c"
SERVER_SOURCE="$ROOT_DIR/server/server.c"
INIT_SQL="$ROOT_DIR/database/init.sql"

assert_contains "$CLIENT_SOURCE" "gtk_stack_add_named(GTK_STACK(main_stack), login_window, \"login\")"
assert_contains "$CLIENT_SOURCE" "gtk_stack_add_named(GTK_STACK(main_stack), chat_window, \"chat\")"
assert_contains "$CLIENT_SOURCE" "gdk_threads_add_idle(parse_friends_list, g_strdup(buffer))"
assert_contains "$CLIENT_SOURCE" "gdk_threads_add_idle(parse_groups_list, g_strdup(buffer))"
assert_contains "$CLIENT_SOURCE" "gdk_threads_add_idle(parse_group_messages_list, g_strdup(buffer))"
assert_contains "$CLIENT_SOURCE" "SEND_GROUP:%d,%d,%s"
assert_contains "$CLIENT_SOURCE" "ADDFRIEND_USERNAME:%s"
assert_contains "$CLIENT_SOURCE" "ADD_GROUP_MEMBER_USERNAME:%d,%s"
assert_contains "$CLIENT_SOURCE" "BLOCK_USER_USERNAME:%s"
assert_contains "$CLIENT_SOURCE" "UNBLOCK_USER_USERNAME:%s"
assert_contains "$CLIENT_SOURCE" "请输入好友用户名"
assert_contains "$CLIENT_SOURCE" "请输入成员用户名"
assert_contains "$CLIENT_SOURCE" "请输入用户名"
assert_contains "$CLIENT_SOURCE" "#define HAS_PREFIX(text, prefix)"
assert_contains "$CLIENT_SOURCE" "#define PREFIX_PAYLOAD(text, prefix)"
assert_contains "$CLIENT_SOURCE" "HAS_PREFIX(buffer, \"REGISTER_SUCCESS:\")"
assert_contains "$CLIENT_SOURCE" "PREFIX_PAYLOAD(buffer, \"FRIENDS_LIST:\")"
assert_contains "$CLIENT_SOURCE" "REGISTER_FAILED_DUPLICATE_USERNAME"
assert_contains "$CLIENT_SOURCE" "REGISTER_FAILED_INVALID_INPUT"
assert_contains "$CLIENT_SOURCE" "gtk_entry_set_text(username_entry, \"\")"
assert_contains "$CLIENT_SOURCE" "gtk_entry_set_text(password_entry, \"\")"
assert_contains "$CLIENT_SOURCE" "gtk_entry_set_text(nickname_entry, \"\")"
assert_contains "$CLIENT_SOURCE" "strlen(nickname) > 0 && !is_client_protocol_safe(nickname, FALSE)"
assert_contains "$CLIENT_SOURCE" "%1023[^:]:%49[^:]:%49[^;];"
assert_contains "$CLIENT_SOURCE" "%d:%49[^:]:%1023[^\\n]"
assert_contains "$CLIENT_SOURCE" "is_client_protocol_safe"
assert_contains "$CLIENT_SOURCE" "%d:%49[^:]:%49[^\\n]"
assert_not_contains "$CLIENT_SOURCE" "strncmp(buffer, \"REGISTER_SUCCESS:\", 18)"
assert_not_matches "$CLIENT_SOURCE" "buffer[[:space:]]*\\+[[:space:]]*[0-9]+"
assert_not_matches "$CLIENT_SOURCE" "strncmp\\(buffer,[[:space:]]*\"[^\"]+\",[[:space:]]*[0-9]+\\)"
assert_not_contains "$CLIENT_SOURCE" "请输入好友ID"
assert_not_contains "$CLIENT_SOURCE" "请输入成员ID"
assert_not_contains "$CLIENT_SOURCE" "请输入用户ID"
assert_not_contains "$CLIENT_SOURCE" "(GSourceFunc)parse_"
assert_not_contains "$CLIENT_SOURCE" "sprintf("

assert_contains "$SERVER_SOURCE" "pthread_mutex_t db_mutex"
assert_contains "$SERVER_SOURCE" "START TRANSACTION"
assert_contains "$SERVER_SOURCE" "COMMIT"
assert_contains "$SERVER_SOURCE" "ROLLBACK"
assert_contains "$SERVER_SOURCE" "ON DELETE CASCADE"
assert_contains "$SERVER_SOURCE" "UNIQUE KEY unique_friend (user_id, friend_id)"
assert_contains "$SERVER_SOURCE" "UNIQUE KEY unique_group_member (group_id, user_id)"
assert_contains "$SERVER_SOURCE" "UNIQUE KEY unique_block (blocker_id, blocked_id)"
assert_contains "$SERVER_SOURCE" "CREATE TABLE IF NOT EXISTS group_messages"
assert_contains "$SERVER_SOURCE" "CREATE_GROUP:"
assert_contains "$SERVER_SOURCE" "SEND_GROUP:"
assert_contains "$SERVER_SOURCE" "OFFLINE_MESSAGES:"
assert_contains "$SERVER_SOURCE" "volatile sig_atomic_t server_running"
assert_contains "$SERVER_SOURCE" "install_signal_handlers"
assert_contains "$SERVER_SOURCE" "sigaction(SIGINT"
assert_contains "$SERVER_SOURCE" "sigaction(SIGTERM"
assert_contains "$SERVER_SOURCE" "while (server_running)"
assert_contains "$SERVER_SOURCE" "errno == EINTR"
assert_contains "$SERVER_SOURCE" "DATE_FORMAT(m.timestamp, '%%Y-%%m-%%d %%H-%%i-%%s')"
assert_contains "$SERVER_SOURCE" "DATE_FORMAT(gm.timestamp, '%Y-%m-%d %H-%i-%s')"
assert_contains "$SERVER_SOURCE" "if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)"
assert_contains "$SERVER_SOURCE" "VALUES (?, SHA2(?, 256), ?)"
assert_contains "$SERVER_SOURCE" "password = SHA2(?, 256)"
assert_contains "$SERVER_SOURCE" "REGISTER_ERROR_DUPLICATE"
assert_contains "$SERVER_SOURCE" "REGISTER_ERROR_INVALID_INPUT"
assert_contains "$SERVER_SOURCE" "REGISTER_FAILED_DUPLICATE_USERNAME"
assert_contains "$SERVER_SOURCE" "REGISTER_FAILED_INVALID_INPUT"
assert_contains "$SERVER_SOURCE" "effective_nickname"
assert_contains "$SERVER_SOURCE" "get_user_id_by_username(const char *username, int *user_id)"
assert_contains "$SERVER_SOURCE" "SELECT id FROM users WHERE username = ?"
assert_contains "$SERVER_SOURCE" "ADDFRIEND_USERNAME:"
assert_contains "$SERVER_SOURCE" "ADD_GROUP_MEMBER_USERNAME:"
assert_contains "$SERVER_SOURCE" "BLOCK_USER_USERNAME:"
assert_contains "$SERVER_SOURCE" "UNBLOCK_USER_USERNAME:"
assert_contains "$SERVER_SOURCE" "add_friend_by_username(client->user_id, friend_username)"
assert_contains "$SERVER_SOURCE" "add_group_member_by_username(client->user_id, group_id, new_member_username)"
assert_contains "$SERVER_SOURCE" "block_user_by_username(client->user_id, blocked_username)"
assert_contains "$SERVER_SOURCE" "unblock_user_by_username(client->user_id, blocked_username)"
assert_contains "$SERVER_SOURCE" "mysql_stmt_prepare(stmt, statement, strlen(statement))"
assert_contains "$SERVER_SOURCE" "#define HAS_PREFIX(text, prefix)"
assert_contains "$SERVER_SOURCE" "#define PREFIX_PAYLOAD(text, prefix)"
assert_contains "$SERVER_SOURCE" "HAS_PREFIX(buffer, \"ADDFRIEND_USERNAME:\")"
assert_contains "$SERVER_SOURCE" "PREFIX_PAYLOAD(buffer, \"ADDFRIEND_USERNAME:\")"
assert_contains "$SERVER_SOURCE" "is_protocol_safe_text(content, 1)"
assert_contains "$SERVER_SOURCE" "add_friend(client->user_id, friend_id)"
assert_contains "$SERVER_SOURCE" "get_friends(client->user_id, friends, sizeof(friends))"
assert_contains "$SERVER_SOURCE" "get_messages(client->user_id, friend_id, messages"
assert_contains "$SERVER_SOURCE" "can_send_private_message(sender_id, receiver_id)"
assert_contains "$SERVER_SOURCE" "is_group_member(sender_id, group_id)"
assert_not_contains "$SERVER_SOURCE" "char messages[BUFFER_SIZE * 10]"
assert_not_matches "$SERVER_SOURCE" "buffer[[:space:]]*\\+[[:space:]]*[0-9]+"
assert_not_matches "$SERVER_SOURCE" "strncmp\\(buffer,[[:space:]]*\"[^\"]+\",[[:space:]]*[0-9]+\\)"
assert_not_contains "$SERVER_SOURCE" "sprintf("
assert_not_contains "$SERVER_SOURCE" "strcat("
assert_not_contains "$SERVER_SOURCE" "strcpy("
assert_not_contains "$SERVER_SOURCE" "password='%s'"
assert_not_contains "$SERVER_SOURCE" "VALUES ('%s'"

assert_contains "$INIT_SQL" "ON DELETE CASCADE"
assert_contains "$INIT_SQL" "UNIQUE KEY unique_friend (user_id, friend_id)"
assert_contains "$INIT_SQL" "UNIQUE KEY unique_group_member (group_id, user_id)"
assert_contains "$INIT_SQL" "UNIQUE KEY unique_block (blocker_id, blocked_id)"
assert_contains "$INIT_SQL" "CREATE TABLE IF NOT EXISTS group_messages"
assert_contains "$INIT_SQL" "CREATE TABLE IF NOT EXISTS group_message_deliveries"
assert_contains "$INIT_SQL" "INSERT IGNORE INTO users"
assert_contains "$INIT_SQL" "INSERT IGNORE INTO friends"
assert_contains "$INIT_SQL" "INSERT IGNORE INTO chat_groups"
assert_contains "$INIT_SQL" "SHA2('admin123', 256)"

if [[ "$RUN_DB_TESTS" == "1" ]]; then
    printf '== Database integration tests ==\n'
    require_db_env
    compile_mysql_test "$ROOT_DIR/tests/test_database_integration.c" "$TMP_DIR/test_database_integration"
    run_with_runtime_paths "$TMP_DIR/test_database_integration"
else
    printf '== Database integration tests skipped ==\n'
    printf 'Run with ./tests/run_all_tests.sh --with-db after setting a disposable MySQL database:\n'
    printf '  LINUXCHAT_TEST_DB_USER, LINUXCHAT_TEST_DB_NAME\n'
    printf 'Optional:\n'
    printf '  LINUXCHAT_TEST_DB_PASSWORD, LINUXCHAT_TEST_DB_HOST, LINUXCHAT_TEST_DB_PORT\n'
fi

printf 'All non-database tests passed.\n'
