// 对公共 CLI 实际导出的 Markdown 做数学感知解析，并调用真实 KaTeX。
import assert from 'node:assert/strict';
import fs from 'node:fs';
import katex from 'katex';
import MarkdownIt from 'markdown-it';
import texmath from 'markdown-it-texmath';

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
const parser = new MarkdownIt({html: true}).use(texmath, {
  engine, delimiters: 'dollars', katexOptions: {strict: 'ignore'},
});
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
  console.log(`${test.name}: KaTeX ${katex.version} PASS (${inputs.length} formulas)`);
}
