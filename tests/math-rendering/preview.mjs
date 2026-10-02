// Render a public CLI document.md to a sibling, offline document.html.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createDocumentRenderer} from './document-renderer.mjs';

const input = process.argv[2];
if (process.argv.length !== 3 || !input?.endsWith('.md')) {
  console.error('用法：node tests/math-rendering/preview.mjs 作业目录/document.md');
  process.exit(2);
}
const renderer = createDocumentRenderer();
const body = renderer.render(fs.readFileSync(input, 'utf8'));
const katexDist = path.dirname(fileURLToPath(import.meta.resolve('katex')));
const css = fs.readFileSync(path.join(katexDist, 'katex.min.css'), 'utf8')
  .replace(/url\((fonts\/[^)]+)\)/g, (_, font) => {
    const extension = path.extname(font).slice(1);
    const data = fs.readFileSync(path.join(katexDist, font)).toString('base64');
    return `url(data:font/${extension};base64,${data})`;
  });
const output = input.slice(0, -3) + '.html';
const html = `<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>文档预览</title><style>${css}
body{max-width:1100px;margin:36px auto;padding:0 24px;font:18px/1.7 sans-serif}
table{border-collapse:collapse;max-width:100%}td,th{border:1px solid #ccc;padding:10px}
img{max-width:100%}.katex-display{overflow-x:auto;overflow-y:hidden}
</style><body>${body}</body></html>`;
fs.writeFileSync(output, html, {flag: 'wx'});
console.log(output);
