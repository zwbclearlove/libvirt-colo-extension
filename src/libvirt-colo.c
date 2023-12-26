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
 * virColoConfigLoadFile:
 * @cfg: colo config
 * @filename: colo config file path
 * 
 * get colo config from filename
 *
 * Returns result.
 * 
 * Since: 9.1.0
 */
int virColoConfigLoadFile(virColoConfig *cfg, const char *filename) {
    g_autoptr(virConf) conf = NULL;
    int rv;
    char* tmp = g_new0(char, 10);
    /* Just check the file is readable before opening it, otherwise
     * libvirt emits an error.
     */
    
    if (access(filename, R_OK) == -1) {
        VIR_INFO("Could not read colo config file %s", filename);
        return 0;
    }

    if (!(conf = virConfReadFile(filename, 0)))
        return -1;
    

    if ((rv = virConfGetValueString(conf, "colo_local_libvirt_status", &tmp)) < 0)
        return -1;
    
    if (strcmp(tmp, "PRIMARY") == 0) {
        cfg->colo_local_libvirt_status = VIRSH_COLO_PEER_PRIMARY;
    } else {
        cfg->colo_local_libvirt_status = VIRSH_COLO_PEER_SECONDARY;
    }
    if ((rv = virConfGetValueString(conf, "colo_peer_libvirt_status", &tmp)) < 0)
        return -1;
    
    if (strcmp(tmp, "PRIMARY") == 0) {
        cfg->colo_peer_libvirt_status = VIRSH_COLO_PEER_PRIMARY;
    } else {
        cfg->colo_peer_libvirt_status = VIRSH_COLO_PEER_SECONDARY;
    }

    if ((rv = virConfGetValueString(conf, "colo_peer_libvirt_uri", &cfg->colo_peer_libvirt_uri)) < 0)
        return -1;
    g_free(tmp);
    return 0;
}


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


/**
 * virColoGetColoVMList:
 *
 * Get Colo enabled VM list
 *
 * Returns the result..
 * 
 * Since: 9.1.0
 */



