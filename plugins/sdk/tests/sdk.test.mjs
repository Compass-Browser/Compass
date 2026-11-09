// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Unit tests for the live folder SDK and runtime rules.
//   node --test plugins/sdk/tests/

import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import test from 'node:test';

const require = createRequire(import.meta.url);
const text = require('../text.js');
const xml = require('../xml.js');
const html = require('../html.js');
const ics = require('../ics.js');
const manifest = require('../../runtime/manifest.js');
const {fixtureFetcher} = await import('../../../tools/live_folders/runtime.mjs');

const iso = (ms) => new Date(ms).toISOString();

test('decodeEntities', () => {
  assert.equal(text.decodeEntities('a &amp; b &lt;&gt; &#233;&#xE9; &nbsp;&bogus; &#0;'),
               'a & b <> éé  &bogus; �');
  assert.equal(text.decodeEntities(null), '');
});

test('stripTags', () => {
  assert.equal(text.stripTags('<p>Hello <b>you</b><br>there<script>x()</script> &amp; bye</p>'),
               'Hello you there & bye');
});

test('parseDate', () => {
  assert.equal(iso(text.parseDate('2026-11-09T10:00:00+01:00')), '2026-11-09T09:00:00.000Z');
  assert.equal(iso(text.parseDate('2026-11-09T10:00:00.5Z')), '2026-11-09T10:00:00.500Z');
  assert.equal(iso(text.parseDate('2026-11-09')), '2026-11-09T00:00:00.000Z');
  assert.equal(iso(text.parseDate('Mon, 09 Nov 2026 10:00:00 EST')), '2026-11-09T15:00:00.000Z');
  assert.equal(iso(text.parseDate('9 Nov 26 10:00 +0130')), '2026-11-09T08:30:00.000Z');
  assert.equal(iso(text.parseDate('Monday, 09 November 2026 10:00:00 GMT')), '2026-11-09T10:00:00.000Z');
  assert.equal(text.parseDate('yesterday'), null);
  assert.equal(text.parseDate('Mon, 09 Nov 2026 10:00:00 XYZT'), null);
});

test('parseAttributes', () => {
  assert.deepEqual(text.parseAttributes(' HREF="/a?x=1&amp;y=2" data-x=\'q\' hidden v=3', true),
                   {href: '/a?x=1&y=2', 'data-x': 'q', hidden: '', v: '3'});
});

test('xml: structure, prefixes, CDATA and comments', () => {
  const doc = xml.parse('﻿<?xml version="1.0"?><!DOCTYPE x [<!ENTITY e "v">]>' +
      '<root xmlns:dc="d"><!-- <item>no</item> --><item a="1 &gt; 0"><dc:date>D</dc:date>' +
      '<atom:link href="self"/><link>L</link><t><![CDATA[<b>&amp;</b>]]></t></item></root>');
  const items = doc.findAll('item');
  assert.equal(items.length, 1);
  assert.equal(items[0].attr('a'), '1 > 0');
  assert.equal(items[0].childText('date'), 'D');
  assert.equal(items[0].childText('link'), 'L', 'exact name wins over atom:link');
  assert.equal(items[0].child('atom:link').attr('href'), 'self');
  assert.equal(items[0].childText('t'), '<b>&amp;</b>');
  assert.equal(items[0].elements('link').length, 2);
});

test('xml: malformed input does not throw', () => {
  const doc = xml.parse('<a><b>one</c><b>two & three < four');
  assert.equal(doc.child('a').elements('b').length, 1);
  assert.match(doc.text(), /one/);
  assert.equal(xml.parse('').children.length, 0);
});

test('html.links', () => {
  const page = '<base href="https://x.com/dir/"><a href="p1">One</a>' +
      '<a href="p1#frag">Dup</a><a href="#top">Top</a><a href="mailto:a@b">Mail</a>' +
      '<a href="//cdn.x.com/i"><img alt="Pic"></a><!-- <a href="p9">c</a> -->';
  assert.deepEqual(html.links(page, 'https://x.com/start'), [
    {url: 'https://x.com/dir/p1', text: 'One', title: ''},
    {url: 'https://cdn.x.com/i', text: 'Pic', title: ''},
  ]);
  assert.equal(html.title('<svg><title>icon</title></svg><title> A &amp; B </title>'), 'A & B');
});

test('ics: zones', () => {
  assert.equal(ics.resolveZone('W. Europe Standard Time'), 'Europe/Berlin');
  assert.equal(ics.resolveZone('/mozilla.org/20050126_1/Europe/Rome'), 'Europe/Rome');
  assert.equal(ics.resolveZone('Not/AZone'), null);
  const w = (h, mi) => ({y: 2026, mo: 3, d: 29, h, mi, s: 0});
  // Europe/Rome springs forward at 02:00 on 2026-03-29.
  assert.equal(iso(ics.wallToUtc(w(1, 30), 'Europe/Rome')), '2026-03-29T00:30:00.000Z');
  assert.equal(iso(ics.wallToUtc(w(3, 30), 'Europe/Rome')), '2026-03-29T01:30:00.000Z');
  assert.equal(iso(ics.wallToUtc(w(2, 30), 'Europe/Rome')), '2026-03-29T01:30:00.000Z', 'gap moves forward');
});

function expand(body, from, to, timeZone = 'UTC') {
  const cal = ics.parse('BEGIN:VCALENDAR\n' + body.trim().split('\n').map((l) => l.trim()).join('\n') + '\nEND:VCALENDAR');
  return ics.occurrences(cal, {from: Date.parse(from), to: Date.parse(to), timeZone}).map((o) => iso(o.start));
}

test('ics: monthly last Friday, BYSETPOS and UNTIL', () => {
  assert.deepEqual(expand(`
    BEGIN:VEVENT
    UID:m
    DTSTART:20260130T090000Z
    RRULE:FREQ=MONTHLY;BYDAY=-1FR;UNTIL=20260501T000000Z
    END:VEVENT`, '2026-01-01T00:00:00Z', '2026-12-31T00:00:00Z'),
  ['2026-01-30T09:00:00.000Z', '2026-02-27T09:00:00.000Z', '2026-03-27T09:00:00.000Z', '2026-04-24T09:00:00.000Z']);
  assert.deepEqual(expand(`
    BEGIN:VEVENT
    UID:w
    DTSTART:20260130T090000Z
    RRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1;COUNT=3
    END:VEVENT`, '2026-01-01T00:00:00Z', '2026-12-31T00:00:00Z'),
  ['2026-01-30T09:00:00.000Z', '2026-02-27T09:00:00.000Z', '2026-03-31T09:00:00.000Z']);
});

test('ics: monthly on the 31st skips short months; yearly; biweekly', () => {
  assert.deepEqual(expand(`
    BEGIN:VEVENT
    UID:a
    DTSTART:20260131T120000Z
    RRULE:FREQ=MONTHLY;COUNT=3
    END:VEVENT`, '2026-01-01T00:00:00Z', '2026-12-31T00:00:00Z'),
  ['2026-01-31T12:00:00.000Z', '2026-03-31T12:00:00.000Z', '2026-05-31T12:00:00.000Z']);
  assert.deepEqual(expand(`
    BEGIN:VEVENT
    UID:b
    DTSTART;VALUE=DATE:20200229
    RRULE:FREQ=YEARLY
    END:VEVENT`, '2023-01-01T00:00:00Z', '2029-01-01T00:00:00Z'),
  ['2024-02-29T00:00:00.000Z', '2028-02-29T00:00:00.000Z']);
  assert.deepEqual(expand(`
    BEGIN:VEVENT
    UID:c
    DTSTART;TZID=America/New_York:20261026T090000
    RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO
    END:VEVENT`, '2026-10-20T00:00:00Z', '2026-11-30T00:00:00Z'),
  // New York leaves DST on 2026-11-01: 09:00 local stays 09:00.
  ['2026-10-26T13:00:00.000Z', '2026-11-09T14:00:00.000Z', '2026-11-23T14:00:00.000Z']);
});

test('ics: a rule that never matches ends', () => {
  assert.deepEqual(expand(`
    BEGIN:VEVENT
    UID:n
    DTSTART:20260101T000000Z
    RRULE:FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30
    END:VEVENT`, '2026-01-01T00:00:00Z', '2027-01-01T00:00:00Z'), ['2026-01-01T00:00:00.000Z']);
});

test('ics: folded lines and escapes', () => {
  const cal = ics.parse('BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:x\r\nDTSTART:20261109T100000Z\r\n' +
      'SUMMARY:One\\, two\\; three\\nfour\r\nDESCRIPTION:long\r\n  line\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n');
  assert.equal(cal.events[0].title, 'One, two; three\nfour');
  assert.equal(cal.events[0].description, 'long line');
});

test('manifest: errors are listed together', () => {
  assert.throws(() => manifest.validateManifest({
    id: 'Bad', name: '', version: 'x', api: 2,
    hosts: ['ok.com', 'https://no.com', '{config:missing}'], cookies: ['other.com'],
    config: [{key: 'k', type: 'color', label: 'K'}],
  }), (e) => e.code === 'bad_manifest' && e.errors.length === 8);
});

test('manifest: placeholders, config and host matching', () => {
  const m = manifest.validateManifest({
    id: 'a.b', name: 'N', version: '1.0', api: 1, hosts: ['{config:url}', '*.cdn.com'],
    config: [{key: 'url', type: 'url', label: 'URL'}, {key: 'n', type: 'number', label: 'N', default: 5, max: 9},
             {key: 'on', type: 'bool', label: 'On'}],
    refresh: {default: 60, min: 10},
  });
  assert.deepEqual(m.refresh, {default: 300, min: 300}, 'floor applies');
  const cfg = manifest.validateConfig(m, {url: 'https://Feeds.Example.com/x', n: '7', extra: 1});
  assert.deepEqual(cfg, {url: 'https://Feeds.Example.com/x', n: 7, on: false});
  assert.throws(() => manifest.validateConfig(m, {url: 'ftp://x', n: 99}), (e) => e.code === 'bad_config' && e.errors.length === 2);
  const {hosts} = manifest.resolveHosts(m, cfg, {});
  assert.deepEqual(hosts, ['feeds.example.com', '*.cdn.com']);
  assert.ok(manifest.urlAllowed('http://feeds.example.com:8080/a', hosts));
  assert.ok(manifest.urlAllowed('https://cdn.com/', hosts));
  assert.ok(!manifest.urlAllowed('https://evilcdn.com/', hosts));
  assert.ok(!manifest.urlAllowed('https://feeds.example.com.evil.com/', hosts));
  assert.ok(!manifest.urlAllowed('file:///etc/passwd', hosts));
});

test('fixture fetcher rejects listed headers', async () => {
  const f = fixtureFetcher({'https://a.com/': {body: '', rejectHeaders: ['cookie']}}, '.');
  await assert.rejects(f({method: 'GET', url: 'https://a.com/', headers: {Cookie: 'x'}}));
  await f({method: 'GET', url: 'https://a.com/', headers: {}});
});
