#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC_BIN="${CC:-gcc}"

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

    printf '未找到 mysql_config。请安装 MySQL，或设置 MYSQL_CONFIG=/path/to/mysql_config。\n' >&2
    exit 1
}

find_openssl_libdir() {
    if [[ -n "${OPENSSL_LIBDIR:-}" && -d "$OPENSSL_LIBDIR" ]]; then
        printf '%s\n' "$OPENSSL_LIBDIR"
        return
    fi

    if [[ -n "${OPENSSL_PREFIX:-}" && -d "$OPENSSL_PREFIX/lib" ]]; then
        printf '%s\n' "$OPENSSL_PREFIX/lib"
        return
    fi

    if command -v brew >/dev/null 2>&1; then
        local prefix
        prefix="$(brew --prefix openssl@3 2>/dev/null || true)"
        if [[ -n "$prefix" && -d "$prefix/lib" ]]; then
            printf '%s\n' "$prefix/lib"
            return
        fi
    fi

    if [[ -d /opt/homebrew/opt/openssl@3/lib ]]; then
        printf '%s\n' /opt/homebrew/opt/openssl@3/lib
        return
    fi

    if [[ -d /usr/local/opt/openssl@3/lib ]]; then
        printf '%s\n' /usr/local/opt/openssl@3/lib
        return
    fi

    printf '未找到 openssl@3。请运行 brew install openssl@3，或设置 OPENSSL_LIBDIR=/path/to/openssl/lib。\n' >&2
    exit 1
}

require_command() {
    local command_name="$1"
    local install_hint="$2"

    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf '未找到 %s。%s\n' "$command_name" "$install_hint" >&2
        exit 1
    fi
}

require_command pkg-config '请安装 pkg-config 和 gtk+3，例如：brew install pkg-config gtk+3。'
require_command install_name_tool '请安装 Xcode Command Line Tools，例如：xcode-select --install。'
require_command otool '请安装 Xcode Command Line Tools，例如：xcode-select --install。'

MYSQL_CONFIG_BIN="$(find_mysql_config)"
MYSQL_LIBDIR="$("$MYSQL_CONFIG_BIN" --variable=pkglibdir)"
OPENSSL_LIBDIR="$(find_openssl_libdir)"

if [[ ! -d "$MYSQL_LIBDIR" ]]; then
    printf 'MySQL 库目录不存在：%s\n' "$MYSQL_LIBDIR" >&2
    exit 1
fi

printf '使用编译器：%s\n' "$CC_BIN"
printf '使用 mysql_config：%s\n' "$MYSQL_CONFIG_BIN"
printf 'MySQL 库目录：%s\n' "$MYSQL_LIBDIR"
printf 'OpenSSL 库目录：%s\n' "$OPENSSL_LIBDIR"

# pkg-config/mysql_config 输出的是 shell 风格参数列表，这里保留有意的拆词。
# shellcheck disable=SC2207
GTK_FLAGS=($(pkg-config --cflags --libs gtk+-3.0))
# shellcheck disable=SC2207
MYSQL_CFLAGS=($("$MYSQL_CONFIG_BIN" --cflags))

printf '\n== 编译客户端 ==\n'
"$CC_BIN" "$ROOT_DIR/client/client.c" \
    -o "$ROOT_DIR/client/client" \
    "${GTK_FLAGS[@]}" -lpthread

printf '\n== 编译服务端 ==\n'
"$CC_BIN" "$ROOT_DIR/server/server.c" \
    -o "$ROOT_DIR/server/server" \
    "${MYSQL_CFLAGS[@]}" \
    -L"$MYSQL_LIBDIR" -L"$OPENSSL_LIBDIR" \
    -lmysqlclient -lssl -lcrypto -lresolv \
    -Wl,-rpath,"$MYSQL_LIBDIR" \
    -Wl,-rpath,"$OPENSSL_LIBDIR" \
    -Wl,-headerpad_max_install_names \
    -lpthread

install_name_tool -change libssl.3.dylib @rpath/libssl.3.dylib "$ROOT_DIR/server/server"
install_name_tool -change libcrypto.3.dylib @rpath/libcrypto.3.dylib "$ROOT_DIR/server/server"

printf '\n== 服务端动态库链接 ==\n'
otool -L "$ROOT_DIR/server/server"

printf '\n== 服务端 rpath ==\n'
otool -l "$ROOT_DIR/server/server" | awk '
    /LC_RPATH/ { in_rpath = 1; next }
    in_rpath && /path / { print $2; in_rpath = 0 }
'

printf '\n编译完成：\n'
printf '  %s\n' "$ROOT_DIR/client/client"
printf '  %s\n' "$ROOT_DIR/server/server"
