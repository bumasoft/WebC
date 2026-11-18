// The resolver's gate, run by `make lsp`. No editor, no spawning, no network: a fake
// filesystem is enough, and every case here is one a real user has hit or would.
'use strict';

const assert = require('assert');
const path = require('path');
const { findBinary, expand, explain } = require('./resolve');

const HOME = '/Users/ada';
const ROOT = '/Users/ada/Projects/house-edge';

// files: absolute paths that exist. exec: the subset that is executable. dirs: paths
// that exist but are not files.
function fsOf(files, exec, dirs) {
  const dirSet = new Set(dirs || []);
  const set = new Set(files.concat(dirs || []));
  const run = new Set(exec === undefined ? files : exec);
  return {
    exists: (p) => set.has(p),
    isFile: (p) => set.has(p) && !dirSet.has(p),
    isExecutable: (p) => run.has(p),
  };
}

const ENV = { HOME: HOME, PATH: '/usr/bin:/bin' };
let n = 0;

function caseNamed(name, fn) {
  n++;
  try {
    fn();
    console.log(`  ok   resolve: ${name}`);
  } catch (e) {
    console.error(`  FAIL resolve: ${name}\n       ${e.message}`);
    process.exitCode = 1;
  }
}

// ---- the reported bug: a tilde in the setting
caseNamed('expands ~ in the setting (the bug that said "set webc.lsp.path")', () => {
  const bin = HOME + '/Documents/Projects/WebC/build/webc';
  const r = findBinary({
    configured: '~/Documents/Projects/WebC/build/webc',
    root: ROOT,
    env: ENV,
    home: HOME,
    ...fsOf([bin]),
  });
  assert.strictEqual(r.ok, true, 'a plain ~/… path must resolve, not be handed to spawn');
  assert.strictEqual(r.bin, bin);
  assert.strictEqual(r.source, 'setting');
});

caseNamed('expands ~ on its own', () => {
  assert.strictEqual(expand('~', { home: HOME, root: ROOT, env: ENV }), HOME);
  assert.strictEqual(expand('  ~/x  ', { home: HOME, root: ROOT, env: ENV }), path.join(HOME, 'x'));
});

caseNamed('expands ${workspaceFolder} and ${env:NAME}', () => {
  assert.strictEqual(expand('${workspaceFolder}/build/webc', { root: ROOT, home: HOME, env: ENV }), ROOT + '/build/webc');
  assert.strictEqual(expand('${env:WEBC_BIN}', { root: ROOT, home: HOME, env: { WEBC_BIN: '/opt/webc' } }), '/opt/webc');
  assert.strictEqual(expand('${env:NOPE}', { root: ROOT, home: HOME, env: {} }), '${env:NOPE}', 'an unset variable is left visible, not blanked');
});

caseNamed('resolves a relative setting against the workspace, not the editor', () => {
  const bin = ROOT + '/build/webc';
  const r = findBinary({ configured: 'build/webc', root: ROOT, env: ENV, home: HOME, ...fsOf([bin]) });
  assert.strictEqual(r.ok, true);
  assert.strictEqual(r.bin, bin);
});

caseNamed('a wrong setting is reported, never silently replaced by another binary', () => {
  const other = ROOT + '/build/webc';
  const r = findBinary({ configured: '/nope/webc', root: ROOT, env: ENV, home: HOME, ...fsOf([other]) });
  assert.strictEqual(r.ok, false);
  assert.strictEqual(r.problem, 'missing');
  assert.strictEqual(r.bin, '/nope/webc');
  const msg = explain(r, '/nope/webc');
  assert.match(msg, /\/nope\/webc/);
  assert.match(msg, /does not exist/);
  assert.match(msg, /make/, 'the message should say how to get one');
});

caseNamed('a directory and a non-executable file are different failures with different advice', () => {
  const dir = findBinary({ configured: '~/WebC', root: ROOT, env: ENV, home: HOME, ...fsOf([], undefined, [HOME + '/WebC']) });
  assert.strictEqual(dir.problem, 'directory', 'a path that exists but is not a file is a directory');

  const noexec = HOME + '/WebC/build/webc';
  const r = findBinary({ configured: noexec, root: ROOT, env: ENV, home: HOME, ...fsOf([noexec], []) });
  assert.strictEqual(r.problem, 'not-executable');
  assert.match(explain(r, noexec), /chmod \+x/);
});

// ---- no setting at all
caseNamed('empty setting: build/webc in the workspace', () => {
  const bin = ROOT + '/build/webc';
  const r = findBinary({ root: ROOT, env: ENV, home: HOME, ...fsOf([bin]) });
  assert.strictEqual(r.ok, true);
  assert.strictEqual(r.source, 'workspace');
  assert.strictEqual(r.bin, bin);
});

caseNamed('empty setting: build/webc in a parent folder', () => {
  const bin = '/Users/ada/Projects/WebC/build/webc';
  const r = findBinary({ root: '/Users/ada/Projects/WebC/examples/todo', env: ENV, home: HOME, ...fsOf([bin]) });
  assert.strictEqual(r.ok, true);
  assert.strictEqual(r.source, 'parent');
  assert.strictEqual(r.bin, bin);
});

caseNamed('empty setting: webc on the PATH', () => {
  const r = findBinary({ root: ROOT, env: { PATH: '/opt/homebrew/bin:/usr/bin' }, home: HOME, ...fsOf(['/opt/homebrew/bin/webc']) });
  assert.strictEqual(r.ok, true);
  assert.strictEqual(r.source, 'PATH');
  assert.strictEqual(r.bin, '/opt/homebrew/bin/webc');
});

caseNamed('empty setting and nothing built: says which of the three lookups failed', () => {
  const r = findBinary({ root: ROOT, env: ENV, home: HOME, ...fsOf([]) });
  assert.strictEqual(r.ok, false);
  const msg = explain(r, '');
  assert.match(msg, /webc\.lsp\.path is empty/);
  assert.match(msg, /build\/webc/);
  assert.match(msg, /PATH/);
  assert.ok(r.checked.length >= 6, 'the walk up the tree should be recorded for the log');
});

// ---- the message must never blame the setting when the setting is the thing that works
caseNamed('a resolved path is what the message and the log show, never the raw tilde', () => {
  const bin = HOME + '/w/bin/webc';
  const r = findBinary({ configured: '~/w/bin/webc', root: ROOT, env: ENV, home: HOME, ...fsOf([]) });
  assert.strictEqual(r.bin, bin);
  assert.ok(!explain(r, '~/w/bin/webc').includes('cannot be executed'));
  assert.match(explain(r, '~/w/bin/webc'), new RegExp(bin.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')));
});

if (!process.exitCode) console.log(`  ok   editors/vscode/resolve.js — ${n} cases`);