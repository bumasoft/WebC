-- The postgres spelling of 0001_accounts.sql.
create table if not exists accounts (
  id bigserial primary key,
  email text not null unique,
  password_hash text not null,
  created_at text not null
);
