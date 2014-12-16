/*
 * Copyright (c) 2014 Red Hat.
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

#ifndef PCP_UNITS_H__
#define PCP_UNITS_H__

#include <pcp/pmapi.h>

/* This is the version of pmParseUnitsStr as proposed here:

   fche/units-parse @ git://sourceware.org/git/pcpfans.git
 */

int cockpit_pmParseUnitsStr(const char *, pmUnits *, double *);

#endif /* PCP_UNITS_H__ */
