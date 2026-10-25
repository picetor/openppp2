'use strict';
// 最小 DOM 模型：只实现 web/app.js 实际用到的 API，用来在没有浏览器的环境里
// 执行面板脚本并观察“刷新是否替换了节点”。不是通用 DOM 实现，不要用于其他页面。

const VOID = new Set(['input', 'br', 'link', 'meta', 'img', 'hr', 'source', 'area', 'base', 'col', 'embed', 'param', 'track', 'wbr']);
const ENTITIES = { amp: '&', lt: '<', gt: '>', quot: '"', '#39': "'" };
function decode(text) {
  return text.replace(/&(amp|lt|gt|quot|#39|#\d+);/g, (match, entity) => ENTITIES[entity] ?? String.fromCharCode(Number(entity.slice(1))));
}

class TextNode {
  constructor(text) { this.nodeType = 3; this._text = text; this.parentNode = null; }
  get textContent() { return this._text; }
  set textContent(value) { this._text = value; }
  get isConnected() { let node = this; while (node.parentNode) node = node.parentNode; return node.tag === '#document'; }
}

class Element {
  constructor(tag) {
    this.nodeType = 1; this.tag = tag; this.nodes = []; this.parentNode = null;
    this.attrs = new Map(); this.listeners = new Map(); this.style = {};
    this.hidden = false; this.disabled = false; this.value = ''; this.checked = false; this.type = '';
  }
  get children() { return this.nodes.filter(node => node.nodeType === 1); }
  get childElementCount() { return this.children.length; }
  get firstElementChild() { return this.children[0] || null; }
  get lastElementChild() { const children = this.children; return children[children.length - 1] || null; }
  get textContent() { return this.nodes.map(node => node.textContent).join(''); }
  set textContent(value) {
    this.nodes = [];
    if (value !== '') { const text = new TextNode(String(value)); text.parentNode = this; this.nodes.push(text); }
  }
  get className() { return this.attrs.get('class') || ''; }
  set className(value) { this.attrs.set('class', String(value)); }
  get id() { return this.attrs.get('id') || ''; }
  get classList() {
    if (!this._classList) {
      const has = name => this.className.split(/\s+/).includes(name);
      this._classList = {
        contains: has,
        add: name => { if (!has(name)) this.className = (this.className + ' ' + name).trim(); },
        remove: name => { this.className = this.className.split(/\s+/).filter(item => item && item !== name).join(' '); },
        toggle: (name, on) => { const want = on === undefined ? !has(name) : !!on; if (want === has(name)) return want; want ? this.classList.add(name) : this.classList.remove(name); return want; }
      };
    }
    return this._classList;
  }
  get dataset() {
    if (!this._dataset) {
      const self = this;
      const attribute = key => 'data-' + String(key).replace(/[A-Z]/g, letter => '-' + letter.toLowerCase());
      this._dataset = new Proxy({}, {
        get: (_, key) => self.attrs.get(attribute(key)),
        set: (_, key, value) => { self.attrs.set(attribute(key), String(value)); return true; },
        has: (_, key) => self.attrs.has(attribute(key))
      });
    }
    return this._dataset;
  }
  setAttribute(name, value) {
    const key = String(name).toLowerCase(), text = String(value);
    this.attrs.set(key, text);
    if (key === 'hidden') this.hidden = true;
    if (key === 'disabled') this.disabled = true;
    if (key === 'value') this.value = text;
    if (key === 'type') this.type = text;
  }
  getAttribute(name) { const key = String(name).toLowerCase(); return this.attrs.has(key) ? this.attrs.get(key) : null; }
  hasAttribute(name) { return this.attrs.has(String(name).toLowerCase()); }
  append(...nodes) { for (const node of nodes) { if (!node) continue; node.parentNode = this; this.nodes.push(node); } }
  replaceChildren(...nodes) { this.nodes = []; this.append(...nodes); }
  contains(node) { let current = node; while (current) { if (current === this) return true; current = current.parentNode; } return false; }
  get isConnected() { let node = this; while (node.parentNode) node = node.parentNode; return node.tag === '#document'; }
  get innerHTML() { return ''; }
  set innerHTML(html) { this.nodes = []; this.append(...parseHtml(String(html))); }
  querySelector(selector) { return selectAll(this, selector)[0] || null; }
  querySelectorAll(selector) { return selectAll(this, selector); }
  closest(selector) {
    const compound = parseCompound(selector.trim().split(/\s+/)[0]);
    let node = this;
    while (node && node.nodeType === 1) { if (matches(node, compound)) return node; node = node.parentNode; }
    return null;
  }
  addEventListener(type, handler) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(handler);
  }
}

class Document {
  constructor() { this.nodeType = 9; this.tag = '#document'; this.nodes = []; this.body = null; }
  createElement(tag) { return new Element(tag); }
  createTextNode(text) { return new TextNode(text); }
  append(...nodes) { for (const node of nodes) { node.parentNode = this; this.nodes.push(node); } }
  get children() { return this.nodes.filter(node => node.nodeType === 1); }
  getElementById(id) {
    let found = null;
    walk(this, element => { if (!found && element.attrs.get('id') === id) found = element; });
    return found;
  }
  querySelector(selector) { return selectAll(this, selector)[0] || null; }
  querySelectorAll(selector) { return selectAll(this, selector); }
  addEventListener() { }
}

function walk(node, visit) {
  for (const child of node.nodes) if (child.nodeType === 1) { visit(child); walk(child, visit); }
}
function descendants(node, out = []) {
  for (const child of node.nodes) if (child.nodeType === 1) { out.push(child); descendants(child, out); }
  return out;
}

function parseHtml(html) {
  const root = new Element('#fragment');
  const stack = [root];
  let index = 0;
  const text = value => {
    if (!value) return;
    const node = new TextNode(decode(value));
    const parent = stack[stack.length - 1];
    node.parentNode = parent;
    parent.nodes.push(node);
  };
  while (index < html.length) {
    const open = html.indexOf('<', index);
    if (open < 0) { text(html.slice(index)); break; }
    if (open > index) text(html.slice(index, open));
    if (html.startsWith('<!', open)) { const end = html.indexOf('>', open); if (end < 0) break; index = end + 1; continue; }
    const end = html.indexOf('>', open);
    if (end < 0) { text(html.slice(open)); break; }
    let token = html.slice(open + 1, end);
    if (token.startsWith('/')) {
      const name = token.slice(1).trim().toLowerCase();
      for (let depth = stack.length - 1; depth > 0; depth--) if (stack[depth].tag === name) { stack.length = depth; break; }
      index = end + 1; continue;
    }
    const selfClosing = token.endsWith('/');
    if (selfClosing) token = token.slice(0, -1).trim();
    const tag = /^([a-zA-Z0-9:-]+)/.exec(token);
    if (!tag) { index = end + 1; continue; }
    const element = new Element(tag[1].toLowerCase());
    const attributes = /([a-zA-Z0-9_:-]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'>]+)))?/g;
    const rest = token.slice(tag[1].length);
    let attribute;
    while ((attribute = attributes.exec(rest))) element.setAttribute(attribute[1], decode(attribute[2] ?? attribute[3] ?? attribute[4] ?? ''));
    stack[stack.length - 1].append(element);
    if (!selfClosing && !VOID.has(element.tag)) stack.push(element);
    index = end + 1;
  }
  return root.nodes;
}

function parseCompound(part) {
  const compound = { tag: null, id: null, classes: [], attrs: [] };
  let rest = part;
  const tag = /^([a-zA-Z][a-zA-Z0-9-]*)/.exec(rest);
  if (tag) { compound.tag = tag[1].toLowerCase(); rest = rest.slice(tag[0].length); }
  while (rest.length) {
    if (rest[0] === '.' || rest[0] === '#') {
      const match = /^[.#]([A-Za-z0-9_-]+)/.exec(rest);
      if (!match) break;
      if (rest[0] === '.') compound.classes.push(match[1]); else compound.id = match[1];
      rest = rest.slice(match[0].length);
    } else if (rest[0] === '[') {
      const match = /^\[([A-Za-z0-9_-]+)(?:=(?:"([^"]*)"|'([^']*)'|([^\]]*)))?\]/.exec(rest);
      if (!match) break;
      compound.attrs.push([match[1], match[2] ?? match[3] ?? match[4] ?? null]);
      rest = rest.slice(match[0].length);
    } else break;
  }
  return compound;
}

function matches(element, compound) {
  if (element.nodeType !== 1) return false;
  if (compound.tag && element.tag !== compound.tag) return false;
  if (compound.id && element.attrs.get('id') !== compound.id) return false;
  for (const name of compound.classes) if (!element.className.split(/\s+/).includes(name)) return false;
  for (const [name, value] of compound.attrs) {
    if (!element.attrs.has(name)) return false;
    if (value !== null && element.attrs.get(name) !== value) return false;
  }
  return true;
}

function selectAll(root, selector) {
  const parts = selector.trim().split(/\s+/);
  let index = 0, current = [root], direct = false;
  if (parts[0] === ':scope') index = 1;
  while (index < parts.length) {
    if (parts[index] === '>') { direct = true; index++; continue; }
    const compound = parseCompound(parts[index++]);
    const found = [];
    for (const context of current) {
      const pool = direct ? context.children : descendants(context);
      for (const element of pool) if (matches(element, compound) && !found.includes(element)) found.push(element);
    }
    current = found;
    direct = false;
  }
  return current;
}

function createDocument(html) {
  const document = new Document();
  document.append(...parseHtml(html));
  document.body = document.querySelector('body');
  return document;
}

// 只模拟“用户选中了某个元素的内容”这一种状态：containsNode 对它的后代与祖先都返回 true。
function createSelection() {
  let root = null;
  const api = {
    select(element) { root = element; },
    clear() { root = null; },
    get root() { return root; },
    getSelection: () => ({
      get rangeCount() { return root ? 1 : 0; },
      get isCollapsed() { return !root; },
      get anchorNode() { return root; },
      containsNode(node) { return !!root && (node === root || root.contains(node) || (typeof node.contains === 'function' && node.contains(root))); },
      removeAllRanges() { root = null; }
    })
  };
  return api;
}

module.exports = { createDocument, createSelection, Element, Document, TextNode };
