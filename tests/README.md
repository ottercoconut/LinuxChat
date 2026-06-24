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
- security checks for protocol delimiter rejection and pre-database validation
- source-level checks for GTK idle callbacks, bounded parsing, username-based user operations, prepared statements, password hashing, session authorization, database locking, schema constraints, group protocol handling, block handling, offline-message handling, and response construction

Database integration run:

```bash
LINUXCHAT_RUN_DB_TESTS=1 \
LINUXCHAT_TEST_DB_HOST=localhost \
LINUXCHAT_TEST_DB_USER=chat_test_user \
LINUXCHAT_TEST_DB_PASSWORD=chat_test_password \
LINUXCHAT_TEST_DB_NAME=linuxchat_test \
./tests/run_all_tests.sh --with-db
```

The database name must contain `test`. The integration test drops and recreates the `users`, `messages`, `friends`, `friend_blocks`, `chat_groups`, `group_members`, `group_messages`, and `group_message_deliveries` tables inside that database.

The database suite checks:

- duplicate usernames are rejected
- stored passwords are SHA-256 hashes instead of plaintext
- SQL-injection-shaped login input does not bypass authentication
- username lookup resolves `users.username`, rejects missing or delimiter-unsafe usernames, and does not use nicknames as identifiers
- invalid friend foreign keys are rejected
- reciprocal friend rows are created together
- adding a friend by username rejects self-adds
- duplicate friendships do not create extra rows
- half-friendship rollback does not create a second inconsistent row
- user blocks prevent private-message send eligibility and can be removed
- group creation stores the owner and initial members
- duplicate group members do not create extra rows
- non-members cannot read group members/history or send group messages
- adding a group member by username still authorizes with the requester session user ID
- group messages persist and create per-member delivery rows
- private and group offline-message summaries clear after history is opened
- historical messages are ordered and use delimiter-safe timestamps
- delimiter-unsafe message content is rejected before persistence
- invalid message foreign keys do not persist
- deleting a user cascades related messages, friendships, groups, group messages, and delivery rows
