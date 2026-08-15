// Loads the addon the way node loads any other one, and checks that what
// answers is really the cosmopolitan runtime rather than a file that
// merely opened.
//
//     node test/ape/napi/test.js ./cosmo_addon.node

const path = process.argv[2] || './cosmo_addon.node';
const addon = require(require('path').resolve(path));

let failed = false;
function check(ok, what) {
  console.log(`${ok ? 'ok' : 'FAIL'}: ${what}`);
  if (!ok) failed = true;
}

check(typeof addon.add === 'function', 'node found what the addon published');
check(addon.add(20, 22) === 42,
      'the guest added two numbers through its own heap');

const said = addon.probe();
console.log(`      ${said}`);
check(/^cosmo libc in node:/.test(said), 'the guest formatted that itself');

// the pid is node's, because this is node's process
const claimed = Number(/pid=(\d+)/.exec(said)[1]);
check(claimed === process.pid,
      `the guest agrees which process it's in (${claimed})`);

// and node is unharmed by having a second libc in the building
check(require('fs').existsSync(path), "node's own libc still works");

let sum = 0;
for (let i = 0; i < 100000; ++i) sum += addon.add(i, 1);
check(sum === 100000 * 1 + (99999 * 100000) / 2,
      '100k calls through the guest heap, under gc pressure');

console.log(failed ? 'the addon is not well' : 'all good');
process.exit(failed ? 1 : 0);
