#!/usr/bin/env node

// Compiles a Metro bundle to Hermes bytecode one module at a time, reusing the bytecode of modules
// compiled before, and links the result with hbc-link. The output runs like a whole-bundle compile.
// Anything this cannot handle exits with an error, and the caller should compile the whole bundle.
//
//   config.js   what the cache is keyed on, and where it keeps things
//   tools.js    builds hermesc and hbc-link from this tree, and checks the bytecode version they emit
//   split.js    splits a bundle into a skeleton and one unit per module
//   compile.js  compiles the units the cache is missing, and links the bundle
//
//   hermes-module-cache compile --bundle <in> --out <out.hbc> [--source-name <name>]
//       [--cache-dir <dir>]        where module bytecode is kept
//       [--match-hermesc <path>]   fails unless this tree's hermesc emits the same bytecode version
//   hermes-module-cache build-tools

const { buildTools } = require('./tools');
const { compileWithModuleCache } = require('./compile');

const [command, ...rest] = process.argv.slice(2);
const args = {};
for (let i = 0; i < rest.length; i += 2) args[rest[i].replace(/^--/, '')] = rest[i + 1];

const main = async () => {
  if (command === 'build-tools') {
    const tools = buildTools();
    console.log(`hermesc: ${tools.hermesc}\nhbc-link: ${tools.hbcLink}`);
  } else if (command === 'compile' && args.bundle && args.out) {
    const result = await compileWithModuleCache({
      bundle: args.bundle,
      out: args.out,
      sourceName: args['source-name'],
      cacheDir: args['cache-dir'],
      matchHermesc: args['match-hermesc'],
    });
    const seconds = (ms) => `${(ms / 1000).toFixed(1)}s`;
    console.log(
      `Compiled ${result.compiled} of ${result.modules} modules (split ${seconds(result.timings.split)}, ` +
        `compile ${seconds(result.timings.compile)}, link ${seconds(result.timings.link)}).`,
    );
  } else {
    throw new Error('usage: hermes-module-cache compile --bundle <in> --out <out> [--source-name <name>] ' +
      '[--cache-dir <dir>] [--match-hermesc <path>] | build-tools');
  }
};

main().catch((error) => {
  console.error(error.message);
  process.exit(1);
});
