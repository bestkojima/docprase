// Shared by the offline preview and the public-export rendering regressions.
import katex from 'katex';
import MarkdownIt from 'markdown-it';
import texmath from 'markdown-it-texmath';
import {parseFragment, serialize} from 'parse5';

export function createDocumentRenderer(engine = katex) {
  function withMath(parser) {
    parser.use(texmath, {engine, delimiters: 'dollars'});
    // texmath otherwise catches KaTeX errors and returns apparently successful
    // HTML. Keep errors observable and the engine local to this renderer.
    const rules = texmath.mergeDelimiters('dollars');
    for (const [entries, block] of [[rules.inline, false], [rules.block, true]]) {
      for (const rule of entries) {
        parser.renderer.rules[rule.name] = (tokens, index) => {
          const token = tokens[index];
          const html = engine.renderToString(token.content, {
            displayMode: block || Boolean(rule.displayMode),
            throwOnError: true, trust: false, strict: 'ignore',
          });
          return rule.tmpl.replace(/\$2/g, () => parser.utils.escapeHtml(token.info))
            .replace(/\$1/g, () => html);
        };
      }
    }
    return parser;
  }
  const parser = withMath(new MarkdownIt({html: true}));
  // Cell prose is literal HTML text, not another Markdown document. Recognize
  // only math delimiters, including legacy wrappers and multiline formulas.
  function renderCellText(source) {
    const delimiters = [['$$', '$$', true], ['\\[', '\\]', true],
      ['\\(', '\\)', false], ['$', '$', false]];
    const escaped = position => {
      let slashes = 0;
      while (position > 0 && source[--position] === '\\') slashes++;
      return slashes % 2 === 1;
    };
    let html = '', plain = 0;
    for (let position = 0; position < source.length;) {
      const delimiter = !escaped(position) && delimiters.find(([open]) => source.startsWith(open, position));
      if (!delimiter) { position++; continue; }
      const [open, close, displayMode] = delimiter;
      let end = source.indexOf(close, position + open.length);
      while (end !== -1 && escaped(end)) end = source.indexOf(close, end + close.length);
      // An unpaired currency amount is ordinary text, not broken TeX.
      if (open === '$' && /^\d/.test(source.slice(position + 1)) &&
          (end === -1 || /^\d/.test(source.slice(end + 1)))) { position++; continue; }
      if (end === -1) throw new SyntaxError('表格单元格数学分隔符未闭合');
      const latex = source.slice(position + open.length, end).trim();
      if (!latex) throw new SyntaxError('表格单元格数学内容为空');
      html += parser.utils.escapeHtml(source.slice(plain, position));
      html += engine.renderToString(latex, {
        displayMode, throwOnError: true, trust: false, strict: 'ignore',
      });
      position = plain = end + close.length;
    }
    return html + parser.utils.escapeHtml(source.slice(plain));
  }
  const allowed = new Set(['table', 'thead', 'tbody', 'tfoot', 'tr', 'td', 'th', 'br']);
  parser.renderer.rules.html_block = (tokens, index) => {
    const source = tokens[index].content;
    const fragment = parseFragment(source);
    function safe(node) {
      if (node.nodeName === '#text') return true;
      if (node.tagName && !allowed.has(node.tagName)) return false;
      if ((node.attrs || []).some(({name, value}) =>
        !['td', 'th'].includes(node.tagName) ||
        !['rowspan', 'colspan'].includes(name) || !/^[1-9][0-9]?$/.test(value))) return false;
      return (node.childNodes || []).every(safe);
    }
    if (!fragment.childNodes.some(node => node.tagName === 'table') || !safe(fragment))
      return parser.utils.escapeHtml(source);
    function renderCells(node) {
      if (node.tagName === 'td' || node.tagName === 'th') {
        const children = [];
        for (const child of node.childNodes) {
          if (child.nodeName === '#text') {
            const rendered = parseFragment(renderCellText(child.value));
            children.push(...rendered.childNodes);
          } else children.push(child);
        }
        node.childNodes = children;
        for (const child of children) child.parentNode = node;
      } else for (const child of node.childNodes || []) renderCells(child);
    }
    renderCells(fragment);
    return serialize(fragment);
  };
  return parser;
}
