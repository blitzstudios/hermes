// hermesc and hbc-link, built from this tree.

const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const { CACHE_VERSION, hermesDir, cacheRoot, sha256 } = require('./config');

const SOURCES = ['CMakeLists.txt', 'cmake', 'external', 'include', 'lib', 'tools'];

// Named after the sources the tools are built from, leaving out this folder. Hashing them takes about 0.1s.
function toolsDir() {
  const hash = crypto.createHash('sha256').update(`${CACHE_VERSION}\0${os.platform()}\0${os.arch()}\0`);
  const add = (relative) => {
    const file = path.join(hermesDir, relative);
    if (file === __dirname) return;
    if (fs.statSync(file).isDirectory()) {
      for (const name of fs.readdirSync(file).sort()) add(path.join(relative, name));
    } else {
      hash.update(`${relative}\0`).update(fs.readFileSync(file)).update('\0');
    }
  };
  SOURCES.forEach(add);
  return path.join(cacheRoot, `tools-${hash.digest('hex').slice(0, 16)}`);
}

function findCMake() {
  const works = (command) => {
    try {
      execFileSync(command, ['--version'], { stdio: 'ignore' });
      return true;
    } catch {
      return false;
    }
  };
  if (works('cmake')) return 'cmake';
  const sdk = process.env.ANDROID_HOME || process.env.ANDROID_SDK_ROOT || path.join(os.homedir(), 'Library/Android/sdk');
  const versions = fs.existsSync(path.join(sdk, 'cmake')) ? fs.readdirSync(path.join(sdk, 'cmake')) : [];
  versions.sort((a, b) => a.localeCompare(b, undefined, { numeric: true }));
  const cmake = versions.map((version) => path.join(sdk, 'cmake', version, 'bin/cmake')).reverse().find(works);
  if (!cmake) throw new Error('cmake is not installed');
  return cmake;
}

function findNinja(cmake) {
  const candidates = [path.join(path.dirname(cmake), 'ninja'), 'ninja'];
  return candidates.find((ninja) => {
    try {
      execFileSync(ninja, ['--version'], { stdio: 'ignore' });
      return true;
    } catch {
      return false;
    }
  });
}

function buildTools() {
  const dir = toolsDir();
  const tools = { hermesc: path.join(dir, 'hermesc'), hbcLink: path.join(dir, 'hbc-link') };
  if (fs.existsSync(tools.hermesc) && fs.existsSync(tools.hbcLink)) return tools;

  console.log(`Building hermesc and hbc-link from ${hermesDir}...`);
  const cmake = findCMake();
  const ninja = findNinja(cmake);
  const tmp = `${dir}.${crypto.randomBytes(4).toString('hex')}.tmp`;
  const build = path.join(tmp, 'build');
  try {
    execFileSync(cmake, [
      '-S', hermesDir,
      '-B', build,
      ...(ninja ? ['-G', 'Ninja', `-DCMAKE_MAKE_PROGRAM=${ninja}`] : []),
      '-DCMAKE_BUILD_TYPE=Release',
      '-DHERMES_ENABLE_INTL=OFF',
      '-DHERMES_ENABLE_DEBUGGER=OFF',
    ], { stdio: ['ignore', 'ignore', 'inherit'] });
    execFileSync(cmake, ['--build', build, '--target', 'hermesc', 'hbc-link', '--parallel', String(os.cpus().length)], {
      stdio: ['ignore', 'ignore', 'inherit'],
    });
    fs.copyFileSync(path.join(build, 'bin/hermesc'), path.join(tmp, 'hermesc'));
    fs.copyFileSync(path.join(build, 'bin/hbc-link'), path.join(tmp, 'hbc-link'));
    fs.rmSync(build, { recursive: true, force: true });
    try {
      fs.renameSync(tmp, dir);
    } catch (error) {
      if (!fs.existsSync(tools.hbcLink)) throw error;
    }
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
  return tools;
}

function checkBytecodeVersion(tools, otherHermesc) {
  const version = (hermesc) => execFileSync(hermesc, ['-version'], { encoding: 'utf8' }).match(/HBC bytecode version: (\d+)/)?.[1];
  const ours = version(tools.hermesc);
  const expected = version(otherHermesc);
  if (!ours || ours !== expected) {
    throw new Error(`this hermesc emits bytecode version ${ours}, and ${otherHermesc} emits ${expected}`);
  }
}

module.exports = { buildTools, checkBytecodeVersion };
