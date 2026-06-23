#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC_BIN="${CC:-gcc}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/linuxchat-p1-tests.XXXXXX")"

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

"$ROOT_DIR/tests/run_p0_tests.sh"

MYSQL_CONFIG_BIN="$(find_mysql_config)"

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
    "$ROOT_DIR/tests/test_p1_stability.c" \
    -o "$TMP_DIR/test_p1_stability" \
    "${MYSQL_CFLAGS[@]}" "${MYSQL_LIBS[@]}" "${MYSQL_RPATH_FLAGS[@]}" -lpthread

RUNTIME_LIBRARY_PATH="$(IFS=:; printf '%s' "${RUNTIME_LIBRARY_DIRS[*]}")"
DYLD_LIBRARY_PATH="${RUNTIME_LIBRARY_PATH}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
LD_LIBRARY_PATH="${RUNTIME_LIBRARY_PATH}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$TMP_DIR/test_p1_stability"

CLIENT_SOURCE="$ROOT_DIR/c-native/client/client.c"
SERVER_SOURCE="$ROOT_DIR/c-native/server/server.c"

assert_contains "$SERVER_SOURCE" "pthread_mutex_t db_mutex"
assert_contains "$SERVER_SOURCE" "pthread_mutex_lock(&db_mutex)"
assert_contains "$SERVER_SOURCE" "DATE_FORMAT(m.timestamp, '%%Y-%%m-%%d %%H-%%i-%%s')"
assert_contains "$SERVER_SOURCE" "if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)"
assert_contains "$SERVER_SOURCE" "get_messages(user_id, friend_id, messages, sizeof(messages) - strlen(\"MESSAGES_LIST:\"))"
assert_not_contains "$SERVER_SOURCE" "char messages[BUFFER_SIZE * 10]"
assert_not_contains "$SERVER_SOURCE" "sprintf(response, \"MESSAGES_LIST:%s\", messages)"
assert_not_contains "$SERVER_SOURCE" "sprintf("
assert_not_contains "$SERVER_SOURCE" "strcat("
assert_not_contains "$SERVER_SOURCE" "strcpy("

assert_contains "$CLIENT_SOURCE" "%1023[^:]:%49[^:]:%49[^;];"
assert_contains "$CLIENT_SOURCE" "snprintf(msg, sizeof(msg), \"%s [%s]: %s\\n\", nickname, timestamp, content)"

printf 'P1 tests passed.\n'
