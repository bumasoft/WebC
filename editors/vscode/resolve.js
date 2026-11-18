// Where the server is. Separate from extension.js and free of `vscode`, of spawning and
// of the filesystem it did not ask for: a settings string is a string, and turning it
// into a path is a decision worth testing without an editor (`make lsp` does).
//
// The rule this file exists for: **a settings field is typed by a human, so it is a
// path in the sense their shell means, not in the sense spawn(2) means.** `~` is a
// promise in every terminal they have used, `${workspaceFolder}` is what VS Code's own
// settings UI suggests, and `spawn("~/…")` answers ENOENT to both. Expanding them here,
// once, is cheaper than the bug report that says "I set the correct path and it still
// fails" while the client quietly hands a literal tilde to the kernel.
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

// How far up to look for a framework checkout when the workspace is a WebC *app* kept
// inside the framework's own `examples/` (or anywhere else below it).
const PARENTS = 5;

function defaultIsFile(p) {
  try {
    return fs.statSync(p).isFile();
  } catch (_) {
    return false;
  }
}

function defaultIsExecutable(p) {
  try {
    fs.accessSync(p, fs.constants.X_OK);
    return true;
  } catch (_) {
    return false;
  }
}

function defaultExists(p) {
  try {
    fs.statSync(p);
    return true;
  } catch (_) {
    return false;
  }
}

// `~`, `${workspaceFolder}`, `${userHome}` and `${env:NAME}` — the four spellings that
// arrive in this field in practice. An unrecognised `${…}` is left alone rather than
// blanked: a path with a literal `${` in it is broken loudly, not silently.
function expand(raw, ctx) {
  let p = String(raw == null ? '' : raw).trim();
  if (!p) return '';
  const root = ctx.root || '';
  const home = ctx.home || '';
  p = p
    .replace(/\$\{workspaceFolder\}/g, root)
    .replace(/\$\{userHome\}/g, home)
    .replace(/\$\{env:([^}]+)\}/g, (m, name) => {
      const v = ctx.env && ctx.env[name];
      return v == null ? m : v;
    });
  if (p === '~') return home;
  if (p.startsWith('~/') || p.startsWith('~' + path.sep)) return path.join(home, p.slice(2));
  return p;
}

// Why a path is not a program. Empty string means it is one.
function inspect(p, fsx) {
  if (!p) return 'missing';
  if (fsx.isFile(p)) return fsx.isExecutable(p) ? '' : 'not-executable';
  return fsx.exists(p) ? 'directory' : 'missing';
}

// The sentence shown when it cannot start. It names the path that was actually tried —
// the resolved one — because "set webc.lsp.path" is useless advice to someone who has
// just set it.
function explain(result, configured) {
  const shown = result.bin || '(no path)';
  const setting = String(configured == null ? '' : configured).trim();
  switch (result.problem) {
    case 'directory':
      return `the language server was not started — \`${shown}\` is a directory, not the webc binary.`;
    case 'not-executable':
      return `the language server was not started — \`${shown}\` is not executable (\`chmod +x\` it, or rebuild with \`make\`).`;
    case 'missing':
      if (result.source === 'setting') {
        return (
          `the language server was not started — \`${shown}\` does not exist.\n` +
          `webc.lsp.path is \`${setting}\`, which resolves to \`${shown}\`; ` +
          `build it (\`make\` in the WebC checkout) or correct the setting.`
        );
      }
      return (
        `the language server was not started — no \`webc\` binary found: webc.lsp.path is empty, ` +
        `there is no \`build/webc\` in the workspace or in ${PARENTS} folders above it, and none on the PATH.`
      );
    default:
      return `the language server was not started — \`${shown}\` cannot be executed.`;
  }
}

// → { ok, bin, source, configured, problem, checked }
//   source: 'setting' | 'workspace' | 'parent' | 'PATH'
//   problem: '' | 'missing' | 'directory' | 'not-executable'
function findBinary(opts) {
  const o = opts || {};
  const root = o.root || '';
  const env = o.env || process.env;
  const home = o.home || (env && env.HOME) || os.homedir();
  const fsx = {
    isFile: o.isFile || defaultIsFile,
    isExecutable: o.isExecutable || defaultIsExecutable,
    exists: o.exists || defaultExists,
  };
  const parents = o.parents == null ? PARENTS : o.parents;
  const configured = String(o.configured == null ? '' : o.configured).trim();
  const ctx = { root: root, home: home, env: env };
  const checked = [];

  // 1. The setting, if the human wrote one. It wins — and when it is wrong, say so
  //    instead of falling back to some other binary: silently starting a *different*
  //    webc is how "my change made no difference" bugs are born.
  if (configured) {
    const expanded = expand(configured, ctx);
    const bin = path.isAbsolute(expanded) ? expanded : path.resolve(root || process.cwd(), expanded);
    const problem = inspect(bin, fsx);
    return { ok: !problem, bin: bin, source: 'setting', configured: configured, problem: problem, checked: [bin] };
  }

  // 2. `build/webc` in the workspace, then in its parents: a WebC app usually has a
  //    framework checkout above it rather than a binary of its own.
  if (root) {
    let dir = root;
    for (let i = 0; i <= parents; i++) {
      const candidate = path.join(dir, 'build', 'webc');
      checked.push(candidate);
      const problem = inspect(candidate, fsx);
      if (!problem) {
        return { ok: true, bin: candidate, source: i === 0 ? 'workspace' : 'parent', configured: '', problem: '', checked: checked };
      }
      if (problem === 'not-executable') {
        // A binary that is there but not executable is the answer to "where is it";
        // keep looking would only find a different one and hide the chmod.
        return { ok: false, bin: candidate, source: i === 0 ? 'workspace' : 'parent', configured: '', problem: problem, checked: checked };
      }
      const up = path.dirname(dir);
      if (up === dir) break;
      dir = up;
    }
  }

  // 3. The PATH — searched here rather than left to spawn, because a GUI editor does
  //    not inherit a login shell's PATH and the error spawn gives back ("ENOENT") does
  //    not say which half of the lookup failed.
  const dirs = String((env && env.PATH) || '').split(path.delimiter).filter(Boolean);
  const names = process.platform === 'win32' ? ['webc.exe', 'webc'] : ['webc'];
  for (const dir of dirs) {
    for (const name of names) {
      const candidate = path.join(dir, name);
      checked.push(candidate);
      if (!inspect(candidate, fsx)) {
        return { ok: true, bin: candidate, source: 'PATH', configured: '', problem: '', checked: checked };
      }
    }
  }

  return { ok: false, bin: 'webc', source: 'PATH', configured: '', problem: 'missing', checked: checked };
}

module.exports = { findBinary: findBinary, expand: expand, explain: explain, PARENTS: PARENTS };