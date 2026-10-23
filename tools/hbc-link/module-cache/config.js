// What the module cache is keyed on, and where it keeps things.

const crypto = require('crypto');
const path = require('path');

const CACHE_VERSION = 1;
const UNIT_FLAGS = ['-O', '-g2', '-Xcontent-template-ids', '-emit-binary'];
const PRUNE_AFTER_DAYS = 14;

const hermesDir = path.join(__dirname, '../../..');
const installed = path.basename(path.join(hermesDir, '../..')) === 'node_modules';
const cacheRoot = installed
  ? path.join(hermesDir, '../../.cache/hermes-module-cache')
  : path.join(hermesDir, 'build/hermes-module-cache');

const sha256 = (...parts) => {
  const hash = crypto.createHash('sha256');
  for (const part of parts) hash.update(part).update('\0');
  return hash.digest('hex');
};

module.exports = {
  CACHE_VERSION,
  UNIT_FLAGS,
  PRUNE_AFTER_DAYS,
  hermesDir,
  cacheRoot,
  modulesDir: path.join(cacheRoot, 'modules'),
  sha256,
};
