// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// iCalendar (RFC 5545) events for live folders. Plugins reach it as
// `compass.ics`:
//
//   const cal = compass.ics.parse(text);
//   const list = compass.ics.occurrences(cal, {from, to, timeZone});
//   // → [{uid, title, start, end, allDay, location, description, url,
//   //     status, props}], sorted by start (epoch ms)
//
// Covers what calendar exports actually use: TZID times (through Intl, so
// no VTIMEZONE parsing), Windows zone names, floating and all-day dates,
// RRULE (DAILY, WEEKLY, MONTHLY, YEARLY with INTERVAL, COUNT, UNTIL, BYDAY,
// BYMONTHDAY, BYMONTH, BYSETPOS), RDATE, EXDATE and RECURRENCE-ID overrides.

(function (root) {
  'use strict';

  const DAY = 86400000;
  const WEEKDAYS = {SU: 0, MO: 1, TU: 2, WE: 3, TH: 4, FR: 5, SA: 6};

  // Outlook and Exchange write Windows zone names.
  const WINDOWS_ZONES = {
    'UTC': 'UTC',
    'GMT Standard Time': 'Europe/London',
    'Greenwich Standard Time': 'Atlantic/Reykjavik',
    'W. Europe Standard Time': 'Europe/Berlin',
    'Central Europe Standard Time': 'Europe/Budapest',
    'Central European Standard Time': 'Europe/Warsaw',
    'Romance Standard Time': 'Europe/Paris',
    'E. Europe Standard Time': 'Europe/Chisinau',
    'FLE Standard Time': 'Europe/Kiev',
    'GTB Standard Time': 'Europe/Bucharest',
    'Russian Standard Time': 'Europe/Moscow',
    'Eastern Standard Time': 'America/New_York',
    'Central Standard Time': 'America/Chicago',
    'Mountain Standard Time': 'America/Denver',
    'US Mountain Standard Time': 'America/Phoenix',
    'Pacific Standard Time': 'America/Los_Angeles',
    'Atlantic Standard Time': 'America/Halifax',
    'E. South America Standard Time': 'America/Sao_Paulo',
    'India Standard Time': 'Asia/Kolkata',
    'China Standard Time': 'Asia/Shanghai',
    'Singapore Standard Time': 'Asia/Singapore',
    'Tokyo Standard Time': 'Asia/Tokyo',
    'Korea Standard Time': 'Asia/Seoul',
    'AUS Eastern Standard Time': 'Australia/Sydney',
    'New Zealand Standard Time': 'Pacific/Auckland',
  };

  // ---- Time zones -------------------------------------------------------

  const formatters = new Map();

  function formatter(tz) {
    if (formatters.has(tz)) return formatters.get(tz);
    let f = null;
    try {
      f = new Intl.DateTimeFormat('en-US', {
        timeZone: tz, hourCycle: 'h23', year: 'numeric', month: '2-digit',
        day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit',
      });
    } catch (e) {
      f = null;
    }
    formatters.set(tz, f);
    return f;
  }

  function resolveZone(tzid) {
    if (!tzid) return null;
    let z = String(tzid).replace(/^"|"$/g, '').trim();
    if (Object.prototype.hasOwnProperty.call(WINDOWS_ZONES, z)) return WINDOWS_ZONES[z];
    if (formatter(z)) return z;
    // "/mozilla.org/20050126_1/Europe/Rome" and similar prefixes.
    const parts = z.split('/').filter(Boolean);
    for (let k = Math.min(3, parts.length); k >= 1; k--) {
      const tail = parts.slice(parts.length - k).join('/');
      if (formatter(tail)) return tail;
    }
    return null;
  }

  // The zone's offset from UTC at instant `ms`, in milliseconds.
  function zoneOffset(ms, tz) {
    const p = {};
    for (const x of formatter(tz).formatToParts(new Date(ms))) p[x.type] = x.value;
    const asUtc = Date.UTC(+p.year, +p.month - 1, +p.day, +p.hour % 24,
                           +p.minute, +p.second);
    return asUtc - Math.floor(ms / 1000) * 1000;
  }

  // Wall clock time in `tz` ('UTC', an IANA name, or null for the host's
  // local time) to epoch ms. Times skipped by a DST change move forward.
  function wallToUtc(w, tz) {
    if (tz === 'UTC') return Date.UTC(w.y, w.mo - 1, w.d, w.h, w.mi, w.s);
    if (!tz || !formatter(tz)) return new Date(w.y, w.mo - 1, w.d, w.h, w.mi, w.s).getTime();
    const guess = Date.UTC(w.y, w.mo - 1, w.d, w.h, w.mi, w.s);
    const off1 = zoneOffset(guess, tz);
    const t = guess - off1;
    const off2 = zoneOffset(t, tz);
    return off1 === off2 ? t : guess - off2;
  }

  // ---- Civil dates as day numbers (days since 1970-01-01) -------------

  function dayNumber(y, mo, d) { return Math.floor(Date.UTC(y, mo - 1, d) / DAY); }

  function fromDayNumber(n) {
    const dt = new Date(n * DAY);
    return {y: dt.getUTCFullYear(), mo: dt.getUTCMonth() + 1, d: dt.getUTCDate()};
  }

  function weekday(n) { return new Date(n * DAY).getUTCDay(); }

  function daysInMonth(y, mo) { return new Date(Date.UTC(y, mo, 0)).getUTCDate(); }

  // ---- Content lines ----------------------------------------------------

  function splitOutsideQuotes(s, sep) {
    const out = [];
    let cur = '';
    let quote = false;
    for (const c of s) {
      if (c === '"') quote = !quote;
      if (c === sep && !quote) {
        out.push(cur);
        cur = '';
      } else {
        cur += c;
      }
    }
    out.push(cur);
    return out;
  }

  function parseLine(line) {
    let quote = false;
    let i = 0;
    for (; i < line.length; i++) {
      const c = line[i];
      if (c === '"') quote = !quote;
      else if (c === ':' && !quote) break;
    }
    if (i >= line.length) return null;
    const head = splitOutsideQuotes(line.slice(0, i), ';');
    const params = {};
    for (const p of head.slice(1)) {
      const eq = p.indexOf('=');
      if (eq > 0) params[p.slice(0, eq).toUpperCase()] = p.slice(eq + 1).replace(/^"|"$/g, '');
    }
    return {name: head[0].toUpperCase(), params, value: line.slice(i + 1)};
  }

  function unescapeText(v) {
    return v.replace(/\\([\\;,nN])/g, (m, c) => (c === 'n' || c === 'N' ? '\n' : c));
  }

  function parseWall(value) {
    const m = /^(\d{4})(\d{2})(\d{2})(?:T(\d{2})(\d{2})(\d{2})?(Z)?)?$/i.exec(value.trim());
    if (!m) return null;
    return {
      wall: {y: +m[1], mo: +m[2], d: +m[3], h: +(m[4] || 0), mi: +(m[5] || 0), s: +(m[6] || 0)},
      allDay: m[4] === undefined,
      utc: !!m[7],
    };
  }

  // A DTSTART/DTEND/EXDATE/RDATE/RECURRENCE-ID property to a list of
  // {wall, tz, allDay}. tz is 'UTC', an IANA name or null (floating).
  function parseDates(prop, calendarZone) {
    const out = [];
    for (const v of prop.value.split(',')) {
      const p = parseWall(v);
      if (!p) continue;
      const allDay = p.allDay || prop.params.VALUE === 'DATE';
      let tz = null;
      if (p.utc) tz = 'UTC';
      else if (!allDay) tz = resolveZone(prop.params.TZID) || calendarZone || null;
      out.push({wall: p.wall, tz, allDay});
    }
    return out;
  }

  function parseDuration(v) {
    const m = /^([+-])?P(?:(\d+)W)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?$/i.exec(v.trim());
    if (!m) return null;
    const ms = ((+(m[2] || 0) * 7 + +(m[3] || 0)) * 86400 + +(m[4] || 0) * 3600 +
                +(m[5] || 0) * 60 + +(m[6] || 0)) * 1000;
    return m[1] === '-' ? -ms : ms;
  }

  function parseRule(v) {
    const r = {};
    for (const part of v.split(';')) {
      const eq = part.indexOf('=');
      if (eq > 0) r[part.slice(0, eq).toUpperCase()] = part.slice(eq + 1).toUpperCase();
    }
    if (!r.FREQ) return null;
    const ints = (s) => s.split(',').map((x) => parseInt(x, 10)).filter((x) => !isNaN(x));
    return {
      freq: r.FREQ,
      interval: Math.max(1, parseInt(r.INTERVAL || '1', 10) || 1),
      count: r.COUNT ? parseInt(r.COUNT, 10) : null,
      until: r.UNTIL ? parseWall(r.UNTIL) : null,
      byDay: r.BYDAY ? r.BYDAY.split(',').map((x) => {
        const m = /^([+-]?\d+)?(SU|MO|TU|WE|TH|FR|SA)$/.exec(x.trim());
        return m ? {ord: m[1] ? parseInt(m[1], 10) : 0, wd: WEEKDAYS[m[2]]} : null;
      }).filter(Boolean) : null,
      byMonthDay: r.BYMONTHDAY ? ints(r.BYMONTHDAY) : null,
      byMonth: r.BYMONTH ? ints(r.BYMONTH) : null,
      bySetPos: r.BYSETPOS ? ints(r.BYSETPOS) : null,
      wkst: WEEKDAYS[r.WKST] !== undefined ? WEEKDAYS[r.WKST] : 1,
    };
  }

  // ---- Parsing ----------------------------------------------------------

  function parse(src) {
    const lines = String(src == null ? '' : src)
        .replace(/\r\n?/g, '\n').replace(/\n[ \t]/g, '').split('\n');
    const calendar = {timeZone: null, events: []};
    const stack = [];
    let rawEvents = [];
    for (const line of lines) {
      if (!line.trim()) continue;
      const prop = parseLine(line);
      if (!prop) continue;
      if (prop.name === 'BEGIN') {
        stack.push({name: prop.value.trim().toUpperCase(), props: []});
        continue;
      }
      if (prop.name === 'END') {
        const comp = stack.pop();
        if (comp && comp.name === 'VEVENT' &&
            !stack.some((c) => c.name !== 'VCALENDAR')) {
          rawEvents.push(comp);
        }
        continue;
      }
      const top = stack[stack.length - 1];
      if (!top) continue;
      if (top.name === 'VCALENDAR' && prop.name === 'X-WR-TIMEZONE') {
        calendar.timeZone = resolveZone(prop.value);
      }
      top.props.push(prop);
    }

    for (const comp of rawEvents) {
      const first = (name) => comp.props.find((p) => p.name === name) || null;
      const textOf = (name) => {
        const p = first(name);
        return p ? unescapeText(p.value).trim() : '';
      };
      const dtstart = first('DTSTART');
      const start = dtstart ? parseDates(dtstart, calendar.timeZone)[0] : null;
      if (!start) continue;
      const dtend = first('DTEND');
      const duration = first('DURATION');
      const rrule = first('RRULE');
      const recurrence = first('RECURRENCE-ID');
      const props = {};
      for (const p of comp.props) {
        if (!Object.prototype.hasOwnProperty.call(props, p.name)) props[p.name] = unescapeText(p.value);
      }
      const all = (name) => comp.props.filter((p) => p.name === name)
          .flatMap((p) => parseDates(p, calendar.timeZone));
      calendar.events.push({
        uid: textOf('UID'),
        title: textOf('SUMMARY'),
        description: textOf('DESCRIPTION'),
        location: textOf('LOCATION'),
        url: textOf('URL'),
        status: textOf('STATUS').toUpperCase(),
        start,
        end: dtend ? parseDates(dtend, calendar.timeZone)[0] || null : null,
        duration: duration ? parseDuration(duration.value) : null,
        rule: rrule ? parseRule(rrule.value) : null,
        exdates: all('EXDATE'),
        rdates: all('RDATE'),
        recurrenceId: recurrence ? parseDates(recurrence, calendar.timeZone)[0] || null : null,
        props,
      });
    }
    return calendar;
  }

  // ---- Expansion --------------------------------------------------------

  function toUtc(date, fallbackZone) {
    // All-day and floating dates are read in the viewer's zone.
    return wallToUtc(date.wall, date.allDay ? fallbackZone : (date.tz || fallbackZone));
  }

  // Candidate day numbers of a rule, ascending, from the start day on.
  function* ruleDays(rule, startDay) {
    const s = fromDayNumber(startDay);
    const keep = (n) => {
      const c = fromDayNumber(n);
      if (rule.byMonth && rule.byMonth.indexOf(c.mo) < 0) return false;
      if (rule.freq === 'DAILY' && rule.byDay && !rule.byDay.some((b) => b.wd === weekday(n))) return false;
      if (rule.freq === 'DAILY' && rule.byMonthDay && rule.byMonthDay.indexOf(c.d) < 0) return false;
      return true;
    };
    const setPos = (days) => {
      if (!rule.bySetPos) return days;
      const out = [];
      for (const p of rule.bySetPos) {
        const d = p > 0 ? days[p - 1] : days[days.length + p];
        if (d !== undefined) out.push(d);
      }
      return out.sort((a, b) => a - b);
    };
    const monthDays = (y, mo) => {
      const dim = daysInMonth(y, mo);
      const first = dayNumber(y, mo, 1);
      let days = [];
      if (rule.byDay) {
        for (const b of rule.byDay) {
          const all = [];
          for (let d = 0; d < dim; d++) if (weekday(first + d) === b.wd) all.push(first + d);
          if (!b.ord) days.push(...all);
          else {
            const pick = b.ord > 0 ? all[b.ord - 1] : all[all.length + b.ord];
            if (pick !== undefined) days.push(pick);
          }
        }
        if (rule.byMonthDay) {
          days = days.filter((n) => rule.byMonthDay.some(
              (md) => (md > 0 ? md : dim + md + 1) === fromDayNumber(n).d));
        }
      } else {
        for (const md of rule.byMonthDay || [s.d]) {
          const d = md > 0 ? md : dim + md + 1;
          if (d >= 1 && d <= dim) days.push(first + d - 1);
        }
      }
      return setPos([...new Set(days)].sort((a, b) => a - b));
    };

    // Bounded, so a rule that never matches (BYMONTHDAY=30;BYMONTH=2)
    // can't spin forever.
    for (let k = 0; k < 100000; k++) {
      let days;
      if (rule.freq === 'DAILY') {
        days = [startDay + k * rule.interval];
      } else if (rule.freq === 'WEEKLY') {
        const weekStart = startDay - ((weekday(startDay) - rule.wkst + 7) % 7) + k * 7 * rule.interval;
        const wds = rule.byDay ? rule.byDay.map((b) => b.wd) : [weekday(startDay)];
        days = setPos([...new Set(wds.map((wd) => weekStart + ((wd - rule.wkst + 7) % 7)))]
            .sort((a, b) => a - b));
      } else if (rule.freq === 'MONTHLY') {
        const idx = s.y * 12 + (s.mo - 1) + k * rule.interval;
        days = monthDays(Math.floor(idx / 12), (idx % 12) + 1);
      } else if (rule.freq === 'YEARLY') {
        const y = s.y + k * rule.interval;
        days = [];
        for (const mo of rule.byMonth || [s.mo]) {
          if (rule.byDay || rule.byMonthDay) days.push(...monthDays(y, mo));
          else if (s.d <= daysInMonth(y, mo)) days.push(dayNumber(y, mo, s.d));
        }
        days.sort((a, b) => a - b);
      } else {
        return;
      }
      for (const n of days) if (n >= startDay && keep(n)) yield n;
    }
  }

  // DTSTART is always the first occurrence, even when the rule wouldn't
  // produce it (RFC 5545 3.8.5.3).
  function* withStart(days, startDay) {
    yield startDay;
    for (const n of days) if (n !== startDay) yield n;
  }

  function occurrences(calendar, options) {
    const opts = options || {};
    const from = +opts.from;
    const to = +opts.to;
    const zone = opts.timeZone || null;
    const maxSteps = opts.maxSteps || 20000;
    const out = [];

    const key = (ev, startMs) => ev.uid + '|' + startMs;
    const overridden = new Set();
    for (const ev of calendar.events) {
      if (ev.recurrenceId) overridden.add(key(ev, toUtc(ev.recurrenceId, zone)));
    }

    const lengthOf = (ev, startMs) => {
      if (ev.end) return Math.max(0, toUtc(ev.end, zone) - startMs);
      if (ev.duration !== null) return Math.max(0, ev.duration);
      return ev.start.allDay ? DAY : 0;
    };

    for (const ev of calendar.events) {
      const firstStart = toUtc(ev.start, zone);
      const length = lengthOf(ev, firstStart);
      const emit = (startMs) => {
        if (ev.status === 'CANCELLED') return;
        const end = startMs + length;
        if (!(end > from || (length === 0 && startMs >= from)) || startMs >= to) return;
        out.push({
          uid: ev.uid, title: ev.title, start: startMs, end, allDay: ev.start.allDay,
          location: ev.location, description: ev.description, url: ev.url,
          status: ev.status, props: ev.props,
        });
      };

      if (ev.recurrenceId || !ev.rule) {
        if (!ev.recurrenceId && overridden.has(key(ev, firstStart))) continue;
        emit(firstStart);
        if (ev.recurrenceId) continue;
      }

      const excluded = new Set(ev.exdates.map((d) => toUtc(d, zone)));
      const consider = (startMs) => {
        if (excluded.has(startMs) || overridden.has(key(ev, startMs))) return;
        emit(startMs);
      };

      if (ev.rule) {
        const rule = ev.rule;
        const startDay = dayNumber(ev.start.wall.y, ev.start.wall.mo, ev.start.wall.d);
        const time = {h: ev.start.wall.h, mi: ev.start.wall.mi, s: ev.start.wall.s};
        const fromDay = Math.floor(from / DAY) - 2;
        let untilMs = null;
        if (rule.until) {
          untilMs = rule.until.utc ? wallToUtc(rule.until.wall, 'UTC')
              : toUtc({wall: rule.until.wall, tz: ev.start.tz, allDay: ev.start.allDay}, zone);
          if (rule.until.allDay) untilMs += DAY - 1;
        }
        const untilDay = untilMs === null ? Infinity : Math.floor(untilMs / DAY) + 2;
        let count = 0;
        let steps = 0;
        for (const n of withStart(ruleDays(rule, startDay), startDay)) {
          if (++steps > maxSteps || n > untilDay) break;
          if (rule.count !== null && count >= rule.count) break;
          count++;
          if (n < fromDay) continue;
          const c = fromDayNumber(n);
          const startMs = toUtc({wall: {y: c.y, mo: c.mo, d: c.d, h: time.h, mi: time.mi, s: time.s},
                                 tz: ev.start.tz, allDay: ev.start.allDay}, zone);
          if (untilMs !== null && startMs > untilMs) break;
          if (startMs >= to) break;
          consider(startMs);
        }
      }
      for (const d of ev.rdates) {
        const startMs = toUtc(d, zone);
        if (startMs !== firstStart) consider(startMs);
      }
    }

    out.sort((a, b) => a.start - b.start || a.title.localeCompare(b.title));
    return out;
  }

  const api = Object.freeze({parse, occurrences, resolveZone, wallToUtc});
  root.CompassSDK = root.CompassSDK || {};
  root.CompassSDK.ics = api;
  if (typeof module === 'object' && module && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
