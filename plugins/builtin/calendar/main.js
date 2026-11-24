// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Live folder: upcoming events from a private iCal (ICS) address, each
// opening its meeting link (Meet, Zoom, Teams, Webex, ...). See
// docs/design/live-folders.md.

const SERVICES = [
  {name: 'Google Meet', re: /https:\/\/meet\.google\.com\/[a-z]{3}-[a-z]{4}-[a-z]{3}(\?[^\s"'<>]*)?/i},
  {name: 'Zoom', re: /https:\/\/(?:[\w-]+\.)?zoom\.us\/(?:j|my|w|s)\/[^\s"'<>]+/i},
  {name: 'Teams', re: /https:\/\/teams\.(?:microsoft|live)\.com\/(?:l\/meetup-join|meet)\/[^\s"'<>]+/i},
  {name: 'Webex', re: /https:\/\/[\w-]+\.webex\.com\/[^\s"'<>]+/i},
  {name: 'FaceTime', re: /https:\/\/facetime\.apple\.com\/join[^\s"'<>]+/i},
  {name: 'Whereby', re: /https:\/\/whereby\.com\/[^\s"'<>]+/i},
  {name: 'Jitsi', re: /https:\/\/meet\.jit\.si\/[^\s"'<>]+/i},
];

// Where calendars put the link, most specific first.
const LINK_PROPS = ['X-GOOGLE-CONFERENCE', 'X-MICROSOFT-SKYPETEAMSMEETINGURL',
                    'X-MICROSOFT-ONLINEMEETINGCONFLINK', 'URL', 'LOCATION', 'DESCRIPTION'];

function meeting(ev) {
  for (const prop of LINK_PROPS) {
    const value = ev.props[prop];
    if (!value) continue;
    for (const s of SERVICES) {
      const m = s.re.exec(compass.text.decodeEntities(value));
      if (m) return {service: s.name, url: m[0].replace(/[).,;>\]]+$/, '')};
    }
  }
  return null;
}

function httpUrl(s) {
  return /^https?:\/\/\S+$/i.test((s || '').trim()) ? s.trim() : null;
}

function dayKey(ms) {
  return new Intl.DateTimeFormat('en-CA', {timeZone: compass.timeZone}).format(new Date(ms));
}

function when(ev, now) {
  const time = new Intl.DateTimeFormat(compass.locale, {timeZone: compass.timeZone, hour: 'numeric', minute: '2-digit'});
  const today = dayKey(now);
  const tomorrow = dayKey(now + 86400000);
  const day = dayKey(ev.start);
  let prefix = '';
  if (day === tomorrow) prefix = 'Tomorrow ';
  else if (day !== today) {
    prefix = new Intl.DateTimeFormat(compass.locale, {timeZone: compass.timeZone, weekday: 'short'})
        .format(new Date(ev.start)) + ' ';
  }
  if (ev.allDay) return (prefix || 'Today ') + '(all day)';
  return prefix + time.format(new Date(ev.start)) + '–' + time.format(new Date(ev.end));
}

async function fetchItems(config) {
  const address = compass.secret('ics').trim().replace(/^webcal:/i, 'https:');
  const res = await compass.fetch(address, {headers: {accept: 'text/calendar, */*;q=0.8'}});
  if (res.status === 404 || res.status === 401 || res.status === 403) {
    throw new Error('The calendar address no longer works (HTTP ' + res.status + '). Paste it again in the folder\'s settings.');
  }
  if (!res.ok) throw new Error('The calendar answered HTTP ' + res.status);
  const body = await res.text();
  if (body.indexOf('BEGIN:VCALENDAR') < 0) throw new Error('This address doesn\'t return a calendar');

  const now = compass.now();
  const to = now + config.hours * 3600000;
  const events = compass.ics.occurrences(compass.ics.parse(body), {from: now, to, timeZone: compass.timeZone});

  const items = [];
  let nextChange = Infinity;
  for (const ev of events) {
    if (ev.allDay && !config.allDay) continue;
    const link = meeting(ev);
    if (!link && config.onlyMeetings) continue;
    const url = link ? link.url : (httpUrl(ev.url) || config.fallback);
    if (!url) continue;
    const ongoing = ev.start <= now && now < ev.end;
    const detail = link ? link.service : compass.text.collapse(ev.location).slice(0, 60);
    items.push({
      id: (ev.uid || ev.title) + '@' + ev.start,
      title: ev.title || '(No title)',
      url,
      subtitle: when(ev, now) + (detail ? ' · ' + detail : ''),
      time: ev.start,
      badge: ongoing && !ev.allDay ? 'Now' : undefined,
    });
    if (!ev.allDay) {
      if (ev.start > now) nextChange = Math.min(nextChange, ev.start);
      if (ev.end > now) nextChange = Math.min(nextChange, ev.end);
    }
  }
  const result = {items: items.slice(0, 50)};
  // Refresh when the next event starts or ends, so "Now" stays right; the
  // host still applies the manifest's minimum.
  if (nextChange !== Infinity) result.nextRefresh = Math.max(60, Math.ceil((nextChange - now) / 1000));
  return result;
}

// ---- Tab preview: the next events on a Google Calendar tab ---------------
//
// Reads the event chips the page shows: their accessible label, like
// "10:30am to 11:30am, Team sync, Calendar: Work, No location, November 21,
// 2026" (en-US) or "…, 21 November 2026" (en-GB), and their visible title,
// since titles can have commas. It needs no setup. Only English labels are
// read; events the page doesn't show (another week, a month view's "+3
// more") aren't seen.
//
// A row opens the event's meeting (Meet, Zoom, Teams, ...) when it has one,
// else the event's page. The chips don't carry meeting links, so with the
// tab's session (on for built-ins unless turned off in Settings › Plugins)
// the preview fetches each shown event's page: its server-rendered data
// (script#initialdata) holds the event's record, an array whose first
// element is the event's id. The page also carries other events' records
// (today's), so only that record is read. See docs/design/live-folders.md,
// "Tab previews".

const MONTHS = ['january', 'february', 'march', 'april', 'may', 'june', 'july',
                'august', 'september', 'october', 'november', 'december'];
const MONTH = '(January|February|March|April|May|June|July|August|September|October|November|December)';
// "November 21, 2026" or "21 November 2026".
const DATE_US = new RegExp('\\b' + MONTH + ' (\\d{1,2}), (\\d{4})\\b', 'g');
const DATE_GB = new RegExp('\\b(\\d{1,2}) ' + MONTH + ' (\\d{4})\\b', 'g');

// The label's last date, as {y, mo, d}.
function lastDate(label) {
  const us = [...label.matchAll(DATE_US)].map((m) => ({at: m.index, y: +m[3], mo: m[1], d: +m[2]}));
  const gb = [...label.matchAll(DATE_GB)].map((m) => ({at: m.index, y: +m[3], mo: m[2], d: +m[1]}));
  const all = us.concat(gb).sort((a, b) => a.at - b.at);
  if (!all.length) return null;
  const last = all[all.length - 1];
  return {y: last.y, mo: MONTHS.indexOf(last.mo.toLowerCase()) + 1, d: last.d};
}
const CLOCK = /^(\d{1,2})(?::(\d{2}))?\s*(am|pm)?$/i;

function parseClock(s, fallbackMeridiem) {
  const m = CLOCK.exec((s || '').trim());
  if (!m) return null;
  let h = +m[1];
  const mi = m[2] ? +m[2] : 0;
  const meridiem = (m[3] || fallbackMeridiem || '').toLowerCase();
  if (meridiem === 'pm' && h < 12) h += 12;
  if (meridiem === 'am' && h === 12) h = 0;
  if (h > 23 || mi > 59) return null;
  return {h, mi, meridiem: m[3] ? m[3].toLowerCase() : ''};
}

// The chip's title: its visible title, else what its text has after the
// label (all-day chips), else the label's second part.
function chipTitle(label, title, text) {
  if (title && label.includes(', ' + title)) return title;
  const after = text && text.startsWith(label) ? text.slice(label.length).trim() : '';
  if (after && label.includes(', ' + after + ',')) return after;
  return (label.split(',')[1] || '').trim();
}

// {start, end, allDay, title} from a chip's label, or null.
function parseChip(label, givenTitle, text) {
  const day = lastDate(label);
  if (!day) return null;
  const title = chipTitle(label, givenTitle, text);
  // What comes before the title: "All day" or "10am to 6pm".
  const cut = title ? label.indexOf(', ' + title) : label.indexOf(',');
  const head = (cut > 0 ? label.slice(0, cut) : label).trim();
  const wall = (c) => compass.ics.wallToUtc(Object.assign({h: c.h, mi: c.mi, s: 0}, day), compass.timeZone);

  if (/^all day$/i.test(head)) {
    const start = wall({h: 0, mi: 0});
    return {start, end: start + 86400000, allDay: true, title};
  }
  const range = /^(.+?)\s+to\s+(.+)$/i.exec(head);
  if (!range) return null;
  // "10 to 11:30am": the start takes the end's am/pm.
  const endClock = parseClock(range[2]);
  const startClock = endClock && parseClock(range[1], endClock.meridiem);
  if (!startClock || !endClock) return null;
  const start = wall(startClock);
  let end = wall(endClock);
  if (end <= start) end += 86400000;  // Past midnight.
  return {start, end, allDay: false, title};
}

// The event's page, in the tab's account ("/calendar/u/1/"): a chip's
// data-eventid is the event's eid (base64url of "<event id> <calendar>").
function eventUrl(pageUrl, eid) {
  if (!eid || eid.length > 512 || !/^[A-Za-z0-9_-]+$/.test(eid)) return undefined;
  const m = /^(https:\/\/calendar\.google\.com\/calendar\/(?:u\/\d+\/)?)/.exec(pageUrl || '');
  return m ? m[1] + 'r/eventedit/' + eid : undefined;
}

// The session's request limit: later rows open the event's page.
const MAX_LOOKUPS = 5;

const BASE64URL = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';

// The event id in an eid (base64url of "<event id> <calendar>"), or null.
// Plugins have no atob().
function eventIdOf(eid) {
  let bits = 0;
  let value = 0;
  let out = '';
  for (const ch of eid) {
    const v = BASE64URL.indexOf(ch);
    if (v < 0) return null;
    value = (value << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += String.fromCharCode((value >> bits) & 0xff);
      value &= (1 << bits) - 1;
    }
  }
  return out.split(' ')[0] || null;
}

function initialData(html) {
  const m = /<script\b[^>]*\bid="initialdata"[^>]*>([\s\S]*?)<\/script>/i.exec(html);
  if (!m) return null;
  try {
    return JSON.parse(m[1]);
  } catch (e) {
    return null;
  }
}

// The array in |data| whose first element is |id|.
function findRecord(data, id) {
  const stack = [data];
  while (stack.length) {
    const v = stack.pop();
    if (!Array.isArray(v)) continue;
    if (v[0] === id && v.length > 1) return v;
    for (const x of v) if (Array.isArray(x)) stack.push(x);
  }
  return null;
}

// The record's meeting, {service, url}: Meet first, as SERVICES orders them.
function meetingIn(record) {
  const strings = [];
  const stack = [record];
  while (stack.length) {
    const v = stack.pop();
    if (typeof v === 'string') strings.push(v);
    else if (Array.isArray(v)) stack.push(...v);
  }
  for (const s of SERVICES) {
    for (const str of strings) {
      const m = s.re.exec(str);
      if (m) return {service: s.name, url: m[0].replace(/[).,;>\]]+$/, '')};
    }
  }
  return null;
}

// Gives |events| (those with an event page) their meetings, from their
// pages. A failed lookup leaves the event's page.
async function addMeetings(events) {
  const todo = events.filter((ev) => ev.url && ev.eid).slice(0, MAX_LOOKUPS);
  await Promise.all(todo.map(async (ev) => {
    try {
      const res = await compass.fetch(ev.url);
      if (!res.ok) return;
      const id = eventIdOf(ev.eid);
      const record = id && findRecord(initialData(await res.text()), id);
      const link = record && meetingIn(record);
      if (link) ev.meeting = link;
    } catch (e) {
      // Over the limits, signed out: the event's page.
    }
  }));
}

async function previewTab(page, config) {
  const now = compass.now();
  const seen = new Set();
  const events = [];
  for (const chip of page.groups.events || []) {
    const id = chip.attrs && chip.attrs['data-eventid'];
    const fields = chip.fields || {};
    const text = compass.text.collapse(chip.text);
    const label = compass.text.collapse(fields.label || chip.text);
    const ev = label && parseChip(label, compass.text.collapse(fields.title), text);
    if (!ev || !ev.title || ev.end <= now || (ev.allDay && !config.allDay)) continue;
    // A chip shows once per day it spans, and twice in some views.
    const key = (id || ev.title) + '@' + ev.start;
    if (seen.has(key)) continue;
    seen.add(key);
    events.push(Object.assign({key, eid: id, url: eventUrl(page.url, id)}, ev));
  }
  events.sort((a, b) => a.start - b.start || a.title.localeCompare(b.title));

  const shown = events.slice(0, config.count);
  if (compass.hasSession) await addMeetings(shown);
  const items = shown.map((ev) => ({
    id: ev.key,
    title: ev.title,
    url: ev.meeting ? ev.meeting.url : ev.url,
    subtitle: when(ev, now) + (ev.meeting ? ' · ' + ev.meeting.service : ''),
    time: ev.start,
    badge: ev.start <= now && !ev.allDay ? 'Now' : undefined,
  }));
  if (!items.length) {
    return {items, footer: page.live ? 'Nothing coming up in this view' : undefined};
  }
  return {items, title: 'Coming up'};
}
