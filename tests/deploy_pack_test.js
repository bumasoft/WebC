// `webc deploy --pack` and `webc ci` write files that must be true to the app they
// were stamped from: the port from webc.json, the name everywhere, and no promise
// about a route the framework does not serve (a healthcheck that "works" in the
// template and fails on the platform is worse than none).
//
// The pack runs a real release build of a real scaffold, the way a developer would.
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

function has(file, text, what) {
  const body = fs.readFileSync(file, 'utf8');
  if (!body.includes(text)) fail(path.basename(file) + ': missing ' + (what || JSON.stringify(text)));
  return body;
}

(function main() {
  console.log('deploy: the packs are stamped from the app that built them');
  if (!fs.existsSync(CLI)) { console.log('  SKIP: no build/webc (run make cli)'); return; }

  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'webc-deploy-'));
  try {
    const dir = path.join(root, 'api');
    const made = run(['new', dir, '--template=server'], root);
    if (!made.ok) { fail('webc new failed\n' + made.out.slice(0, 400)); return; }

    // the server template's webc.json says port 8080; every stamped file must agree
    const d = run(['deploy', '--pack', 'all', '--out', 'disttest'], dir);
    if (!d.ok) { fail('webc deploy --pack all failed\n' + d.out.slice(0, 800)); return; }

    const out = path.join(dir, 'disttest');
    const docker = has(path.join(out, 'Dockerfile'), 'EXPOSE 8080');
    has(path.join(out, 'Dockerfile'), 'COPY api run api.manifest.json ./');
    if (!/ARG BASE=/.test(docker)) fail('Dockerfile: the base must be an ARG, not a choice');
    has(path.join(out, 'fly.toml'), 'internal_port = 8080');
    const unit = has(path.join(out, 'api.service'), 'ExecStart=/opt/webc/api/api');
    has(path.join(out, 'api.service'), 'DynamicUser=yes');
    // 8080 is unprivileged: a capability grant would be a line nobody can explain
    if (unit.includes('CAP_NET_BIND_SERVICE')) fail('api.service: CAP_NET_BIND_SERVICE at port 8080');
    // the server template's database is a sqlite file: the unit must allow writing it
    if (!unit.includes('ReadWritePaths=')) fail('api.service: no ReadWritePaths for a sqlite file');
    const caddy = has(path.join(out, 'Caddyfile.webc'), 'flush_interval -1');
    if (!/reverse_proxy 127\.0\.0\.1:8080/.test(caddy)) fail('Caddyfile.webc: wrong upstream port');
    const nginx = has(path.join(out, 'nginx-webc.conf'), 'proxy_buffering off');
    if (!/proxy_read_timeout 24h/.test(nginx)) fail('nginx-webc.conf: an SSE stream needs the long read timeout');
    has(path.join(out, 'DEPLOY.md'), 'Deploying api');
    if (!fs.existsSync(path.join(out, 'api'))) fail('disttest/api: the binary itself is missing');
    if (!fs.existsSync(path.join(out, 'api.manifest.json'))) fail('the manifest is missing');

    // unknown pack names are refused, not guessed at
    const bad = run(['deploy', '--pack', 'kubernetes'], dir);
    if (bad.ok) fail('an unknown pack name was accepted');

    // `webc ci` writes the gate as a workflow, and only inside an app
    const ci = run(['ci'], dir);
    if (!ci.ok) { fail('webc ci failed\n' + ci.out.slice(0, 400)); return; }
    const wf = path.join(dir, '.github', 'workflows', 'webc.yml');
    if (!fs.existsSync(wf)) { fail('webc ci wrote no workflow'); }
    else {
      has(wf, 'webc check', 'the gate itself');
      if (!/WEBC_REF: v\d/.test(fs.readFileSync(wf, 'utf8'))) fail('webc.yml: no framework pin');
    }
    const again = run(['ci'], dir);
    if (again.ok) fail('webc ci overwrote a workflow it had already written');
    const outside = run(['ci'], root);
    if (outside.ok) fail('webc ci ran outside an app');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
})();
