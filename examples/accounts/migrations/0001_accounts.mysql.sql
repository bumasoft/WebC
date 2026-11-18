-- The mysql/mariadb spelling of 0001_accounts.sql: a bigint identity, and a varchar for the
-- unique email (mysql cannot index a `text` column without a prefix length).
create table if not exists accounts (
  id bigint auto_increment primary key,
  email varchar(255) not null unique,
  password_hash varchar(255) not null,
  created_at varchar(40) not null
);
