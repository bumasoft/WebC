// `webc openapi` writes the API boundary from what the sources say: bodies from
// the body family, responses from json/json_list with their status literals,
// parameter types from path_i64, constraints from the model qualifiers. A
// document that asserts a shape the handler does not build is worse than none,
// so the assertions run against a real scaffold, not a fixture.
const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const CLI = path.resolve(__dirname, '..', 'build', 'webc');

function fail(msg) {
  console.log('  FAIL ' + msg);
  process.exitCode = 1;
}

function run(args, cwd) {
  try {
    return { ok: true, out: execFileSync(CLI, args, { cwd, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }) };
  } catch (e) {
    return { ok: false, out: (e.stdout || '') + (e.stderr || '') };
  }
}

(function main() {
  console.log('openapi: the document says what the handlers build');
  if (!fs.existsSync(CLI)) { console.log('  SKIP: no build/webc (run make cli)'); return; }

  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'webc-openapi-'));
  try {
    const dir = path.join(root, 'full');
    const made = run(['new', dir, '--template=fullstack'], root);
    if (!made.ok) { fail('webc new failed\n' + made.out.slice(0, 400)); return; }

    const gen = run(['openapi'], dir);
    if (!gen.ok) { fail('webc openapi failed\n' + gen.out.slice(0, 600)); return; }
    const file = path.join(dir, 'openapi.json');
    if (!fs.existsSync(file)) { fail('no openapi.json was written'); return; }

    let d = null;
    try { d = JSON.parse(fs.readFileSync(file, 'utf8')); }
    catch (e) { fail('openapi.json is not valid JSON: ' + e.message); return; }

    const p = d.paths;
    // the fullstack shape: GET /api/notes (json over db_all), POST /api/notes
    // (body_try + json 201), POST /api/notes/:id/delete (path_i64)
    const post = p['/api/notes'] && p['/api/notes'].post;
    if (!post) { fail('POST /api/notes is missing'); return; }
    const bodyJson = post.requestBody && post.requestBody.content['application/json'];
    if (!bodyJson || !bodyJson.schema.$ref || !bodyJson.schema.$ref.endsWith('/Note'))
      fail('POST /api/notes: requestBody should reference Note (body_try counts)');
    if (!post.requestBody.content['application/x-www-form-urlencoded'])
      fail('POST /api/notes: body_try parses a form too — the content type belongs here');
    const r201 = post.responses['201'];
    if (!r201 || !r201.content['application/json'].schema.$ref ||
        !r201.content['application/json'].schema.$ref.endsWith('/Note'))
      fail('POST /api/notes: the json(201, Note, &n) response should reference Note');
    if (!post.responses['422']) fail('POST /api/notes: a body route answers 422');

    const get = p['/api/notes'].get;
    const arr = get && get.responses['200'].content['application/json'].schema;
    if (!arr || arr.type !== 'array' || !arr.items.$ref || !arr.items.$ref.endsWith('/Note'))
      fail('GET /api/notes: json(200, db_all(Note)) is a list of Note');

    const del = p['/api/notes/{id}/delete'] && p['/api/notes/{id}/delete'].post;
    if (!del) { fail('the :id route did not become {id}'); return; }
    const prm = del.parameters && del.parameters[0];
    if (!prm || prm.name !== 'id' || prm.in !== 'path' || !prm.required)
      fail('the {id} parameter is wrong: ' + JSON.stringify(prm));
    if (prm.schema.type !== 'integer')
      fail("path_i64('id') should type the parameter integer, got " + prm.schema.type);

    const note = d.components.schemas.Note;
    if (!note.properties.body || note.properties.body.maxLength !== 500)
      fail('Note.body: .max = 500 should become maxLength (the quals span, not the first)');
    if (!note.required || !note.required.includes('body'))
      fail('Note.body: .required should become a required entry');
    if (!note.properties.id || note.properties.id.readOnly !== true)
      fail('Note.id: .pk should become readOnly');
    if (!d.components.schemas.WebcError) fail('the flat error shape is missing');

    // a shape the index cannot read (res_text) still produces a valid document
    const h = p['/health'].get;
    if (!h || !h.responses || Object.keys(h.responses).length === 0)
      fail('/health: an empty responses object is not a document a generator accepts');

    // --check: green when current, red when the sources moved under the file
    const ok = run(['openapi', '--check'], dir);
    if (!ok.ok) fail('--check should pass right after generation:\n' + ok.out.slice(0, 300));
    fs.appendFileSync(file, '\n');
    const stale = run(['openapi', '--check'], dir);
    if (stale.ok) fail('--check should fail when the file drifted');

    const outside = run(['openapi'], root);
    if (outside.ok) fail('webc openapi ran outside an app');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
})();
