/*
SurfOS user program: cal
--------------------
File: cal.c     Date: 11/01/26
--------------------
cal [[month] year]: a month's calendar, its weeks from Sunday, as the desktop's Clock copies it
(user/wclock/clock.c, whose arithmetic this is): this month without arguments, with a number
of 1 to 9999 the whole of that year, three months a row, and with two that month. A lone
number is always a year: cal 26 is the year 26. The calendar is the Gregorian one for every
year: the months other systems skip in 1752 are not skipped here.
*/

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../diff/cli.h"
#include "../wclock/clock.h"

#define MONTH_WIDTH 20
#define GAP 2

/* line row (0 the title, 1 the days' names, 2.. the weeks) of a month into out, 20 columns, spaces at the end
   included; the weeks a month does not have are blank */
static void month_row(unsigned year, unsigned month, int row, int with_year, char *out) {
    int first = clock_weekday(year, month, 1), days = clock_days(year, month), cell, k = 0;
    memset(out, ' ', MONTH_WIDTH);
    out[MONTH_WIDTH] = 0;
    if(row == 0) {
        char title[32];
        int n = with_year ? snprintf(title, sizeof(title), "%s %u", clock_months[month - 1], year)
                          : snprintf(title, sizeof(title), "%s", clock_months[month - 1]);
        memcpy(out + (MONTH_WIDTH - n) / 2, title, (size_t)n);
        return;
    }
    if(row == 1) {
        memcpy(out, "Su Mo Tu We Th Fr Sa", MONTH_WIDTH);
        return;
    }
    for(cell = (row - 2) * 7; cell < (row - 1) * 7; cell++, k += 3) {
        int day = cell - first + 1;
        if(day >= 1 && day <= days) {
            if(day >= 10) out[k] = (char)('0' + day / 10);
            out[k + 1] = (char)('0' + day % 10);
        }
    }
}

static void put_trimmed(char *line) {
    size_t n = strlen(line);
    while(n && line[n - 1] == ' ') n--;
    line[n] = 0;
    puts(line);
}

static void show_month(unsigned year, unsigned month) {
    char line[MONTH_WIDTH + 1];
    int row, weeks = (clock_weekday(year, month, 1) + clock_days(year, month) + 6) / 7;
    for(row = 0; row < weeks + 2; row++) {
        month_row(year, month, row, 1, line);
        put_trimmed(line);
    }
}

static void show_year(unsigned year) {
    char line[3 * MONTH_WIDTH + 2 * GAP + 1], part[MONTH_WIDTH + 1];
    int total = 3 * MONTH_WIDTH + 2 * GAP, row, q, m, n;
    n = snprintf(part, sizeof(part), "%u", year);
    printf("%*s%s\n", (3 * MONTH_WIDTH - n) / 2, "", part);       /* over the months, as cal elsewhere has it */
    for(q = 0; q < 4; q++) {
        if(q) putchar('\n');
        for(row = 0; row < 8; row++) {
            memset(line, ' ', (size_t)total);
            line[total] = 0;
            for(m = 0; m < 3; m++) {
                month_row(year, (unsigned)(q * 3 + m + 1), row, 0, part);
                memcpy(line + m * (MONTH_WIDTH + GAP), part, MONTH_WIDTH);
            }
            put_trimmed(line);
        }
    }
}

static int number(const char *s, int top, const char *what, int *v) {
    if(cli_int(s, v) < 0 || *v < 1 || *v > top) {
        fprintf(stderr, "cal: %s: not a %s (1 to %d)\n", s, what, top);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int month = 0, year = 0;
    if(argc > 3 || (argc > 1 && argv[1][0] == '-')) {
        fprintf(stderr, "usage: cal [[month] year]\n");
        return 2;
    }
    if(argc == 1) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        month = tm.tm_mon + 1;
        year = tm.tm_year + 1900;
    } else if(argc == 2) {
        if(number(argv[1], 9999, "year", &year) < 0) return 2;
    } else if(number(argv[1], 12, "month", &month) < 0 || number(argv[2], 9999, "year", &year) < 0) return 2;
    if(month) show_month((unsigned)year, (unsigned)month);
    else show_year((unsigned)year);
    return cli_flush("cal");
}
