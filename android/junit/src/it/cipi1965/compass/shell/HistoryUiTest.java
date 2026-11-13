// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

import org.chromium.base.test.BaseRobolectricTestRunner;

import java.util.ArrayList;
import java.util.Calendar;
import java.util.List;

/** History's days (HistoryUi.byDay()). */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class HistoryUiTest {
    private static HistoryUi.Visit visit(int index, Calendar time) {
        return new HistoryUi.Visit(index, "Page " + index, "https://e.example/", "e.example",
                time.getTimeInMillis());
    }

    private static Calendar at(int year, int month, int day, int hour) {
        Calendar calendar = Calendar.getInstance();
        calendar.clear();
        calendar.set(year, month, day, hour, 0);
        return calendar;
    }

    @Test
    public void todayYesterdayThenDates() {
        Calendar now = at(2026, Calendar.NOVEMBER, 13, 9);
        List<HistoryUi.Visit> visits = new ArrayList<>();
        visits.add(visit(0, at(2026, Calendar.NOVEMBER, 13, 8)));
        visits.add(visit(1, at(2026, Calendar.NOVEMBER, 13, 0)));
        visits.add(visit(2, at(2026, Calendar.NOVEMBER, 12, 23)));
        visits.add(visit(3, at(2026, Calendar.NOVEMBER, 10, 12)));
        visits.add(visit(4, at(2026, Calendar.NOVEMBER, 10, 11)));
        List<HistoryUi.Day> days = HistoryUi.byDay(visits, now, time -> "D" + (time / 86400000L));
        assertEquals(3, days.size());
        assertEquals("Today", days.get(0).title);
        assertEquals(2, days.get(0).visits.size());
        assertEquals("Yesterday", days.get(1).title);
        assertEquals(1, days.get(1).visits.size());
        assertTrue(days.get(2).title.startsWith("D"));
        assertEquals(2, days.get(2).visits.size());
    }

    @Test
    public void noVisitsNoDays() {
        assertTrue(
                HistoryUi.byDay(new ArrayList<>(), Calendar.getInstance(), time -> "").isEmpty());
    }
}
