/*
 * libvirt-colo.c: entry points for colo APIs
 *
 * Copyright (C) 2006-2019 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "datatypes.h"
#include "virlog.h"

VIR_LOG_INIT("libvirt.colo");

#define VIR_FROM_THIS VIR_FROM_NONE

/**
 * virColoSavePeerStatus:
 * @conn: peer libvirt conn
 * @uri: peer libvirt uri
 *
 * Save COLO peer status
 *
 * Returns the result.
 * 
 * Since: 9.1.0
 */
int virColoSavePeerStatus(virConnectPtr conn, const char *uri) {
    int ret;
    VIR_DEBUG("in libvirt-colo.c virColoSavePeerStatus\n conn=%p\n uri: %s\n", conn, uri);
    virResetLastError();

    virCheckConnectReturn(conn, -1);

    if (conn->driver->coloSavePeerStatus) {
        VIR_DEBUG("conn->driver->coloSavePeerStatus exists! \n");
        ret = conn->driver->coloSavePeerStatus(conn, uri);
        if (ret != 0) {
            goto error;
        }
        return ret;
    }
    virReportUnsupportedError();

error:
    virDispatchError(conn);
    return -1;
}
