// Splits a Metro bundle into a skeleton and one unit per module.

const FIRST_LINE = /^__d\(function ?\([\w$, ]*\) ?\{$/;
const LAST_LINE = /^\},\d+,\[(?:(?:\d+|null)(?:,(?:\d+|null))*)?\]\);$/;

// Each multi-line module becomes a unit, and the skeleton keeps a one-line stub in its place, made of
// the module's first and last lines. A unit ends in `});` instead of its own last line, which holds
// the module id and dependencies; those belong to the skeleton, and leaving them out keeps a module's
// cache key stable when ids shift.
function split(bundle) {
  const lines = bundle.split('\n');
  if (lines[lines.length - 1] === '') lines.pop();

  const skeleton = [];
  const units = [];
  const inline = [];
  let tailStart = -1;
  let i = 0;
  while (i < lines.length && !lines[i].startsWith('__d(')) skeleton.push(lines[i++]);
  while (i < lines.length) {
    const line = lines[i];
    if (line.startsWith('__r(')) {
      tailStart = i;
      break;
    }
    if (!line.startsWith('__d(')) throw new Error(`unexpected line ${i + 1} between modules`);
    let end = i + 1;
    while (end < lines.length && !lines[end].startsWith('__d(') && !lines[end].startsWith('__r(')) end++;
    if (end === i + 1) {
      skeleton.push(line);
      inline.push({ skelLine: skeleton.length, bundleLine: i + 1 });
    } else {
      const last = lines[end - 1];
      if (!FIRST_LINE.test(line) || !LAST_LINE.test(last)) {
        throw new Error(`module at line ${i + 1} has an unexpected shape`);
      }
      skeleton.push(line + last);
      units.push({
        text: [...lines.slice(i, end - 1), '});'].join('\n') + '\n',
        skelLine: skeleton.length,
        splitCol: line.length + 1,
        bundleStart: i + 1,
        bundleEnd: end,
      });
    }
    i = end;
  }
  if (tailStart < 0) throw new Error('no __r( line after the modules');
  const tailSkelLine = skeleton.length + 1;
  skeleton.push(...lines.slice(tailStart));
  return { skeleton: skeleton.join('\n') + '\n', units, inline, tail: { skelLine: tailSkelLine, bundleLine: tailStart + 1 } };
}

// A unit reads the skeleton's globals as properties of the global object, which top-level lexical
// declarations are not, and a top-level directive would change how every module compiles.
function checkSkeleton(skeleton) {
  const { parseSync } = require('@babel/core');
  const ast = parseSync(skeleton, { configFile: false, babelrc: false, sourceType: 'script' });
  if (ast.program.directives.length > 0) throw new Error('the bundle has a top-level directive');
  for (const node of ast.program.body) {
    if ((node.type === 'VariableDeclaration' && node.kind !== 'var') || node.type === 'ClassDeclaration') {
      throw new Error(`the bundle declares a top-level ${node.kind ?? 'class'} on line ${node.loc.start.line}`);
    }
  }
}

module.exports = { split, checkSkeleton };
