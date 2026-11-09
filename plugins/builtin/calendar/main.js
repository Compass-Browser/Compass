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
