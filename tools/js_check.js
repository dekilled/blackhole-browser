// Prints "OK\tpath" or "ERR\tpath\tmessage" for every path read from stdin (classic script syntax, like <script>).
const fs = require('fs'), vm = require('vm');
for (const f of fs.readFileSync(0, 'utf8').split('\n').filter(Boolean)) {
  try { new vm.Script(fs.readFileSync(f, 'utf8'), { filename: f }); console.log('OK\t' + f); }
  catch (e) { console.log('ERR\t' + f + '\t' + String(e.message).split('\n')[0]); }
}
