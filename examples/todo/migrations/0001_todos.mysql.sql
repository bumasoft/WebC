-- The mysql/mariadb spelling of 0001_todos.sql (same migration, so the same row in
-- schema_migrations): mysql has no `integer primary key autoincrement`, and it wants a
-- length on an indexed varchar — `text` cannot be a key without a prefix.
create table if not exists todos (
  id bigint auto_increment primary key,
  title varchar(255) not null,
  done tinyint(1) not null default 0,
  created_at varchar(40) not null
);
