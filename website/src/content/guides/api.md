A WebC app's routes and models are typed end to end, and the moment an outside consumer asks what
the API looks like, that typing has an answer: the boundary itself, generated. This page is the
beachhead — the story of pointing a React app (or a mobile client, or a partner) at a WebC backend
without anyone reading C.

# The API boundary: `webc openapi` and `webc client --ts`

## What the sources already say

Every route is a macro with a typed path, every body comes from the `body` family, every JSON
response is a `json`/`json_list` call with its status literal and the shape it names, and every
model carries its own constraints. `webc openapi` writes that down as **OpenAPI 3.1**:

```sh
webc openapi                 # writes openapi.json
webc openapi --check         # exits non-zero when the file drifted from the sources
```

- **paths** from the route table — `:id` becomes `{id}`, and the parameter is an integer where
  the handler's own `path_i64` says so;
- **request bodies** from `body`/`body_try` (JSON *and* form content — both parse), from
  `body_json`/`body_form` (one each);
- **responses** from the `json` calls — `json_list(200, db_all(Note))` is an array of `Note`,
  and so is `json(200, db_all(Note))`;
- **schemas** from the model descriptors: `.max` is `maxLength`, `.gte`/`.lte` are
  `minimum`/`maximum`, `.format` is `format`, `.pk` is `readOnly`, `.no_json` is absent, and the
  flat `{error, message, status}` every failure answers with is the `WebcError` schema.

What it refuses to guess matters as much: a response that is not a model is an `{}` with a
description, no query parameter is invented, and pages are not the API.

## The typed client

A document is for tools; a developer wants functions. `webc client --ts` writes the typed fetch
client from the same facts:

```sh
webc client --ts            # writes webc.client.ts
```

```ts
export interface Note {
  id: number;
  body: string;
}

export interface NoteInput {
  body: string;
}

export const api = {
  // GET /api/notes
  listNote: () => req<Note[]>(`GET`, `/api/notes`),
  // POST /api/notes
  createNote: (body: NoteInput) => req<Note>(`POST`, `/api/notes`, body),
  // POST /api/notes/:id/delete
  deleteNote: (id: number) => req<void>(`POST`, `/api/notes/${id}/delete`),
};
```

The `Input` twin is what a create *sends* — no pk (the database assigns it), no `.no_json` fields.
Names are mechanical on purpose: `todos` becomes `Todo` only because a model backs the rewrite,
`addresses` stays `addresses`, a GET that answers a list reads as `list…`, a POST to a path that
names its own act (`/delete`) reads as that act. Field names keep their wire spelling — the client
describes what the server sends, not what a style guide wishes it sent. The file is the app's:
edit the names, commit it, and `--check` fails when the sources drifted underneath it.

## The ratchet

Both files are generated **once** and owned from then on — commit them, diff them in review. CI is
one line, and `webc ci` writes it for you: the workflow runs the `--check` of each file the app
ships.

```yaml
- name: the API boundary, if the app ships one
  run: |
    test ! -f openapi.json || webc openapi --check
    test ! -f webc.client.ts || webc client --ts --check
```

## Why this is the beachhead

"Ditch React" is a hard sell; "ditch your Node BFF first" is an easy one. A React team can point
the generated client at a WebC backend tomorrow — typed, no C to read — and once the backend is
WebC, moving the view half is the natural next step: it is the same models. If React is the
vocabulary you already speak, the [Rosetta guide](rosetta.html) is the dictionary for that
conversation.

## See also

- Reference §14 for every flag (`--out`, `--check`).
- [Deploying](deploy.html) — `webc ci` and the dist artifact on a tag.
- [The ORM](orm.html) — where the constraints being exported come from.
