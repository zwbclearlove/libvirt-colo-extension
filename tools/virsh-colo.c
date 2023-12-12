/*
 * virsh-colo.c: Commands to manage colo
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

#include "virsh-colo.h"
#include "vsh.h"
#include <assert.h>

#include "internal.h"
#include "virbuffer.h"
#include "viralloc.h"
#include "virfile.h"
#include "virsh-util.h"
#include "virxml.h"
#include "virsh.h"
#include "vsh-table.h"
#include "virsh-completer-domain.h"
/*
 * "colo-connect-peer" command
 */
static const vshCmdInfo info_colo_connect_peer[] = {
    {.name = "help",
     .data = N_("connect peer libvirt")
    },
    {.name = "desc",
     .data = N_("connect peer libvirt and record it ")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_colo_connect_peer[] = {
    {.name = "uri",
     .type = VSH_OT_STRING,
     .flags = VSH_OFLAG_EMPTY_OK,
     .completer = virshCompleteEmpty,
     .help = N_("hypervisor connection URI")
    },
    {.name = "peer-type",
     .type = VSH_OT_STRING,
     .help = N_("peer libvirt type")
    },
    {.name = NULL}
};

static bool
cmdColoConnectPeer(vshControl *ctl,
                    const vshCmd *cmd)
{
    bool ret = false;
    const char *uri = NULL;
    const char *peerType = NULL;
    //char* errorInfo = NULL;

    virshControl* priv = ctl->privData;


    if (vshCommandOptStringReq(ctl, cmd, "uri", &uri) < 0) {
        return false;
    }
    if (vshCommandOptStringReq(ctl, cmd, "peer-type", &peerType) < 0) {
        return false;
    }
    vshPrintExtra(ctl, _("uri : %s\npeer type : %s\n"), uri, peerType);

    if (virshReconnect(ctl, uri, false, true) < 0) {
        vshError(ctl, _("Failed to connect peer libvirt."));
        goto cleanup;
    }

    if (virColoSavePeerStatus(priv->conn, uri) < 0) {
        vshError(ctl, _("Failed to save peer info"));
        goto cleanup;
    }

    vshPrint(ctl, "connect peer libvirt success. \n");

    ret = true;

cleanup:
    return ret;
}



/*
 * "colo-connect-status" command
 */
static const vshCmdInfo info_colo_connect_status[] = {
    {.name = "help",
     .data = N_("show colo node connect status")
    },
    {.name = "desc",
     .data = N_("show the connection status between "
                "current colo node and backup node")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_colo_connect_status[] = {
    {.name = "local",
     .type = VSH_OT_BOOL,
     .help = N_("show colo node status in local.")
    },
    {.name = NULL}
};

static bool
cmdColoConnectStatus(vshControl *ctl,
                    const vshCmd *cmd)
{
    
    bool ret = false;
    // char *buffer;
    if (vshCommandOptBool(cmd, "local")) {
        vshPrint(ctl, "local libvirt status : \n");
        vshPrint(ctl, "-----------------------\n");
    } else {
        vshPrint(ctl, "without local flag!!!\n");
        goto cleanup;
    }
        
    
    vshPrint(ctl, "peer libvirt status : \n");
    vshPrint(ctl, "----------------------\n");

    ret = true;
    cleanup:
    return ret;
}

/*
 * "colo-vm-list" command
 */
static const vshCmdInfo info_colo_vm_list[] = {
    {.name = "help",
     .data = N_("show all the colo domain")
    },
    {.name = "desc",
     .data = N_("show all the domain which can be "
                "colo-enabled")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_colo_vm_list[] = {
    {.name = "colo-enabled",
     .type = VSH_OT_BOOL,
     .help = N_("list colo-enabled domains")
    },
    {.name = "all",
     .type = VSH_OT_BOOL,
     .help = N_("list all domains")
    },
    {.name = NULL}
};

static bool
cmdColoVMList(vshControl *ctl,
                    const vshCmd *cmd)
{
//     size_t i;
//     char uuid[VIR_UUID_STRING_BUFLEN];
//     int state;
//     bool ret = false;
//     struct virshDomainList *list = NULL;
//     virDomainPtr dom;
//     char id_buf[VIR_INT64_STR_BUFLEN];
//     unsigned int id;
//     unsigned int flags = VIR_CONNECT_LIST_DOMAINS_ACTIVE;
//     g_autoptr(vshTable) table = NULL;

//     /* construct filter flags */
//     if (vshCommandOptBool(cmd, "colo-enabled"))
//         flags = 1;

//     if (vshCommandOptBool(cmd, "all"))
//         flags = 1 | 2;

//     if (!(list = virshDomainListCollect(ctl, flags)))
//         goto cleanup;

//     /* print table header in legacy mode */
//     table = vshTableNew(_("Id"), _("Name"), _("State"), NULL);

//     for (i = 0; i < list->ndomains; i++) {
//         const char *sep = "";

//         dom = list->domains[i];
//         id = virDomainGetID(dom);
//         if (id != (unsigned int) -1)
//             g_snprintf(id_buf, sizeof(id_buf), "%d", id);
//         else
//             ignore_value(virStrcpyStatic(id_buf, "-"));

        
//         state = virshDomainState(ctl, dom, NULL);

//         /* Domain could've been removed in the meantime */
//         if (state < 0)
//             continue;

//         if (managed && state == VIR_DOMAIN_SHUTOFF &&
//             virDomainHasManagedSaveImage(dom, 0) > 0)
//             state = -2;

//         if (optTitle) {
//             g_autofree char *title = NULL;

//             if (!(title = virshGetDomainDescription(ctl, dom, true, 0)))
//                 goto cleanup;
//             if (vshTableRowAppend(table, id_buf,
//                                     virDomainGetName(dom),
//                                     state == -2 ? _("saved")
//                                     : virshDomainStateToString(state),
//                                     title, NULL) < 0)
//                 goto cleanup;
//         } else {
//             if (vshTableRowAppend(table, id_buf,
//                                     virDomainGetName(dom),
//                                     state == -2 ? _("saved")
//                                     : virshDomainStateToString(state),
//                                     NULL) < 0)
//                 goto cleanup;
//         }

        
//     }

    
//     vshTablePrintToStdout(table, ctl);

//     ret = true;
//  cleanup:
//     virshDomainListFree(list);
    vshCommandOptBool(cmd, "all");
    vshPrint(ctl, "colo list -----\n");
    return true;
}

/*
 * "colo-enable" command
 */
static const vshCmdInfo info_colo_enable[] = {
    {.name = "help",
     .data = N_("enable colo capacity for a domain")
    },
    {.name = "desc",
     .data = N_("enable colo capacity for a domain "
                "by name, id or uuid")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_colo_enable[] = {
    VIRSH_COMMON_OPT_DOMAIN_FULL(0),
    {.name = NULL}
};

static bool
cmdColoEnable(vshControl *ctl,
                    const vshCmd *cmd)
{
    g_autoptr(virshDomain) dom = NULL;
    if (!(dom = virshCommandOptDomain(ctl, cmd, NULL)))
        vshPrint(ctl, "cannot find domain -----\n");
    vshCommandOptBool(cmd, "all");
    vshPrint(ctl, "colo enabled for domain -----\n");
    return true;
}

/*
 * "colo-disable" command
 */
static const vshCmdInfo info_colo_disable[] = {
    {.name = "help",
     .data = N_("disable colo capacity for a domain")
    },
    {.name = "desc",
     .data = N_("disable colo capacity for a domain "
                "by name, id or uuid")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_colo_disable[] = {
    VIRSH_COMMON_OPT_DOMAIN_FULL(0),
    {.name = NULL}
};

static bool
cmdColoDisable(vshControl *ctl,
                    const vshCmd *cmd)
{
    g_autoptr(virshDomain) dom = NULL;
    if (!(dom = virshCommandOptDomain(ctl, cmd, NULL)))
        vshPrint(ctl, "cannot find domain -----\n");  
    vshPrint(ctl, "colo disabled for domain -----\n");
    return true;
}


/*
 * "colo-vm-status" command
 */
static const vshCmdInfo info_colo_vm_status[] = {
    {.name = "help",
     .data = N_("show colo status of a domain")
    },
    {.name = "desc",
     .data = N_("show colo status of a domain "
                "by name, id or uuid")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_colo_vm_status[] = {
    VIRSH_COMMON_OPT_DOMAIN_FULL(0),
    {.name = NULL}
};

static bool
cmdColoVMStatus(vshControl *ctl,
                    const vshCmd *cmd)
{
    g_autoptr(virshDomain) dom = NULL;
    if (!(dom = virshCommandOptDomain(ctl, cmd, NULL)))
        vshPrint(ctl, "cannot find domain -----\n");    
    vshPrint(ctl, "colo status of domain -----\n");
    return true;
}

/*
 * "colo-set-params" command
 */
static const vshCmdInfo info_colo_set_params[] = {
    {.name = "help",
     .data = N_("set colo params of a domain")
    },
    {.name = "desc",
     .data = N_("set colo params of a domain "
                "by name, id or uuid")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_colo_set_params[] = {
    VIRSH_COMMON_OPT_DOMAIN_FULL(0),
    {.name = "checkpoint-time",
     .type = VSH_OT_STRING,
     .flags = VSH_OFLAG_EMPTY_OK,
     .completer = virshCompleteEmpty,
     .help = N_("colo checkpoint interval")
    },
    {.name = "compare_timeout",
     .type = VSH_OT_STRING,
     .flags = VSH_OFLAG_EMPTY_OK,
     .completer = virshCompleteEmpty,
     .help = N_("colo compare timeout")
    },
    {.name = "max_queue_size",
     .type = VSH_OT_STRING,
     .flags = VSH_OFLAG_EMPTY_OK,
     .completer = virshCompleteEmpty,
     .help = N_("colo max queue size")
    },

    {.name = NULL}
};

static bool
cmdColoSetParams(vshControl *ctl,
                    const vshCmd *cmd)
{
    g_autoptr(virshDomain) dom = NULL;
    if (!(dom = virshCommandOptDomain(ctl, cmd, NULL)))
        vshPrint(ctl, "cannot find domain -----\n"); 
    vshPrint(ctl, "colo set params -----\n");
    return true;
}


/*
 * "colo-do-failover" command
 */
static const vshCmdInfo info_colo_do_failover[] = {
    {.name = "help",
     .data = N_("do colo failover for a domain")
    },
    {.name = "desc",
     .data = N_("do colo failover for a domain "
                "by name, id or uuid")
    },
    {.name = NULL}
};

static const vshCmdOptDef opts_colo_do_failover[] = {
    VIRSH_COMMON_OPT_DOMAIN_FULL(0),
    {.name = NULL}
};

static bool
cmdColoDoFailover(vshControl *ctl,
                    const vshCmd *cmd)
{
    g_autoptr(virshDomain) dom = NULL;
    if (!(dom = virshCommandOptDomain(ctl, cmd, NULL)))
        vshPrint(ctl, "cannot find domain -----\n"); 
    vshPrint(ctl, "colo do failover for domain -----\n");
    return true;
}


const vshCmdDef coloCmds[] = {
    {.name = "colo-connect-peer",
     .handler = cmdColoConnectPeer,
     .opts = opts_colo_connect_peer,
     .info = info_colo_connect_peer,
     .flags = 0
    },
    {.name = "colo-connect-status",
     .handler = cmdColoConnectStatus,
     .opts = opts_colo_connect_status,
     .info = info_colo_connect_status,
     .flags = 0
    },
    {.name = "colo-vm-list",
     .handler = cmdColoVMList,
     .opts = opts_colo_vm_list,
     .info = info_colo_vm_list,
     .flags = 0
    },
    {.name = "colo-enable",
     .handler = cmdColoEnable,
     .opts = opts_colo_enable,
     .info = info_colo_enable,
     .flags = 0
    },
    {.name = "colo-disable",
     .handler = cmdColoDisable,
     .opts = opts_colo_disable,
     .info = info_colo_disable,
     .flags = 0
    },
    {.name = "colo-vm-status",
     .handler = cmdColoVMStatus,
     .opts = opts_colo_vm_status,
     .info = info_colo_vm_status,
     .flags = 0
    },
    {.name = "colo-set-params",
     .handler = cmdColoSetParams,
     .opts = opts_colo_set_params,
     .info = info_colo_set_params,
     .flags = 0
    },
    {.name = "colo-do-failover",
     .handler = cmdColoDoFailover,
     .opts = opts_colo_do_failover,
     .info = info_colo_do_failover,
     .flags = 0
    },
    {.name = NULL}
};


