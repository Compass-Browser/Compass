// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// reader_test.html's checks of reader.js (docs/design/reader-mode.md).

(async () => {
  const R = __compassReader, failures = [];
  const check = (name, cond, detail) => {
    if (!cond) failures.push(name + ': ' + JSON.stringify(detail));
  };
  const kUrl = 'https://coastal.example/2026/lighthouse.html';
  const load = async (name) => {
    const text = await (await fetch('fixtures/' + name)).text();
    return new DOMParser().parseFromString(text, 'text/html');
  };

  try {
    // An article.
    const article = await load('article.html');
    check('article is readerable',
          R.check({document: article}).readerable === true);
    const a = R.parse({document: article, url: kUrl});
    check('article parses', a.ok, a.error);
    check('title', a.title === 'The Lighthouse Keepers of Capo Rosso', a.title);
    check('site name', a.siteName === 'Coastal Notes', a.siteName);
    check('byline', a.byline === 'Giulia Marino', a.byline);
    check('lang', a.lang === 'en', a.lang);
    check('dir', a.dir === 'ltr', a.dir);
    check('url', a.url === kUrl, a.url);
    check('text', a.content.includes('Rosa Bellini'), a.content);
    check('length', a.length > 1000, a.length);
    for (const bad of ['<script', 'onclick', 'onerror', 'onmouseover',
                       '<iframe', '<form', '<input', '<button', 'javascript:',
                       'style=', 'class=', 'Subscribe', 'Sponsored']) {
      check('article has no ' + bad, !a.content.includes(bad), a.content);
    }
    check('image made absolute', a.content.includes(
        'src="https://coastal.example/2026/images/lighthouse.jpg"'), a.content);
    check('image keeps alt', a.content.includes('alt="The lighthouse at dusk"'),
          a.content);
    check('link made absolute', a.content.includes(
        'href="https://coastal.example/archive/lighthouses"'), a.content);
    check('links have noopener', a.content.includes('rel="noopener noreferrer"'),
          a.content);
    check('the page itself is untouched',
          article.querySelectorAll('script').length === 2 &&
          article.querySelector('form') !== null);

    // Not an article.
    const form = await load('not_article.html');
    check('sign-in page is not readerable',
          R.check({document: form}).readerable === false);

    // The sanitizer.
    const base = 'https://b.example/dir/page.html';
    const s = (html) => R.sanitize(html, base);
    const cases = [
      ['<p>a<script>alert(1)</script>b</p>', '<p>ab</p>'],
      ['<p onclick="x()" ONMOUSEOVER="y()">t</p>', '<p>t</p>'],
      ['<img src="x.png" onerror="y()">',
       '<img src="https://b.example/dir/x.png">'],
      ['<img src="javascript:alert(1)">', ''],
      ['<img src="data:image/png;base64,AAAA" alt="d">',
       '<img src="data:image/png;base64,AAAA" alt="d">'],
      ['<img src="data:text/html;base64,AAAA">', ''],
      ['<img srcset="a.jpg 1x, /b.jpg 2x">',
       '<img srcset="https://b.example/dir/a.jpg 1x, https://b.example/b.jpg 2x">'],
      ['<a href="javascript:alert(1)">j</a>', '<a>j</a>'],
      ['<a href=" jav&#x09;ascript:alert(1)">j</a>', '<a>j</a>'],
      ['<a href="data:text/html,x">d</a>', '<a>d</a>'],
      ['<a href="mailto:a@b.example">m</a>',
       '<a href="mailto:a@b.example" rel="noopener noreferrer">m</a>'],
      ['<a href="/x" target="_top">l</a>',
       '<a href="https://b.example/x" rel="noopener noreferrer">l</a>'],
      ['<iframe src="https://e.example"></iframe><p>k</p>', '<p>k</p>'],
      ['<form><p>gone</p><input></form>', ''],
      ['<svg><script>alert(1)</script><a href="https://e.example">s</a></svg>', ''],
      ['<math><mi>x</mi></math>', ''],
      ['<object data="x.swf"></object><embed src="x.swf">', ''],
      ['<style>p{}</style><link rel="stylesheet" href="x.css">', ''],
      ['<custom-el style="color:red">kept</custom-el>', 'kept'],
      ['<font color="red">kept</font>', 'kept'],
      ['<p>a<!-- comment -->b</p>', '<p>ab</p>'],
      ['<p class="x" id="y" style="color:red" data-z="1" lang="it">t</p>',
       '<p lang="it">t</p>'],
      ['<p dir="rtl">r</p><p dir="x">x</p>', '<p dir="rtl">r</p><p>x</p>'],
      ['<table><tr><td colspan="2" onclick="x()">c</td></tr></table>',
       '<table><tbody><tr><td colspan="2">c</td></tr></tbody></table>'],
      ['<noscript><img src="x.png"></noscript>', ''],
      ['<template><p>t</p></template>', ''],
      ['<video src="v.mp4" controls></video>', ''],
      ['<blockquote cite="javascript:x">q</blockquote>',
       '<blockquote>q</blockquote>'],
    ];
    for (const [input, expected] of cases) {
      const output = s(input);
      check('sanitize ' + input, output === expected, output);
    }
  } catch (e) {
    failures.push('threw: ' + (e && e.stack || e));
  }

  document.getElementById('log').textContent =
      failures.length ? 'RESULT FAIL\n' + failures.join('\n') : 'RESULT PASS';
  document.title = failures.length ? 'FAIL' : 'PASS';
})();
