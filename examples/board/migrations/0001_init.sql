-- The board's one table. Written by hand once, because a project's first migration is the
-- one thing `webc migrate` cannot generate: it is the app's own history.
create table if not exists cards (
  id integer primary key autoincrement,
  title text not null,
  lane integer not null default 0,
  position integer not null default 0
);
create index if not exists cards_lane on cards (lane, position);

-- A board that opens empty shows nothing about what a board is: three columns and a promise.
-- These are the cards a first visit finds, so the first thing you can do here is drag one.
-- (An example is a demo as much as it is source; the tests wipe this table per case.)
insert into cards (id, title, lane, position) values
  (1, 'Audit the empty states',      0, 0),
  (2, 'Write the release notes',     0, 1),
  (3, 'Spike: an offline queue',     0, 2),
  (4, 'Keyboard moves on every card', 0, 3),
  (5, 'Board drag and drop',         1, 0),
  (6, 'Live rooms over SSE',         1, 1),
  (7, 'Presence pill and toasts',    1, 2),
  (8, 'Skeleton and tokens',         2, 0),
  (9, 'Tokens into one file',        2, 1),
  (10, 'Dark mode pass',             2, 2);