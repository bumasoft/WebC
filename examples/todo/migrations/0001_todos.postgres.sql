-- The postgres spelling of 0001_todos.sql (same migration, same name in
-- schema_migrations): `integer primary key autoincrement` is sqlite syntax, and
-- postgres needs a sequence, and a boolean where sqlite would use 0/1.
create table if not exists todos (
  id bigserial primary key,
  title text not null,
  done boolean not null default false,
  created_at text not null default (to_char(now(), 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'))
);
