// Compiles the modules the cache is missing, and links the bundle from the cache with hbc-link.

const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFile } = require('child_process');
const { promisify } = require('util');
const { CACHE_VERSION, UNIT_FLAGS, PRUNE_AFTER_DAYS, modulesDir, sha256 } = require('./config');
const { buildTools, checkBytecodeVersion } = require('./tools');
const { split, checkSkeleton } = require('./split');

const run = promisify(execFile);

async function pool(items, limit, fn) {
  let next = 0;
  let failure = null;
  const worker = () => {
    if (failure || next >= items.length) return Promise.resolve();
    return fn(items[next++]).then(worker, (error) => {
      failure ??= error;
    });
  };
  await Promise.all(Array.from({ length: Math.min(limit, items.length) }, worker));
  if (failure) throw failure;
}

function prune(cacheDir, used) {
  const now = new Date();
  for (const file of used) fs.utimesSync(file, now, now);
  const cutoff = now.getTime() - PRUNE_AFTER_DAYS * 24 * 60 * 60 * 1000;
  for (const name of fs.readdirSync(cacheDir)) {
    const file = path.join(cacheDir, name);
    if (fs.statSync(file).mtimeMs < cutoff) fs.rmSync(file, { force: true });
  }
}

async function compileWithModuleCache({
  bundle,
  out,
  sourceName = bundle,
  sourceMap = true,
  cacheDir = modulesDir,
  tools = buildTools(),
  jobs = os.cpus().length,
  matchHermesc,
}) {
  const timings = {};
  let started = Date.now();
  const lap = (name) => {
    timings[name] = Date.now() - started;
    started = Date.now();
  };

  if (matchHermesc) checkBytecodeVersion(tools, matchHermesc);
  const parts = split(fs.readFileSync(bundle, 'utf8'));
  checkSkeleton(parts.skeleton);
  lap('split');

  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'hermes-module-cache-'));
  try {
    if (/\s/.test(work + cacheDir)) throw new Error('hbc-link manifests cannot hold paths with spaces');
    fs.mkdirSync(cacheDir, { recursive: true });
    const compiler = sha256(String(CACHE_VERSION), fs.readFileSync(tools.hermesc), UNIT_FLAGS.join(' '));
    const missing = new Map();
    for (const unit of parts.units) {
      unit.hbc = path.join(cacheDir, `${sha256(compiler, unit.text)}.hbc`);
      if (!fs.existsSync(unit.hbc)) missing.set(unit.hbc, unit);
    }

    const compile = async ({ input, output }) => {
      const tmp = `${output}.${crypto.randomBytes(4).toString('hex')}.tmp`;
      try {
        await run(tools.hermesc, [...UNIT_FLAGS, '-out', tmp, input], { maxBuffer: 64 * 1024 * 1024 });
        fs.renameSync(tmp, output);
      } catch (error) {
        throw new Error(`hermesc failed on ${input}: ${error.stderr || error.message}`);
      } finally {
        fs.rmSync(tmp, { force: true });
      }
    };

    const skeletonJs = path.join(work, 'skeleton.js');
    const skeletonHbc = path.join(work, 'skeleton.hbc');
    fs.writeFileSync(skeletonJs, parts.skeleton);
    const tasks = [{ input: skeletonJs, output: skeletonHbc }];
    for (const unit of missing.values()) {
      const input = path.join(work, `${tasks.length}.js`);
      fs.writeFileSync(input, unit.text);
      tasks.push({ input, output: unit.hbc });
    }
    await pool(tasks, jobs, compile);
    lap('compile');

    const manifest = [
      `tail ${parts.tail.skelLine} ${parts.tail.bundleLine}`,
      ...parts.inline.map((entry) => `line ${entry.skelLine} ${entry.bundleLine}`),
      ...parts.units.map((unit) => `unit ${unit.hbc} ${unit.skelLine} ${unit.splitCol} ${unit.bundleStart} ${unit.bundleEnd} 1`),
    ];
    const manifestPath = path.join(work, 'manifest.txt');
    fs.writeFileSync(manifestPath, manifest.join('\n') + '\n');
    await run(tools.hbcLink, [
      '-skeleton', skeletonHbc,
      '-manifest', manifestPath,
      '-source', bundle,
      '-source-name', sourceName,
      '-out', out,
      ...(sourceMap ? ['-output-source-map'] : []),
    ], { maxBuffer: 64 * 1024 * 1024 }).catch((error) => {
      throw new Error(`hbc-link failed: ${error.stderr || error.message}`);
    });
    lap('link');

    prune(cacheDir, new Set(parts.units.map((unit) => unit.hbc)));
    return { modules: parts.units.length, compiled: missing.size, timings };
  } finally {
    fs.rmSync(work, { recursive: true, force: true });
  }
}

module.exports = { compileWithModuleCache };
