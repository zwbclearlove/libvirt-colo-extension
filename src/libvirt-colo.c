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
 * @uri: peer libvirt uri
 *
 * Save COLO peer status
 *
 * Since: 9.1.0
 */
int virColoSavePeerStatus(virConnectPtr conn, const char *uri) {
    VIR_DEBUG("in libvirt-colo.c virColoSavePeerStatus\n conn=%p\n uri: %s\n", conn, uri);
    virResetLastError();
    return 0;
//     virCheckConnectReturn(conn, -1);
    


// error:
//     virDispatchError(conn);
//     return -1;
}
