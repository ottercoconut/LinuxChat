#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC_BIN="${CC:-gcc}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/linuxchat-p0-tests.XXXXXX")"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

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

MYSQL_CONFIG_BIN="$(find_mysql_config)"

# Split compiler flags intentionally; mysql_config/pkg-config return shell-style flag lists.
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

"$CC_BIN" -Wall -Wextra -Wpedantic \
    "$ROOT_DIR/c-native/client/client.c" \
    -o "$TMP_DIR/client" \
    "${GTK_FLAGS[@]}" -lpthread

"$CC_BIN" -Wall -Wextra -Wpedantic \
    "$ROOT_DIR/c-native/server/server.c" \
    -o "$TMP_DIR/server" \
    "${MYSQL_CFLAGS[@]}" "${MYSQL_LIBS[@]}" "${MYSQL_RPATH_FLAGS[@]}" -lpthread

"$CC_BIN" -Wall -Wextra -Wpedantic \
    "$ROOT_DIR/tests/test_server_session.c" \
    -o "$TMP_DIR/test_server_session" \
    "${MYSQL_CFLAGS[@]}" "${MYSQL_LIBS[@]}" "${MYSQL_RPATH_FLAGS[@]}" -lpthread

RUNTIME_LIBRARY_PATH="$(IFS=:; printf '%s' "${RUNTIME_LIBRARY_DIRS[*]}")"
DYLD_LIBRARY_PATH="${RUNTIME_LIBRARY_PATH}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
LD_LIBRARY_PATH="${RUNTIME_LIBRARY_PATH}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$TMP_DIR/test_server_session"

CLIENT_SOURCE="$ROOT_DIR/c-native/client/client.c"
SERVER_SOURCE="$ROOT_DIR/c-native/server/server.c"

assert_contains "$CLIENT_SOURCE" "gtk_stack_add_named(GTK_STACK(main_stack), login_window, \"login\")"
assert_contains "$CLIENT_SOURCE" "gtk_stack_add_named(GTK_STACK(main_stack), chat_window, \"chat\")"
assert_contains "$CLIENT_SOURCE" "gtk_stack_set_visible_child(GTK_STACK(main_stack), chat_window)"
assert_contains "$CLIENT_SOURCE" "gdk_threads_add_idle(parse_friends_list, g_strdup(buffer))"
assert_contains "$CLIENT_SOURCE" "gdk_threads_add_idle(parse_messages_list, g_strdup(buffer))"
assert_contains "$CLIENT_SOURCE" "gdk_threads_add_idle(parse_new_message, g_strdup(buffer))"
assert_not_contains "$CLIENT_SOURCE" "(GSourceFunc)parse_"

assert_contains "$SERVER_SOURCE" "update_client_session(client->sockfd, user_id, username, nickname)"
assert_contains "$SERVER_SOURCE" "sender_id = client->user_id"
assert_not_contains "$SERVER_SOURCE" "login_user(client->username, \"\","

printf 'P0 tests passed.\n'
