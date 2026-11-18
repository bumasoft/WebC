// The WebC language server, as a VS Code client.
//
// This file is a client and nothing else: it spawns `webc lsp`, speaks JSON-RPC over
// that process's stdin/stdout, and hands the answers to the editor's own APIs. It has
// no dependencies (not even vscode-languageclient) because the server's whole point is
// that a WebC project needs no runtime installed besides the binary it already builds.
//
// Positions: VS Code counts columns in UTF-16 code units and so does the server's
// default encoding, so a position crosses the wire unchanged.
'use strict';

const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const resolve = require('./resolve'); // where the binary is: testable without an editor

let channel = null;
let logFile = null;
let client = null;
const collection = vscode.languages.createDiagnosticCollection('webc');

function log(...parts) {
  const line = parts.map((p) => (typeof p === 'string' ? p : JSON.stringify(p))).join(' ');
  if (channel) channel.appendLine(line);
  if (logFile) {
    try {
      fs.appendFileSync(logFile, line + '\n');
    } catch (_) {}
  }
}

// ---------------------------------------------------------------- conversions

const SEVERITY = [
  vscode.DiagnosticSeverity.Error,
  vscode.DiagnosticSeverity.Warning,
  vscode.DiagnosticSeverity.Information,
  vscode.DiagnosticSeverity.Hint,
];

function range(r) {
  return new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character);
}

function position(p) {
  return { line: p.line, character: p.character };
}

const KINDS = {
  2: vscode.CompletionItemKind.Method,
  3: vscode.CompletionItemKind.Function,
  5: vscode.CompletionItemKind.Field,
  6: vscode.CompletionItemKind.Variable,
  7: vscode.CompletionItemKind.Class,
  8: vscode.CompletionItemKind.Interface,
  9: vscode.CompletionItemKind.Module,
  13: vscode.CompletionItemKind.Enum,
  14: vscode.CompletionItemKind.Keyword,
  20: vscode.CompletionItemKind.Struct,
  21: vscode.CompletionItemKind.Constant,
  22: vscode.CompletionItemKind.Struct,
};

const SYMBOLS = {
  5: vscode.SymbolKind.Class,
  10: vscode.SymbolKind.Enum,
  11: vscode.SymbolKind.Interface,
  12: vscode.SymbolKind.Function,
  13: vscode.SymbolKind.Variable,
  14: vscode.SymbolKind.Constant,
  22: vscode.SymbolKind.Struct,
  23: vscode.SymbolKind.Array,
};

// ---------------------------------------------------------------- the client

class Client {
  constructor(bin, args, root) {
    this.bin = bin;
    this.args = args;
    this.root = root;
    this.seq = 1;
    this.pending = new Map();
    this.buf = Buffer.alloc(0);
    this.started = false;
    this.dead = false; // no process, or never got one
    this.spoke = false; // the server answered something at least once
    this.reported = false; // one failure, one message
    this.stderrTail = [];
  }

  // Three different failures must not share one sentence — "set webc.lsp.path to the webc
  // binary" — shown even when the setting is set and correct. That is the fastest way to
  // teach someone their setting is being ignored. The path here is already resolved
  // (`~` and `${workspaceFolder}` expanded), so a wrong one is visible in the sentence,
  // and the reason (ENOENT / EACCES / exited) says which of the three it is.
  complain(what) {
    this.reported = true;
    const tail = this.stderrTail.length ? ` — server said: ${this.stderrTail.slice(-3).join(' | ')}` : '';
    log('cannot start: ' + what + tail);
    vscode.window.showErrorMessage(`WebC: ${what}${tail}`, 'Show log').then((pick) => {
      if (pick === 'Show log' && channel) channel.show();
    });
  }

  start() {
    log(`starting: ${this.bin} ${this.args.join(' ')} (cwd ${this.root})`);
    try {
      this.proc = cp.spawn(this.bin, this.args, { cwd: this.root, env: process.env });
    } catch (e) {
      this.dead = true;
      this.complain(`could not start \`${this.bin}\` — ${e.code || e.message}`);
      return Promise.resolve(null);
    }
    this.proc.on('error', (e) => {
      this.dead = true;
      this.started = false;
      const code = e.code || 'unknown';
      const reason =
        code === 'ENOENT'
          ? 'no such file — the path is expanded (~, ${workspaceFolder} and ${env:NAME} are understood), so this is the path itself'
          : code === 'EACCES'
            ? 'not executable — `chmod +x` it, or rebuild it with `make`'
            : code === 'ENOEXEC'
              ? 'not a program this machine can run (wrong architecture?)'
              : e.message;
      this.complain(`could not start \`${this.bin}\` — ${code}: ${reason}`);
    });
    this.proc.on('exit', (code, sig) => {
      const wasUp = this.started;
      this.started = false;
      this.dead = true;
      collection.clear();
      log(`server exited: code=${code} signal=${sig}`);
      if (!wasUp && !this.spoke && !this.reported) {
        this.complain(`\`${this.bin}\` started but exited at once (code ${code}${sig ? `, signal ${sig}` : ''})`);
      }
    });
    this.proc.stdout.on('data', (d) => this.onData(d));
    this.proc.stderr.on('data', (d) => {
      const s = d.toString().trim();
      if (s) this.stderrTail.push(s);
      if (this.stderrTail.length > 20) this.stderrTail.shift();
      log('[server] ' + s);
    });
    return this.request('initialize', {
      processId: process.pid,
      clientInfo: { name: 'webc-vscode' },
      rootUri: vscode.Uri.file(this.root).toString(),
      workspaceFolders: [{ uri: vscode.Uri.file(this.root).toString(), name: path.basename(this.root) }],
      capabilities: {
        textDocument: {
          synchronization: { dynamicRegistration: false },
          completion: { completionItem: { snippetSupport: false, documentationFormat: ['markdown', 'plaintext'] } },
          hover: { contentFormat: ['markdown', 'plaintext'] },
          definition: {},
          references: {},
          documentSymbol: { hierarchicalDocumentSymbolSupport: true },
          codeAction: { codeActionLiteralSupport: { codeActionKind: { valueSet: ['quickfix'] } } },
        },
        workspace: { workspaceFolders: true },
      },
    }).then((cap) => {
      if (!cap || !cap.capabilities) {
        // A process that never answered is not a running server: marking it started
        // would send every later request into a hole and leave diagnostics from a
        // previous session on screen.
        if (!this.reported) {
          this.complain(`\`${this.bin} lsp --root ${this.root}\` did not answer initialize — see the log`);
        }
        return null;
      }
      this.caps = cap.capabilities || {};
      log('initialized:', JSON.stringify(this.caps));
      this.notify('initialized', {});
      this.started = true;
      // whatever is already open belongs to the server too
      for (const doc of vscode.workspace.textDocuments) this.open(doc);
      return this;
    });
  }

  dispose() {
    this.started = false;
    this.dead = true;
    for (const p of this.pending.values()) {
      try {
        p.resolve(null);
      } catch (_) {}
    }
    this.pending.clear();
    try {
      if (this.proc) this.proc.kill();
    } catch (_) {}
    this.proc = null;
  }

  send(msg) {
    if (this.dead || !this.proc || !this.proc.stdin || !this.proc.stdin.writable) return;
    const body = JSON.stringify(Object.assign({ jsonrpc: '2.0' }, msg));
    this.proc.stdin.write(`Content-Length: ${Buffer.byteLength(body, 'utf8')}\r\n\r\n${body}`);
  }

  notify(method, params) {
    this.send({ method, params });
  }

  request(method, params) {
    if (!this.started && method !== 'initialize') return Promise.resolve(null);
    const id = this.seq++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, method });
      this.send({ id, method, params });
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          log(`timeout: ${method}`);
          resolve(null);
        }
      }, 10000);
    });
  }

  onData(chunk) {
    this.buf = Buffer.concat([this.buf, chunk]);
    for (;;) {
      const sep = this.buf.indexOf('\r\n\r\n');
      if (sep < 0) return;
      const header = this.buf.slice(0, sep).toString('ascii');
      const m = /Content-Length:\s*(\d+)/i.exec(header);
      if (!m) {
        this.buf = this.buf.slice(sep + 4);
        continue;
      }
      const len = parseInt(m[1], 10);
      if (this.buf.length < sep + 4 + len) return;
      const body = this.buf.slice(sep + 4, sep + 4 + len).toString('utf8');
      this.buf = this.buf.slice(sep + 4 + len);
      let msg = null;
      try {
        msg = JSON.parse(body);
      } catch (e) {
        log('bad json from server:', body.slice(0, 200));
        continue;
      }
      this.handle(msg);
    }
  }

  handle(msg) {
    this.spoke = true;
    if (msg.id !== undefined && msg.method === undefined) {
      const p = this.pending.get(msg.id);
      if (!p) return;
      this.pending.delete(msg.id);
      if (msg.error) {
        log(`error from server on ${p.method}:`, msg.error);
        p.resolve(null);
      } else {
        p.resolve(msg.result);
      }
      return;
    }
    if (msg.id !== undefined && msg.method) {
      // The server asks; this client has nothing to answer for it.
      this.send({ id: msg.id, result: null });
      return;
    }
    if (msg.method === 'textDocument/publishDiagnostics') {
      this.publish(msg.params);
      return;
    }
    if (msg.method === 'window/logMessage' || msg.method === 'window/showMessage') {
      log(`[server ${msg.params.type}] ${msg.params.message}`);
    }
  }

  publish(params) {
    const uri = vscode.Uri.parse(params.uri);
    const diags = (params.diagnostics || []).map((d) => {
      const diag = new vscode.Diagnostic(range(d.range), d.message, SEVERITY[(d.severity || 1) - 1]);
      diag.source = 'webc';
      diag.code = d.code;
      return diag;
    });
    collection.set(uri, diags);
    log(`${path.basename(uri.fsPath)}: ${diags.length} diagnostic(s)${diags.length ? ' — ' + diags.map((d) => `${d.code}: ${d.message}`).join(' | ') : ''}`);
  }

  // ---- document lifecycle
  open(doc) {
    if (!this.started || !wanted(doc)) return;
    this.notify('textDocument/didOpen', {
      textDocument: { uri: uriOf(doc), languageId: doc.languageId, version: doc.version, text: doc.getText() },
    });
  }

  change(doc) {
    if (!this.started || !wanted(doc)) return;
    this.notify('textDocument/didChange', {
      textDocument: { uri: uriOf(doc), version: doc.version },
      contentChanges: [{ text: doc.getText() }],
    });
  }

  save(doc) {
    if (!this.started || !wanted(doc)) return;
    this.notify('textDocument/didSave', {
      textDocument: { uri: uriOf(doc) },
      text: doc.getText(),
    });
  }

  close(doc) {
    if (!this.started || !wanted(doc)) return;
    this.notify('textDocument/didClose', { textDocument: { uri: uriOf(doc) } });
    collection.delete(vscode.Uri.parse(uriOf(doc)));
  }
}

// The server indexes real paths; a workspace opened through a symlink must still match.
function uriOf(doc) {
  try {
    return vscode.Uri.file(fs.realpathSync(doc.uri.fsPath)).toString();
  } catch (_) {
    return doc.uri.toString();
  }
}

function wanted(doc) {
  if (doc.uri.scheme !== 'file') return false;
  return /\.(c|h)$/.test(doc.uri.fsPath) || doc.languageId === 'c' || doc.languageId === 'cpp';
}

// ---------------------------------------------------------------- providers

function register() {
  const sel = [{ language: 'c' }, { language: 'cpp' }];

  vscode.languages.registerHoverProvider(sel, {
    provideHover: async (doc, pos) => {
      if (!client || !client.started || !wanted(doc)) return null;
      const r = await client.request('textDocument/hover', {
        textDocument: { uri: uriOf(doc) },
        position: position(pos),
      });
      if (!r || !r.contents) return null;
      const value = typeof r.contents === 'string' ? r.contents : r.contents.value || '';
      const md = new vscode.MarkdownString(value);
      md.isTrusted = false;
      return new vscode.Hover(md, r.range ? range(r.range) : undefined);
    },
  });

  vscode.languages.registerCompletionItemProvider(
    sel,
    {
      provideCompletionItems: async (doc, pos) => {
        if (!client || !client.started || !wanted(doc)) return null;
        const items = await client.request('textDocument/completion', {
          textDocument: { uri: uriOf(doc) },
          position: position(pos),
        });
        if (!items || !items.length) return null;
        const word = doc.getWordRangeAtPosition(pos, /[A-Za-z0-9_"<./:-]+/);
        return items.map((it) => {
          const item = new vscode.CompletionItem(it.label, KINDS[it.kind] || vscode.CompletionItemKind.Text);
          item.detail = it.detail;
          if (it.documentation) {
            item.documentation = new vscode.MarkdownString(
              typeof it.documentation === 'string' ? it.documentation : it.documentation.value || ''
            );
          }
          if (it.textEdit) {
            item.insertText = it.textEdit.newText;
            item.range = range(it.textEdit.range);
          } else if (word) {
            item.range = word;
          }
          return item;
        });
      },
    },
    '(',
    '"',
    '<',
    '.'
  );

  vscode.languages.registerDefinitionProvider(sel, {
    provideDefinition: async (doc, pos) => {
      if (!client || !client.started || !wanted(doc)) return null;
      const res = await client.request('textDocument/definition', {
        textDocument: { uri: uriOf(doc) },
        position: position(pos),
      });
      if (!res) return null;
      const list = Array.isArray(res) ? res : [res];
      return list.map((l) => new vscode.Location(vscode.Uri.parse(l.uri), range(l.range)));
    },
  });

  vscode.languages.registerReferenceProvider(sel, {
    provideReferences: async (doc, pos, ctx) => {
      if (!client || !client.started || !wanted(doc)) return null;
      const res = await client.request('textDocument/references', {
        textDocument: { uri: uriOf(doc) },
        position: position(pos),
        context: { includeDeclaration: ctx.includeDeclaration },
      });
      if (!res) return null;
      return res.map((l) => new vscode.Location(vscode.Uri.parse(l.uri), range(l.range)));
    },
  });

  vscode.languages.registerDocumentSymbolProvider(sel, {
    provideDocumentSymbols: async (doc) => {
      if (!client || !client.started || !wanted(doc)) return null;
      const res = await client.request('textDocument/documentSymbol', {
        textDocument: { uri: uriOf(doc) },
      });
      if (!res) return null;
      const conv = (s) => {
        const r = range(s.range);
        const sym = new vscode.DocumentSymbol(
          s.name,
          s.detail || '',
          SYMBOLS[s.kind] || vscode.SymbolKind.Variable,
          r,
          s.selectionRange ? range(s.selectionRange) : r
        );
        sym.children = (s.children || []).map(conv);
        return sym;
      };
      return res.map(conv);
    },
  });

  vscode.languages.registerCodeActionsProvider(
    sel,
    {
      provideCodeActions: async (doc, rng, ctx) => {
        if (!client || !client.started || !wanted(doc)) return null;
        const res = await client.request('textDocument/codeAction', {
          textDocument: { uri: uriOf(doc) },
          range: { start: position(rng.start), end: position(rng.end) },
          context: { diagnostics: ctx.diagnostics.map((d) => ({ range: d.range, message: d.message, code: d.code })) },
        });
        if (!res || !res.length) return null;
        return res.map((a) => {
          const action = new vscode.CodeAction(a.title, vscode.CodeActionKind.QuickFix);
          const edit = new vscode.WorkspaceEdit();
          const changes = (a.edit && a.edit.changes) || {};
          for (const uri of Object.keys(changes)) {
            for (const e of changes[uri]) edit.replace(vscode.Uri.parse(uri), range(e.range), e.newText);
          }
          action.edit = edit;
          return action;
        });
      },
    },
    { providedCodeActionKinds: [vscode.CodeActionKind.QuickFix] }
  );
}

// ---------------------------------------------------------------- activate

function rootOf() {
  const folders = vscode.workspace.workspaceFolders;
  if (!folders || !folders.length) return null;
  return folders[0].uri.fsPath;
}

// The client is disposable so a corrected setting does not need a window reload. That
// matters more than it looks: "I set the correct path and it is still failing" is what
// a setting read once at activation and never again feels like from the outside.
function stopServer() {
  if (client) {
    try {
      client.dispose();
    } catch (_) {}
  }
  client = null;
  collection.clear();
}

function startServer() {
  stopServer();
  const cfg = vscode.workspace.getConfiguration('webc.lsp');
  if (!cfg.get('enabled', true)) {
    log('disabled by configuration');
    return null;
  }
  const root = rootOf();
  if (!root) {
    log('no workspace folder: not starting');
    return null;
  }

  // Where it is: the setting (with ~ and ${…} expanded), else build/webc here or above,
  // else the PATH. See resolve.js — the expansion is the whole reason it is its own file.
  const configured = cfg.get('path', '') || '';
  const found = resolve.findBinary({ configured, root, env: process.env });
  log(`binary: ${found.bin} (${found.source}${found.problem ? ': ' + found.problem : ''})`);
  if (!found.ok) {
    const msg = resolve.explain(found, configured);
    log('not started — ' + msg.replace(/\n/g, ' '));
    vscode.window.showErrorMessage(`WebC: ${msg}`, 'Show log').then((pick) => {
      if (pick === 'Show log' && channel) channel.show();
    });
    return null;
  }

  const extra = cfg.get('args', []) || [];
  client = new Client(found.bin, ['lsp', '--root', root, ...extra], root);
  client.start().catch((e) => log('initialize failed:', e && e.message));
  return client;
}

function activate(context) {
  channel = vscode.window.createOutputChannel('WebC Language Server');
  logFile = path.join(require('os').tmpdir(), 'webc-lsp-client.log');
  try {
    fs.writeFileSync(logFile, '');
  } catch (_) {}
  log(`activate — ${vscode.env.appName || 'editor'} ${vscode.version || ''}`);

  register();
  startServer();

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((d) => client && client.open(d)),
    vscode.workspace.onDidChangeTextDocument((e) => client && client.change(e.document)),
    vscode.workspace.onDidSaveTextDocument((d) => client && client.save(d)),
    vscode.workspace.onDidCloseTextDocument((d) => client && client.close(d)),
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration('webc.lsp')) {
        log('webc.lsp changed: restarting the server');
        startServer();
      }
    }),
    vscode.commands.registerCommand('webc.showLog', () => channel.show()),
    vscode.commands.registerCommand('webc.restartServer', () => {
      startServer();
      vscode.window.setStatusBarMessage('WebC: language server restarted', 3000);
    }),
    collection,
    { dispose: () => stopServer() }
  );
  log(`active — workspace ${rootOf()}`);
}

function deactivate() {
  stopServer();
}

module.exports = { activate, deactivate };