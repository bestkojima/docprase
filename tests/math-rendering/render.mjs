// 对公共 CLI 实际导出的 Markdown 做数学感知解析，并调用真实 KaTeX。
import assert from 'node:assert/strict';
import fs from 'node:fs';
import katex from 'katex';
import {createDocumentRenderer} from './document-renderer.mjs';

let test;
let inputs;
let errors;
let modes;
const engine = {
  renderToString(tex, options) {
    inputs.push(tex.trim());
    modes.push(Boolean(options.displayMode));
    try {
      assert(!/&(?:lt|gt|amp);/.test(tex), `${test.name}: 数学输入被 HTML 实体污染`);
      return katex.renderToString(tex, {...options, throwOnError: true, trust: false});
    } catch (error) {
      errors.push(String(error));
      throw error;
    }
  },
};
const parser = createDocumentRenderer(engine);
for (test of JSON.parse(fs.readFileSync(process.argv[2], 'utf8'))) {
  inputs = [];
  errors = [];
  modes = [];
  const html = parser.render(test.markdown);
  assert.deepEqual(errors, [], `${test.name}: KaTeX 排版失败`);
  assert.deepEqual(inputs, test.formulas, `${test.name}: 数学分隔符或 LaTeX 在解析时被改写`);
  assert.deepEqual(modes, test.display_modes, `${test.name}: 行内/显示属性未正确排版`);
  assert.equal((html.match(/class="katex"/g) || []).length, inputs.length);
  assert.equal((html.match(/<math /g) || []).length, inputs.length);
  assert(!html.includes('katex-error'));
  assert(!/<(?:script|img|a)[\s>]/i.test(html), `${test.name}: 模型文字成为活动 HTML`);
  if (test.name.includes('matrix') || test.name.includes('cases')) {
    assert((html.match(/<mtr>/g) || []).length >= 2, `${test.name}: 未形成多行数学排版`);
  }
  if (test.name === 'table-matrix-and-merged-cells') {
    assert(html.includes('rowspan="2"') && html.includes('colspan="2"'));
    assert(html.includes('[原文](https://example.invalid)'));
    assert(html.includes('&lt;img src=x onerror=alert(1)&gt;'));
    assert(html.includes('<br>'));
  }
  if (['table-currency-with-math', 'body-currency-with-math'].includes(test.name)) {
    assert(html.includes('价格 $5，公式 ') && html.includes('；再付 $10。'));
  }
  if (test.name === 'currency-in-code-span') assert(html.includes('<code>价格 $5 </code>'));
  if (test.name === 'body-currency-code-fence') assert(html.includes('价格 $5 \n'));
  if (test.name === 'table-math-across-break') {
    assert.equal((html.match(/<br>/g) || []).length, 2);
    assert(html.includes('条件<br>') && html.includes('<br>下一行 '));
  }
  console.log(`${test.name}: KaTeX ${katex.version} PASS (${inputs.length} formulas)`);
}
const standalone = createDocumentRenderer();
for (const markdown of [
  '<table><tr><td>$\\frac{a}$</td></tr></table>',
  '<table><tr><td>$\\unknown{x}$</td></tr></table>',
  '<table><tr><td>$x^2</td></tr></table>',
]) assert.throws(() => standalone.render(markdown), undefined, '损坏单元格公式不能冒充排版成功');
const literal = standalone.render('```html\n<table><tr><td>$x$</td></tr></table>\n```');
assert(!literal.includes('class="katex"') && literal.includes('$x$'));
const currency = standalone.render('<table><tr><td>价格 $5 and $10</td></tr></table>');
assert(!currency.includes('class="katex"') && currency.includes('$5 and $10'));
console.log('table-rendering-errors-and-literals: PASS');
