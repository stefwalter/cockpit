/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __COCKPIT_WS_TRANSPORT_H__
#define __COCKPIT_WS_TRANSPORT_H__

#include "cockpit/cockpittransport.h"

#include "cockpitcreds.h"

G_BEGIN_DECLS

#define COCKPIT_TYPE_WS_TRANSPORT         (cockpit_ws_transport_get_type ())
#define COCKPIT_WS_TRANSPORT(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_WS_TRANSPORT, CockpitWsTransport))
#define COCKPIT_IS_WS_TRANSPORT(k)        (G_TYPE_CHECK_INSTANCE_TYPE ((k), COCKPIT_TYPE_WS_TRANSPORT))

typedef struct _CockpitWsTransport        CockpitWsTransport;
typedef struct _CockpitWsTransportClass   CockpitWsTransportClass;

GType               cockpit_ws_transport_get_type              (void) G_GNUC_CONST;

CockpitTransport *  cockpit_ws_transport_new                   (const gchar *host,
                                                                guint port,
                                                                CockpitCreds *creds);

const gchar *       cockpit_ws_transport_get_host_key          (CockpitWsTransport *self);

const gchar *       cockpit_ws_transport_get_host_fingerprint  (CockpitWsTransport *self);

G_END_DECLS

#endif /* __COCKPIT_WS_TRANSPORT_H__ */
