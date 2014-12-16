/*
 * Copyright (c) 2014 Red Hat.
 * Copyright (c) 1995 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */

#include "pcpunits.h"

#include <math.h>
#include <inttypes.h>
#include <assert.h>
#include <ctype.h>

// Parse a general "N $units" string into a pmUnits tuple and a multiplier.
// $units can be a series of SCALE-UNIT^EXPONENT, each unit dimension appearing
// at most once.

// An internal variant of pmUnits, but without the narrow bitfields.
// That way, we can tolerate intermediate arithmetic that goes out of
// range of the 4-bit bitfields.
typedef struct pmUnitsBig {
    int dimSpace;    /* space dimension */
    int dimTime;     /* time dimension */
    int dimCount;    /* event dimension */
    unsigned scaleSpace;  /* one of PM_SPACE_* below */
    unsigned scaleTime;   /* one of PM_TIME_* below */
    int scaleCount;  /* one of PM_COUNT_* below */
} pmUnitsBig;

static int
__pmParseUnitsStrPart(const char *str, const char *str_end, pmUnitsBig *out, double *multiplier)
{
    int sts = 0;
    unsigned i;
    const char *ptr; // scanning along str
    enum dimension_t {d_none,d_space,d_time,d_count} dimension;
    struct unit_keyword_t { const char *keyword; int scale; };
    static const struct unit_keyword_t time_keywords[] = {
        { "nanoseconds", PM_TIME_NSEC }, { "nanosecond", PM_TIME_NSEC },
        { "nanosec", PM_TIME_NSEC }, { "ns", PM_TIME_NSEC },
        { "microseconds", PM_TIME_USEC }, { "microsecond", PM_TIME_USEC },
        { "microsec", PM_TIME_USEC }, { "us", PM_TIME_USEC },
        { "milliseconds", PM_TIME_MSEC }, { "millisecond", PM_TIME_MSEC },
        { "millisec", PM_TIME_MSEC }, { "ms", PM_TIME_MSEC },
        { "seconds", PM_TIME_SEC }, { "second", PM_TIME_SEC },
        { "sec", PM_TIME_SEC },
        { "s", PM_TIME_SEC },
        { "minutes", PM_TIME_MIN }, { "minute", PM_TIME_MIN }, { "min", PM_TIME_MIN },
        { "hours", PM_TIME_HOUR }, { "hour", PM_TIME_HOUR }, { "hr", PM_TIME_HOUR },
        { "time-0", 0 }, /* { "time-1", 1 }, */ { "time-2", 2 }, { "time-3", 3 },
        { "time-4", 4 }, { "time-5", 5 }, { "time-6", 6 }, { "time-7", 7 },
        { "time-8", 8 }, { "time-9", 9 }, { "time-10", 10 }, { "time-11", 11 },
        { "time-12", 12 }, { "time-13", 13 }, { "time-14", 14 }, { "time-15", 15 },
        { "time-1", 1 },
    };
    const size_t num_time_keywords = sizeof(time_keywords) / sizeof(time_keywords[0]);
    static const struct unit_keyword_t space_keywords[] = {
        { "bytes", PM_SPACE_BYTE }, { "byte", PM_SPACE_BYTE },
        { "Kbytes", PM_SPACE_KBYTE }, { "Kbyte", PM_SPACE_KBYTE },
        { "Kilobytes", PM_SPACE_KBYTE }, { "Kilobyte", PM_SPACE_KBYTE },
        { "KB", PM_SPACE_KBYTE },
        { "Mbytes", PM_SPACE_MBYTE }, { "Mbyte", PM_SPACE_MBYTE },
        { "Megabytes", PM_SPACE_MBYTE }, { "Megabyte", PM_SPACE_MBYTE },
        { "MB", PM_SPACE_MBYTE },
        { "Gbytes", PM_SPACE_GBYTE }, { "Gbyte", PM_SPACE_GBYTE },
        { "Gigabytes", PM_SPACE_GBYTE }, { "Gigabyte", PM_SPACE_GBYTE },
        { "GB", PM_SPACE_GBYTE },
        { "Tbytes", PM_SPACE_TBYTE }, { "Tbyte", PM_SPACE_TBYTE },
        { "Terabytes", PM_SPACE_TBYTE }, { "Terabyte", PM_SPACE_TBYTE },
        { "TB", PM_SPACE_TBYTE },
        { "Pbytes", PM_SPACE_PBYTE }, { "Pbyte", PM_SPACE_PBYTE },
        { "Petabytes", PM_SPACE_PBYTE }, { "Petabyte", PM_SPACE_PBYTE },
        { "PB", PM_SPACE_PBYTE },
        { "Ebytes", PM_SPACE_EBYTE }, { "Ebyte", PM_SPACE_EBYTE },
        { "Exabytes", PM_SPACE_EBYTE }, { "Exabyte", PM_SPACE_EBYTE },
        { "EB", PM_SPACE_EBYTE },
        { "space-0", 0 }, /* { "space-1", 1 }, */ { "space-2", 2 }, { "space-3", 3 },
        { "space-4", 4 }, { "space-5", 5 }, { "space-6", 6 }, { "space-7", 7 },
        { "space-8", 8 }, { "space-9", 9 }, { "space-10", 10 }, { "space-11", 11 },
        { "space-12", 12 }, { "space-13", 13 }, { "space-14", 14 }, { "space-15", 15 },
        { "space-1", 1 },
    };
    const size_t num_space_keywords = sizeof(space_keywords) / sizeof(space_keywords[0]);
    static const struct unit_keyword_t count_keywords[] = {
        { "count x 10^-8", -8 },
        { "count x 10^-7", -7 },
        { "count x 10^-6", -6 },
        { "count x 10^-5", -5 },
        { "count x 10^-4", -4 },
        { "count x 10^-3", -3 },
        { "count x 10^-2", -2 },
        { "count x 10^-1", -1 },
        /* { "count", 0 }, { "counts", 0 }, */
        /* { "count x 10", 1 },*/
        { "count x 10^2", 2 },
        { "count x 10^3", 3 },
        { "count x 10^4", 4 },
        { "count x 10^5", 5 },
        { "count x 10^6", 6 },
        { "count x 10^7", 7 },
        { "count x 10", 1 },
        { "counts", 0 },
        { "count", 0 },
        // NB: we don't support the anomalous "x 10^SCALE" syntax for the dimCount=0 case.
    };
    const size_t num_count_keywords = sizeof(count_keywords) / sizeof(count_keywords[0]);
    static const struct unit_keyword_t exponent_keywords[] = {
        { "^-8", -8 }, { "^-7", -7 }, { "^-6", -6 }, { "^-5", -5 },
        { "^-4", -4 }, { "^-3", -3 }, { "^-2", -2 }, { "^-1", -1 },
        { "^0", 0 }, /*{ "^1", 1 }, */ { "^2", 2 }, { "^3", 3 },
        { "^4", 4 }, { "^5", 5 }, { "^6", 6 }, { "^7", 7 },
        // NB: the following larger exponents are enabled by use of pmUnitsBig above.
        // They happen to be necessary because pmUnitsStr emits foo-dim=-8 as "/ foo^8",
        // so the denominator could encounter wider-than-bitfield exponents.
        { "^8", 8 }, { "^9", 9 }, { "^10", 10 }, { "^11", 11 },
        { "^12", 12 }, { "^13", 13 }, { "^14", 14 }, { "^15", 15 },
        { "^1", 1 },
    };
    const size_t num_exponent_keywords = sizeof(exponent_keywords) / sizeof(exponent_keywords[0]);

    *multiplier = 1.0;
    memset (out, 0, sizeof (*out));
    ptr = str;

    while (ptr != str_end) { // parse whole string
        assert (*ptr != '\0');

        if (isspace (*ptr)) { // skip whitespace
            ptr ++;
            continue;
        }

        if (*ptr == '-' || *ptr == '.' || isdigit(*ptr)) { // possible floating-point number
            // parse it with strtod(3).
            char *newptr;
            errno = 0;
            double m = strtod(ptr, &newptr);
            if (errno || newptr == ptr || newptr > str_end) {
                sts = PM_ERR_CONV;
                goto out;
            }
            ptr = newptr;
            *multiplier *= m;
            continue;
        }

        dimension = d_none; // classify dimension of base unit

        // match & skip over keyword (followed by space, ^, or EOL)
#define streqskip(q) (((ptr+strlen(q) <= str_end) &&        \
                       (strncasecmp(ptr,q,strlen(q))==0) && \
                       ((isspace(*(ptr+strlen(q)))) ||      \
                        (*(ptr+strlen(q))=='^') ||          \
                        (ptr+strlen(q)==str_end)))          \
                       ? (ptr += strlen(q), 1) : 0)

        // parse base unit, only once per input string.  We don't support
        // "microsec millisec", as that would require arithmetic on the scales.
        // We could support "sec sec" (and turn that into sec^2) in the future.
        for (i=0; i<num_time_keywords; i++)
            if (dimension == d_none && out->dimTime == 0 && streqskip (time_keywords[i].keyword)) {
                out->scaleTime = time_keywords[i].scale;
                dimension = d_time;
            }
        for (i=0; i<num_space_keywords; i++)
            if (dimension == d_none && out->dimSpace == 0 && streqskip (space_keywords[i].keyword)) {
                out->scaleSpace = space_keywords[i].scale;
                dimension = d_space;
            }
        for (i=0; i<num_count_keywords; i++)
            if (dimension == d_none && out->dimCount == 0 && streqskip (count_keywords[i].keyword)) {
                out->scaleCount = count_keywords[i].scale;
                dimension = d_count;
            }

        // parse optional dimension exponent
        switch (dimension) {
        case d_none:
            // unrecognized base unit, punt!
            sts = PM_ERR_CONV;
            goto out;

        case d_time:
            if (ptr == str_end || isspace(*ptr)) {
                out->dimTime = 1;
            } else {
                for (i=0; i<num_exponent_keywords; i++)
                    if (streqskip (exponent_keywords[i].keyword)) {
                        out->dimTime = exponent_keywords[i].scale;
                        break;
                    }
            }
            break;

        case d_space:
            if (ptr == str_end || isspace(*ptr)) {
                out->dimSpace = 1;
            } else {
                for (i=0; i<num_exponent_keywords; i++)
                    if (streqskip (exponent_keywords[i].keyword)) {
                        out->dimSpace = exponent_keywords[i].scale;
                        break;
                    }
            }
            break;

        case d_count:
            if (ptr == str_end || isspace(*ptr)) {
                out->dimCount = 1;
            } else {
                for (i=0; i<num_exponent_keywords; i++)
                    if (streqskip (exponent_keywords[i].keyword)) {
                        out->dimCount = exponent_keywords[i].scale;
                        break;
                    }
            }
            break;
        }

        // fall through to next unit^exponent bit, if any
    }

out:
    return sts;
}



// Parse a general "N $units / M $units" string into a pmUnits tuple and a multiplier.
int
cockpit_pmParseUnitsStr(const char *str, pmUnits *out, double *multiplier)
{
    const char *slash;
    double dividend_mult, divisor_mult;
    pmUnitsBig dividend, divisor;
    int sts;
    int dim;

    assert (str);
    assert (out);
    assert (multiplier);

    memset (out, 0, sizeof (*out));

    // Parse the dividend and divisor separately
    slash = strchr (str, '/');
    if (slash == NULL) {
        sts = __pmParseUnitsStrPart(str, str+strlen(str), & dividend, & dividend_mult);
        if (sts < 0)
            goto out;
        // empty string for nonexistent denominator; will just return (0,0,0,0,0,0)*1.0
        sts = __pmParseUnitsStrPart(str+strlen(str), str+strlen(str), & divisor, & divisor_mult);
        if (sts < 0)
            goto out;
    } else {
        sts = __pmParseUnitsStrPart(str, slash, & dividend, & dividend_mult);
        if (sts < 0)
            goto out;
        sts = __pmParseUnitsStrPart(slash+1, str+strlen(str), & divisor, & divisor_mult);
        if (sts < 0)
            goto out;
    }

    // Compute the quotient dimensionality, check for possible bitfield overflow.
    dim = dividend.dimSpace - divisor.dimSpace;
    if (dim < -8 || dim > 7) {
        sts = PM_ERR_CONV;
        goto out;
    } else {
        out->dimSpace = dim;
    }
    dim = dividend.dimTime - divisor.dimTime;
    if (dim < -8 || dim > 7) {
        sts = PM_ERR_CONV;
        goto out;
    } else {
        out->dimTime = dim;
    }
    dim = dividend.dimCount - divisor.dimCount;
    if (dim < -8 || dim > 7) {
        sts = PM_ERR_CONV;
        goto out;
    } else {
        out->dimCount = dim;
    }

    // Compute the individual scales.  In theory, we have considerable
    // freedom here, because we are also outputting a multiplier.  We
    // could just set all out->scale* to 0, and accumulate the
    // compensating scaling multipliers there.  But in order to
    // fulfill the testing-oriented invariant:
    //
    // for all valid pmUnits u:
    //     pmParseUnitsStr(pmUnitsStr(u), out_u, out_multiplier) succeeds, and
    //     out_u == u, and
    //     out_multiplier == 1.0
    //
    // we need to propagate scales to some extent.  It is helpful to
    // realize that pmUnitsStr() never generates multiplier literals,
    // nor the same dimension in the dividend and divisor.

    *multiplier = divisor_mult / dividend_mult; // NB: note reciprocation

    if (dividend.dimSpace == 0 && divisor.dimSpace != 0)
        out->scaleSpace = divisor.scaleSpace;
    else if (divisor.dimSpace == 0 && dividend.dimSpace != 0)
        out->scaleSpace = dividend.scaleSpace;
    else { // both have space dimension; must compute a scale/multiplier
        out->scaleSpace = PM_SPACE_BYTE;
        *multiplier *= pow (pow (1024.0, (double) dividend.scaleSpace), -(double)dividend.dimSpace);
        *multiplier *= pow (pow (1024.0, (double) divisor.scaleSpace), (double)divisor.dimSpace);
        if (out->dimSpace == 0) // became dimensionless?
            out->scaleSpace = 0;
    }

    if (dividend.dimCount == 0 && divisor.dimCount != 0)
        out->scaleCount = divisor.scaleCount;
    else if (divisor.dimCount == 0 && dividend.dimCount != 0)
        out->scaleCount = dividend.scaleCount;
    else { // both have count dimension; must compute a scale/multiplier
        out->scaleCount = PM_COUNT_ONE;
        *multiplier *= pow (pow (10.0, (double) dividend.scaleCount), -(double)dividend.dimCount);
        *multiplier *= pow (pow (10.0, (double) divisor.scaleCount), (double)divisor.dimCount);
        if (out->dimCount == 0) // became dimensionless?
            out->scaleCount = 0;
    }

    if (dividend.dimTime == 0 && divisor.dimTime != 0)
        out->scaleTime = divisor.scaleTime;
    else if (divisor.dimTime == 0 && dividend.dimTime != 0)
        out->scaleTime = dividend.scaleTime;
    else { // both have time dimension; must compute a scale/multiplier
        out->scaleTime = PM_TIME_SEC;
        static const double time_scales [] = {[PM_TIME_NSEC] = 0.000000001,
                                              [PM_TIME_USEC] = 0.000001,
                                              [PM_TIME_MSEC] = 0.001,
                                              [PM_TIME_SEC]  = 1,
                                              [PM_TIME_MIN]  = 60,
                                              [PM_TIME_HOUR] = 3600 };
        // guaranteed by __pmParseUnitsStrPart; ensure in-range array access
        assert (dividend.scaleTime >= PM_TIME_NSEC && dividend.scaleTime <= PM_TIME_HOUR);
        assert (divisor.scaleTime >= PM_TIME_NSEC && divisor.scaleTime <= PM_TIME_HOUR);
        *multiplier *= pow (time_scales[dividend.scaleTime], -(double)dividend.dimTime);
        *multiplier *= pow (time_scales[divisor.scaleTime], (double)divisor.dimTime);
        if (out->dimTime == 0) // became dimensionless?
            out->scaleTime = 0;
    }

 out:
    if (sts < 0) {
        memset (out, 0, sizeof (*out)); // clear partially filled in pmUnits
        *multiplier = 1.0;
    }
    return sts;
}
