create table if not exists todos (
  id integer primary key autoincrement,
  title text not null,
  done integer not null default 0,
  created_at text not null default current_timestamp
);
