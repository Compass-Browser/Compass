// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Small DOM builders. Text from boards and the web goes in as textContent,
// never as markup.

export interface ElementOptions {
  className?: string;
  text?: string;
  attrs?: Record<string, string>;
}

export function el<K extends keyof HTMLElementTagNameMap>(
    tag: K, options: ElementOptions = {},
    children: Node[] = []): HTMLElementTagNameMap[K] {
  const element = document.createElement(tag);
  if (options.className) {
    element.className = options.className;
  }
  if (options.text !== undefined) {
    element.textContent = options.text;
  }
  for (const [name, value] of Object.entries(options.attrs || {})) {
    element.setAttribute(name, value);
  }
  for (const child of children) {
    element.appendChild(child);
  }
  return element;
}

const SVG_NS = 'http://www.w3.org/2000/svg';

export function svg<K extends keyof SVGElementTagNameMap>(
    tag: K, attrs: Record<string, string|number> = {},
    children: Node[] = []): SVGElementTagNameMap[K] {
  const element = document.createElementNS(SVG_NS, tag);
  for (const [name, value] of Object.entries(attrs)) {
    element.setAttribute(name, `${value}`);
  }
  for (const child of children) {
    element.appendChild(child);
  }
  return element;
}

export function button(
    label: string, className: string, onClick: (e: MouseEvent) => void,
    content?: Node): HTMLButtonElement {
  const b = el('button', {
    className,
    attrs: {'type': 'button', 'title': label, 'aria-label': label},
  });
  if (content) {
    b.appendChild(content);
  } else {
    b.textContent = label;
  }
  b.addEventListener('click', onClick);
  return b;
}

// localStorage is a convenience: it can be missing or throw.
export function readStorage(key: string): string|null {
  try {
    return window.localStorage.getItem(key);
  } catch {
    return null;
  }
}

export function writeStorage(key: string, value: string) {
  try {
    window.localStorage.setItem(key, value);
  } catch {
    // Not kept; nothing depends on it.
  }
}

// A message at the bottom for a moment ("The image is too large.").
export function toast(message: string) {
  const host = document.querySelector<HTMLElement>('.toast') ||
      document.body.appendChild(
          el('div', {className: 'toast', attrs: {'role': 'status'}}));
  host.textContent = message;
  host.classList.add('shown');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => host.classList.remove('shown'), 3500);
}

let toastTimer = 0;
