MySQL and MariaDB are the third database WebC speaks, and the reason the SQL above the driver never
branches on dialect: everything that actually differs between servers is **declared** by the driver
and read by the framework — a third driver is a capability table, not an abstraction over SQL.

The driver is `src/db_mysql.c`: the wire protocol, by hand. No libmysqlclient, no `.so` to install, no
version to match — the same reason there is no libpq and no hiredis.

## One URL, three drivers

```sh
DATABASE_URL=mysql://app:secret@127.0.0.1:3306/app
DATABASE_URL=mariadb://app:secret@127.0.0.1:3306/app?ssl-mode=require
DATABASE_URL=mysql://app:secret@localhost/app?socket=/run/mysqld/mysqld.sock
```

`mysql://` and `mariadb://` are one driver (the protocol is the same; the two servers differ in a few
error codes and in the name of one timeout variable, and the driver asks which server answered at
handshake time). Anything without `://` is still a sqlite path, and a scheme nobody implements is
**refused at open**, naming the URLs this build speaks. Opened as a sqlite file named after the URL,
`postgre://x` shows its first sign as "my tables are empty" three screens later.

```c
// src/tickets.c — the server half: the same filter as any other database
#include <webc/server.h>

WEBC_MODEL(Ticket, (id, i64, .pk = true), (sku, str, .max = 190), (qty, i64))
Vec(Ticket)

static VecTicket *for_sku(str sku) {
  WDB *db = db_open("mysql://app:secret@127.0.0.1:3306/app?ssl-mode=require");
  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Ticket, sku, sku);
  VecTicket *rows = db_all_w(db, Ticket, &w);
  ww_free(&w);
  return rows;
}
```

`db_driver_name(db)` answers `"mysql"` (or `"mariadb"`, `"postgres"`, `"sqlite"`). The framework
keeps those names as **data**, not as two string comparisons, and that list is what the migration
runner reads when it looks for a `<name>.mysql.sql` twin — so a fourth dialect would be a line there
and a file next to the other two.

## Users and auth plugins

Which plugin your user has decides what the connection can do, and this is the part that surprises
people:

| plugin | this driver | needs |
| --- | --- | --- |
| `mysql_native_password` | SHA-1 scramble, always available | nothing |
| `caching_sha2_password` | the SHA-256 fast path after the first login | the *full* path needs TLS |
| `sha256_password` | supported, TLS only | TLS |
| `ed25519`, GSSAPI | refused **by name**, with the two alternatives in the message | — |

`caching_sha2_password` is the default for a fresh MySQL 8 user, and its fast path only works once the
server has cached the login — on a cold cache it asks for the password over a secure channel. With
TLS, the driver answers; without TLS it refuses and says so, rather than sending a password in the
clear or failing as "access denied" when the password was right.

**There is no RSA public-key exchange, on purpose.** A client that will hand its password encrypted
with whatever key the server offers is a client that gives the password to whoever is in the middle;
`?ssl-mode=require` is the answer, and it is one word in the URL. The cost is honest: `sha256_password`
and a cold `caching_sha2_password` need TLS, and the error says exactly that.

That is why `make mysql-test` starts its container with `--mysql-native-password=ON` and a non-root
`webc` user: a build without TLS can only speak the first row of that table, while MySQL 8.4 loads
that plugin only when asked. With TLS (`make WEBC_TLS=openssl build/webc`) the stock root account
works as it is.

## TLS

```sh
?ssl-mode=disable     # plaintext, and say so
?ssl-mode=prefer      # default: TLS if the server offers it, plaintext if not
?ssl-mode=require     # TLS or refuse to connect
?ssl-mode=verify-ca   # …and the certificate must chain to ?ssl-ca=/etc/ca.pem
?ssl-mode=verify-full # …and the host name must match, too
```

A build without TLS refuses `require`, `verify-ca` and `verify-full` **at open**, with a message that
names the build flag — an app that has quietly been running in plaintext is worse than one that failed
to start. `prefer` is the one that bends: a server that offers no TLS logs one warning and continues in
plaintext, and a *failed* handshake reconnects in plaintext with the reason in the log, because a
half-written handshake cannot be continued in the clear and the alternative is a confusing
"malformed packet" three statements later.

## What the connection sets up

Every connection, before your first statement:

```sql
SET NAMES utf8mb4;
SET SESSION sql_mode='STRICT_TRANS_TABLES,NO_ENGINE_SUBSTITUTION,ERROR_FOR_DIVISION_BY_ZERO';
SET SESSION time_zone='+00:00';
SET SESSION autocommit=1;
```

Strict mode is not decoration. Without it MySQL silently *truncates* an over-long string, which would
make the `WDBE_TOO_LONG` class (a 422) unreachable and let a 250-character title into a
`varchar(200)` column. With it, the server refuses and the request says which field, which is the
behaviour sqlite and postgres already had. `time_zone='+00:00'` is what makes a `datetime` mean the
same instant everywhere: one server in the app's local zone would otherwise shift every row it wrote.

## How a statement travels

Two paths, chosen by whether the statement has parameters:

- **No parameters → `COM_QUERY`.** This is what lets a whole `migrations/0001_init.mysql.sql` run in
  one call, and if it carries several result sets the **last one** is what you get back (the rule all
  three drivers follow).
- **Parameters → `COM_STMT_PREPARE` / `COM_STMT_EXECUTE`**, binary rows, cached per connection: 64
  statements, LRU, keyed on the SQL text (statements longer than a struct-sized key are prepared each
  time rather than kept). A prepared statement is a small win, but it is also the reason a
  `:name` placeholder can never be spliced into SQL — the value goes on a different wire than the
  text.

Rows are decoded from the column definition in both paths: `tinyint(1)` is a `bool` (that length is
how MySQL spells one, and the model already knows the field is a bool), `DECIMAL` is read as an `i64`
or an `f64` by its digits, and a `FLOAT`/`DOUBLE` column is always a real even when its value looks
integral — `sum(1.5 + 2.5 + 4.0)` is `8.0`, not `8`.

`FOUND_ROWS` is deliberately **not** set: `affected` means "changed", which is what a job queue and a
`db_increment` want to know. A count is a `db_count`, not an affected-rows reading of a `select`.

## What a dialect declares

Four flags, three drivers. This table is the whole of the dialect branching in the framework:

| | sqlite | postgres | mysql |
| --- | --- | --- | --- |
| `returning` — a new key comes back with the insert | no (`last_insert_rowid()`) | yes (`insert … returning`) | no (the OK packet's `last_insert_id`) |
| `offset_needs_limit` — `offset 5` alone is legal | no | yes | no |
| `ident_quote` — what quotes a column | `"` | `"` | `` ` `` |
| `multi_statement` — one call may carry several | yes | yes | yes |

Read as behaviour: `db_insert` reports the new key on all three without the app looking at which one
it is; a `ww_limit(&w, 0, 40)` (offset, no limit) becomes
`limit 9223372036854775807 offset 40`, which is the largest *signed* 64-bit value because MySQL
documents the unsigned one and sqlite answers *datatype mismatch* to it; and a model field called
`rank`, `groups` or `order` is quoted on the way out, so a reserved word is a column name rather than a
syntax error. A name that is not a plain identifier (`public.users`, a `.sql()` override) is passed
through untouched, because that is the caller's own SQL.

Two more shapes are worth knowing because they are the *same value* arriving differently:

- **An aggregate is `DECIMAL` here.** `sum(int)` arrives as `DECIMAL(32,0)` and `avg(int)` as
  `DECIMAL(x,4)`. The driver's text decoder reads both, so `db_sum` returns an `i64` and `db_avg` an
  `f64`, exactly as on the other two servers.
- **A `like` pattern is escaped with `!`.** A backslash is an escape character *inside a string
  literal* on MySQL, so `escape '\'` is a syntax error there — every search filter in every app would
  have been a 500 on the third driver. `ww_contains` writes `escape '!'`, and no dialect gives `!` a
  second meaning.

## Errors are classified, not searched for

Each driver maps its **own** codes into one class, and the class picks the status. Nothing looks for
the word "unique" in an error message — that holds on sqlite and postgres and fails on MySQL.

| class | MySQL errno | what the request answers |
| --- | --- | --- |
| `WDBE_CONFLICT` | 1062, 1586, 1451 | 409 |
| `WDBE_BAD_REFERENCE` | 1452, 1216, 1217 | 400 |
| `WDBE_NOT_NULL` | 1048, 1364 | 422 |
| `WDBE_TOO_LONG` | 1406, 1264, 1292 | 422 |
| `WDBE_CHECK` | 3819, 4025 | 422 |
| `WDBE_DEADLOCK` | 1213 | 503 |
| `WDBE_TIMEOUT` | 1205, 3024 | 503 |
| `WDBE_AUTH` | 1045, 1698 | 500 |
| `WDBE_LOST` | 2006, 2013, 1053 | 500, and the connection is not reused |

An error the driver cannot classify stays `WDBE_NONE` and answers 500 with the server's own sentence —
deliberately, because a guess at 409 is worse than a 500 that says what happened.

## Migrations

The migration runner puts the driver's name into the file name: `<name>.mysql.sql` is used instead of
`<name>.sql` when it exists, and the same logical name is recorded in `schema_migrations`, so one
sequence of migrations serves the three servers.

```sh
webc migrate new add_tickets      # writes 0003_add_tickets.sql (+ .postgres.sql, .mysql.sql)
```

**Type the columns the way the field is stored**, which is the mistake that costs an afternoon:

| field | column on MySQL | why |
| --- | --- | --- |
| `(id, i64, .pk = true)` | `bigint auto_increment primary key` | the identity column is spelled by the server |
| an enum (`RKind`, `Lane`) | `varchar(40)` — never `int` | the framework stores an enum by **name** |
| `bool` | `tinyint(1)` | which is also how the driver reads it back |
| `str` indexed or unique | `varchar(190)` | `text` cannot be indexed without a prefix length |
| `str`, unindexed | `text` | |
| money | `integer` (cents) | never a float |

SQLite accepts a name in an `integer` column and will not tell you, so a migration can be wrong for
months and only fail the day someone runs it against postgres or MySQL (`invalid input syntax for type
integer: "balanced"`). `webc check` compares the models against the migrated schema and names a column
that has drifted.

## Running the tests

An app's own suite runs on MySQL by pointing it at the server:

```sh
WEBC_TEST_DB="mysql://webc:webc@127.0.0.1:33406/parity" webc test
```

`make mysql-test` is the framework's version of that, for this repository: it starts MySQL 8 in
docker (port 33406, `--mysql-native-password=ON`), creates a non-root `webc` user, runs the
cross-driver parity test, the driver's own suite, the ORM suites and both example apps, and removes
the container. One target, one line at the end: `mysql: all green`, or the first suite that was not.

- Use `WEBC_MYSQL_URL` to point it at a server you already have.
- Without docker and without `WEBC_MYSQL_URL`, every MySQL test **skips with the reason in its name**,
  so a laptop with no database still gets a green run.
- `tests/test_db.h` is what keeps one suite honest across three servers: `wc_test_url()` (the
  environment or `sqlite::memory:`), `wc_test_pk()`/`wc_test_text()`/`wc_test_flag()` for the three
  DDL spellings that really differ, and `wc_test_q()` for the quoting.

## Timeouts, and a query that has already lost

A request with a deadline (`mw_timeout_ms()`) gets its cap pushed down to the server as well: the
driver sets `max_execution_time` (MySQL) or `max_statement_time` (MariaDB) from the request's
remaining time, so a client that has gone away stops costing the server a query. If the statement is
still running when the handler gives up, the driver sends `KILL QUERY <id>` on a second connection —
the server-side cap and the kill are two halves of one promise, because the first one is per
statement and the second one is now.

A lost connection (`2006`, `2013`, `1053`) marks the session broken: the pool drops it instead of
handing it to the next request, and the statement reports `WDBE_LOST` rather than a mysterious
"server has gone away" in a log nobody reads.

## What is not covered

- **Row sets larger than one 16 MB packet** and `LOAD DATA`-sized blobs: the reassembly path is
  written and read, but nothing has pushed it.
- **`ed25519` and GSSAPI** are refused by name, so there is nothing to verify.
- **No `FOUND_ROWS`**, as above: `affected` means changed.
- **Replication, `LOAD DATA`, multi-host URLs, and the `mysql` client's own table of options** are not
  a driver's job. One URL, one server.

## See also

- Reference [§9a5](../reference/database.html) — the same material in reference voice, with the
  session setup and the TLS build procedure.
- [The ORM](../guides/orm.html) — the filters and relations that run unchanged on all three servers.
- [Redis and the query cache](../guides/data.html) — the other wire protocol in the box.
- Reference [§9a5](../reference/database.html) — the driver table, and how a driver classifies its
  own errors.