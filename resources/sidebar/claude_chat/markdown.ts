// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Claude's Markdown as DOM nodes, built with createElement and text nodes
// only (never innerHTML), so nothing in a reply runs or styles the page:
// paragraphs, headings, lists, quotes, fenced code, tables, and inline code,
// bold, italic and links.

function el<K extends keyof HTMLElementTagNameMap>(
    tag: K, ...children: Array<Node|string>): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  node.append(...children);
  return node;
}

const INLINE =
    /(`+)([\s\S]*?)\1|\*\*([^*]+)\*\*|__([^_]+)__|\*([^*\s][^*]*)\*|\[([^\]]+)\]\((https?:\/\/[^\s)]+)\)|(https?:\/\/[^\s<>()]+[^\s<>().,;:!?])/g;

// Inline Markdown of one block's text.
export function renderInline(text: string): Node[] {
  const nodes: Node[] = [];
  let last = 0;
  for (const match of text.matchAll(INLINE)) {
    const at = match.index;
    if (at > last) {
      nodes.push(document.createTextNode(text.slice(last, at)));
    }
    if (match[1] !== undefined) {
      nodes.push(el('code', match[2]!));
    } else if (match[3] !== undefined || match[4] !== undefined) {
      nodes.push(el('strong', ...renderInline(match[3] ?? match[4]!)));
    } else if (match[5] !== undefined) {
      nodes.push(el('em', ...renderInline(match[5])));
    } else {
      const label = match[6] ?? match[8]!;
      const href = match[7] ?? match[8]!;
      const link = el('a', label);
      link.href = href;
      link.title = href;
      nodes.push(link);
    }
    last = at + match[0].length;
  }
  if (last < text.length) {
    nodes.push(document.createTextNode(text.slice(last)));
  }
  return nodes;
}

function isTableRow(line: string): boolean {
  return /^\s*\|.*\|\s*$/.test(line);
}

function cells(line: string): string[] {
  return line.trim().replace(/^\||\|$/g, '').split('|').map(c => c.trim());
}

// A whole reply.
export function renderMarkdown(source: string): DocumentFragment {
  const fragment = document.createDocumentFragment();
  const lines = source.replace(/\r\n/g, '\n').split('\n');
  let i = 0;
  const paragraph: string[] = [];
  const flush = () => {
    if (paragraph.length) {
      fragment.append(el('p', ...renderInline(paragraph.join('\n'))));
      paragraph.length = 0;
    }
  };

  while (i < lines.length) {
    const line = lines[i]!;

    const fence = line.match(/^\s*(```+|~~~+)\s*([\w+#.-]*)\s*$/);
    if (fence) {
      flush();
      const code: string[] = [];
      i++;
      while (i < lines.length && !lines[i]!.trim().startsWith(fence[1]!)) {
        code.push(lines[i]!);
        i++;
      }
      i++;
      const pre = el('pre', el('code', code.join('\n')));
      if (fence[2]) {
        pre.dataset['lang'] = fence[2];
      }
      fragment.append(pre);
      continue;
    }

    const heading = line.match(/^(#{1,6})\s+(.*)$/);
    if (heading) {
      flush();
      const level = Math.min(heading[1]!.length + 2, 6);
      fragment.append(
          el(`h${level}` as 'h3', ...renderInline(heading[2]!)));
      i++;
      continue;
    }

    if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)) {
      flush();
      fragment.append(el('hr'));
      i++;
      continue;
    }

    if (/^\s*>/.test(line)) {
      flush();
      const quoted: string[] = [];
      while (i < lines.length && /^\s*>/.test(lines[i]!)) {
        quoted.push(lines[i]!.replace(/^\s*>\s?/, ''));
        i++;
      }
      fragment.append(el('blockquote', renderMarkdown(quoted.join('\n'))));
      continue;
    }

    const item = line.match(/^(\s*)([-*+]|\d+[.)])\s+(.*)$/);
    if (item) {
      flush();
      const ordered = /\d/.test(item[2]!);
      const list = el(ordered ? 'ol' : 'ul');
      while (i < lines.length) {
        const m = lines[i]!.match(/^(\s*)([-*+]|\d+[.)])\s+(.*)$/);
        if (!m || /\d/.test(m[2]!) !== ordered) {
          break;
        }
        const text = [m[3]!];
        i++;
        // Continuation lines, indented.
        while (i < lines.length && /^\s{2,}\S/.test(lines[i]!) &&
               !/^\s*([-*+]|\d+[.)])\s+/.test(lines[i]!)) {
          text.push(lines[i]!.trim());
          i++;
        }
        const li = el('li', ...renderInline(text.join(' ')));
        if (m[1]!.length >= 2 && list.lastElementChild) {
          // Nested: under the previous item.
          let nested = list.lastElementChild.querySelector(
              ordered ? 'ol' : 'ul');
          if (!nested) {
            nested = el(ordered ? 'ol' : 'ul');
            list.lastElementChild.append(nested);
          }
          nested.append(li);
        } else {
          list.append(li);
        }
      }
      fragment.append(list);
      continue;
    }

    if (isTableRow(line) && i + 1 < lines.length &&
        /^\s*\|?\s*:?-{2,}/.test(lines[i + 1]!)) {
      flush();
      const table = el('table');
      const head = el('tr', ...cells(line).map(c => el('th', ...renderInline(c))));
      table.append(el('thead', head));
      const body = el('tbody');
      i += 2;
      while (i < lines.length && isTableRow(lines[i]!)) {
        body.append(
            el('tr', ...cells(lines[i]!).map(c => el('td', ...renderInline(c)))));
        i++;
      }
      table.append(body);
      fragment.append(el('div', table));
      (fragment.lastChild as HTMLElement).className = 'table';
      continue;
    }

    if (!line.trim()) {
      flush();
    } else {
      paragraph.push(line);
    }
    i++;
  }
  flush();
  return fragment;
}
