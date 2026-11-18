create table if not exists accounts (
  id integer primary key autoincrement,
  email text not null unique,
  password_hash text not null,
  created_at text not null
);
