# LinuxChat Test Suite

`tests/run_all_tests.sh` is the primary test entrypoint.

Default run:

```bash
./tests/run_all_tests.sh
```

The default suite does not require a running MySQL server. It covers:

- client and server compile checks
- server session state updates
- server message routing with socket pairs
- protocol record bounds and truncation behavior
- P2 security contracts for protocol delimiter rejection and pre-database validation
- source-level contracts for GTK idle callbacks, bounded parsing, prepared statements, password hashing, session authorization, database locking, schema constraints, and response construction

Database integration run:

```bash
LINUXCHAT_RUN_DB_TESTS=1 \
LINUXCHAT_TEST_DB_HOST=localhost \
LINUXCHAT_TEST_DB_USER=chat_test_user \
LINUXCHAT_TEST_DB_PASSWORD=chat_test_password \
LINUXCHAT_TEST_DB_NAME=linuxchat_test \
./tests/run_all_tests.sh --with-db
```

The database name must contain `test`. The integration test drops and recreates the `users`, `messages`, and `friends` tables inside that database.

The database suite checks:

- duplicate usernames are rejected
- stored passwords are SHA-256 hashes instead of plaintext
- SQL-injection-shaped login input does not bypass authentication
- invalid friend foreign keys are rejected
- reciprocal friend rows are created together
- duplicate friendships do not create extra rows
- half-friendship rollback does not create a second inconsistent row
- historical messages are ordered and use delimiter-safe timestamps
- delimiter-unsafe message content is rejected before persistence
- invalid message foreign keys do not persist
- deleting a user cascades related messages and friendships
