#!/usr/bin/env node
// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Live folder plugin harness. See docs/design/live-folders.md, "Testing".
//
//   run.mjs test [--update] [filter]   Fixture tests: plugins/builtin/*/tests/*
//                                      and plugins/conformance/*
//   run.mjs run <plugin-dir> [--config key=value]... [--secret key=value]...
//               [--locale en-GB] [--tz Europe/Rome]
//                                      One refresh against the real network
//   run.mjs check <plugin-dir>         Validate a manifest

import {existsSync, readdirSync, readFileSync, writeFileSync} from 'node:fs';
import {isDeepStrictEqual} from 'node:util';
import path from 'node:path';
import {ROOT, fixtureFetcher, loadPlugin, runPlugin} from './runtime.mjs';

function usage() {
  console.error('usage: run.mjs test [--update] [filter] | run <plugin-dir> [options] | check <plugin-dir>');
  process.exit(2);
}

function findCases() {
  const cases = [];
  const builtin = path.join(ROOT, 'plugins/builtin');
  for (const name of readdirSync(builtin).sort()) {
    const tests = path.join(builtin, name, 'tests');
    if (!existsSync(tests)) continue;
    for (const c of readdirSync(tests).sort()) {
      const dir = path.join(tests, c);
      if (existsSync(path.join(dir, 'case.json'))) cases.push({name: `${name}/${c}`, dir, pluginDir: path.join(builtin, name)});
    }
  }
  const conformance = path.join(ROOT, 'plugins/conformance');
  if (existsSync(conformance)) {
    for (const c of readdirSync(conformance).sort()) {
      const dir = path.join(conformance, c);
      if (existsSync(path.join(dir, 'case.json'))) cases.push({name: `conformance/${c}`, dir, pluginDir: path.join(dir, 'plugin')});
    }
  }
  return cases;
}

// What a test compares: the outcome without log lines or error wording
// (error codes are the contract; messages may differ between runtimes).
function outcome(run) {
  const base = {fetches: run.fetches};
  return run.ok ? {ok: true, result: run.result, ...base} : {ok: false, error: {code: run.error.code}, ...base};
}

async function test(args) {
  process.env.TZ = 'UTC';
  const update = args.includes('--update');
  const filter = args.find((a) => !a.startsWith('--'));
  let failed = 0;
  let count = 0;
  for (const c of findCases()) {
    if (filter && !c.name.includes(filter)) continue;
    count++;
    const spec = JSON.parse(readFileSync(path.join(c.dir, 'case.json'), 'utf8'));
    let actual;
    try {
      const plugin = loadPlugin(c.pluginDir);
      actual = outcome(await runPlugin(plugin, {
        config: spec.config ?? {}, secrets: spec.secrets ?? {}, grantCookies: !!spec.grantCookies,
        env: spec.env ?? {}, limits: spec.limits ?? {},
        fetcher: fixtureFetcher(spec.responses ?? {}, c.dir),
      }));
    } catch (e) {
      actual = {ok: false, error: {code: e.code ?? 'harness'}, fetches: [], message: e.message};
    }
    const expectedPath = path.join(c.dir, 'expected.json');
    if (update) {
      writeFileSync(expectedPath, JSON.stringify(actual, null, 2) + '\n');
      console.log(`updated ${c.name}`);
      continue;
    }
    const expected = existsSync(expectedPath) ? JSON.parse(readFileSync(expectedPath, 'utf8')) : null;
    if (isDeepStrictEqual(actual, expected)) {
      console.log(`ok    ${c.name}`);
    } else {
      failed++;
      console.log(`FAIL  ${c.name}`);
      console.log('  expected:', JSON.stringify(expected));
      console.log('  actual:  ', JSON.stringify(actual));
    }
  }
  console.log(`\n${count - failed}/${count} passed`);
  process.exit(failed ? 1 : 0);
}

function pairs(args, flag) {
  const out = {};
  args.forEach((a, i) => {
    if (a !== flag) return;
    const v = args[i + 1] ?? '';
    const eq = v.indexOf('=');
    if (eq <= 0) usage();
    out[v.slice(0, eq)] = v.slice(eq + 1);
  });
  return out;
}

function option(args, flag) {
  const i = args.indexOf(flag);
  return i >= 0 ? args[i + 1] : undefined;
}

async function run(args) {
  const dir = args[0];
  if (!dir) usage();
  const plugin = loadPlugin(path.resolve(dir));
  const r = await runPlugin(plugin, {
    config: pairs(args, '--config'), secrets: pairs(args, '--secret'),
    env: {locale: option(args, '--locale'), timeZone: option(args, '--tz')},
  });
  for (const line of r.log) console.error(`[log] ${line}`);
  for (const f of r.fetches) console.error(`[fetch] ${f.method} ${f.url}${f.cookies ? ' (cookies)' : ''}`);
  console.log(JSON.stringify(r.ok ? r.result : {error: r.error}, null, 2));
  process.exit(r.ok ? 0 : 1);
}

function check(args) {
  if (!args[0]) usage();
  try {
    const plugin = loadPlugin(path.resolve(args[0]));
    console.log(JSON.stringify(plugin.manifest, null, 2));
  } catch (e) {
    console.error(e.errors ? e.errors.map((x) => `- ${x}`).join('\n') : e.message);
    process.exit(1);
  }
}

const [command, ...rest] = process.argv.slice(2);
if (command === 'test') await test(rest);
else if (command === 'run') await run(rest);
else if (command === 'check') check(rest);
else usage();
