import * as os from 'os';
import * as std from 'std';
import * as path from 'path';
import inspect from 'inspect';
import { Predicate } from 'predicate';
import { Location, Lexer, Token } from 'lexer';
import Console from '../lib/console.js';
import ECMAScriptLexer from '../lib/lexer/ecmascript.js';
import CLexer from '../lib/lexer/c.js';
import BNFLexer from '../lib/lexer/bnf.js';
import Parser, { DumpToken } from '../lib/parser.js';
import EBNFParser from '../lib/parser/ebnf.js';
import extendArray from '../lib/extendArray.js';

('use math');

let code = 'C';
Error.stackTraceLimit = Infinity;

extendArray(Array.prototype);

function WriteFile(file, str) {
  let f = std.open(file, 'w+');
  f.puts(str);
  let pos = f.tell();
  console.log('Wrote "' + file + '": ' + pos + ' bytes');
  f.close();
  return pos;
}

function DumpLexer(lex) {
  const { size, pos, start, line, column, lineStart, lineEnd, columnIndex } = lex;

  return `Lexer ${inspect({ start, pos, size })}`;
}

function InstanceOf(obj, ctor) {
  return typeof obj == 'object' && obj != null && obj instanceof ctor;
}
function IsRegExp(regexp) {
  return InstanceOf(regexp, RegExp);
}

function RegExpToArray(regexp) {
  //console.log("RegExpToArray", regexp);
  const { source, flags } = regexp;
  return [Lexer.unescape(source), flags];
}

function LoadScript(file) {
  let code = std.loadFile(file);
  //console.log('LoadScript', { code });
  return std.evalScript(code, {});
}

function WriteObject(file, obj, fn = arg => arg) {
  return WriteFile(
    file,
    fn(
      inspect(obj, {
        colors: false,
        breakLength: 120,
        maxStringLength: Infinity,
        maxArrayLength: Infinity,
        compact: 3,
        multiline: true
      })
    )
  );
}

function* Range(start, end) {
  for(let i = start | 0; i <= end; i++) yield i;
}

function* MatchAll(regexp, str) {
  let match;
  while((match = regexp.exec(str))) yield match;
}

function main(...args) {
  globalThis.console = new Console({
    inspectOptions: {
      colors: true,
      depth: 2,
      maxArrayLength: Infinity,
      maxStringLength: Infinity,
      breakLength: 80,
      compact: 2,
      showHidden: false,
      customInspect: true
    }
  });
  console.log('console.options', console.options);
  let optind = 0;
  while(args[optind] && args[optind].startsWith('-')) {
    if(/code/.test(args[optind])) {
      code = globalThis.code = args[++optind].toUpperCase();
    }

    optind++;
  } /*
  function TestRegExp(char) {
    let re;
    TryCatch(() => (re = new RegExp(char))).catch(err => (re = new RegExp((char = Lexer.escape(char))))
    )();
    let source = re ? RegExpToString(re) : undefined;
    if(char != source) throw new Error(`'${char}' != '${source}'`);
    console.log({ char, re, source, ok: char == source });
  }

  [...Range(0, 127)].map(code => {
    let char = String.fromCharCode(code);
    TestRegExp(char);
  });
  TestRegExp('\b');
  TestRegExp('\\b');*/

  let file = args[optind] ?? path.join(path.dirname(process.argv[1]), '..', 'tests/Shell-Grammar.y');
  let outputFile = args[optind + 1] ?? 'grammar.kison';
  console.log('file:', file);
  let str = std.loadFile(file, 'utf-8');
  console.log('str:', str.slice(0, 50) + '...');
  let len = str.length;
  let type = path.extname(file).substring(1);

  let grammar = null; //LoadScript(outputFile);

  let parser = new EBNFParser(grammar);

  parser.setInput(str, file);

  grammar = parser.parse();
  if(grammar) {
    WriteObject(
      'grammar.kison',
      grammar,
      str => `(function () {\n    return ` + str.replace(/\n/g, '\n    ') + `;\n\n})();`
    );
    //  console.log('grammar:', grammar);
  }
  std.gc();
  return !!grammar;
}

try {
  main(...scriptArgs.slice(1));
} catch(error) {
  console.log(`FAIL: ${error.message}\n${error.stack}`);
  std.exit(1);
} finally {
  console.log('SUCCESS');
}
