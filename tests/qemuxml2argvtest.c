#include <config.h>

#include <unistd.h>

#include <sys/types.h>
#include <fcntl.h>

#include "testutils.h"

#ifdef WITH_QEMU

# include "internal.h"
# include "viralloc.h"
# include "viridentity.h"
# include "qemu/qemu_capabilities.h"
# include "qemu/qemu_domain.h"
# include "qemu/qemu_migration.h"
# include "qemu/qemu_process.h"
# include "qemu/qemu_slirp.h"
# include "datatypes.h"
# include "conf/storage_conf.h"
# include "virfilewrapper.h"
# include "configmake.h"
# include "testutilsqemuschema.h"

# define LIBVIRT_QEMU_CAPSPRIV_H_ALLOW
# include "qemu/qemu_capspriv.h"

# include "testutilsqemu.h"

# define VIR_FROM_THIS VIR_FROM_QEMU

static virQEMUDriver driver;

static unsigned char *
fakeSecretGetValue(virSecretPtr obj G_GNUC_UNUSED,
                   size_t *value_size,
                   unsigned int fakeflags G_GNUC_UNUSED)
{
    char *secret;
    secret = g_strdup("AQCVn5hO6HzFAhAAq0NCv8jtJcIcE+HOBlMQ1A");
    *value_size = strlen(secret);
    return (unsigned char *) secret;
}

static virSecretPtr
fakeSecretLookupByUsage(virConnectPtr conn,
                        int usageType,
                        const char *usageID)
{
    unsigned char uuid[VIR_UUID_BUFLEN];
    if (usageType == VIR_SECRET_USAGE_TYPE_VOLUME) {
        if (!STRPREFIX(usageID, "/storage/guest_disks/")) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "test provided invalid volume storage prefix '%s'",
                           usageID);
            return NULL;
        }
    } else if (STRNEQ(usageID, "mycluster_myname") &&
               STRNEQ(usageID, "client.admin secret")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "test provided incorrect usage '%s'", usageID);
        return NULL;
    }

    if (virUUIDGenerate(uuid) < 0)
        return NULL;

    return virGetSecret(conn, uuid, usageType, usageID);
}

static virSecretPtr
fakeSecretLookupByUUID(virConnectPtr conn,
                       const unsigned char *uuid)
{
    /* NB: This mocked value could be "tls" or "volume" depending on
     * which test is being run, we'll leave at NONE (or 0) */
    return virGetSecret(conn, uuid, VIR_SECRET_USAGE_TYPE_NONE, "");
}

static virSecretDriver fakeSecretDriver = {
    .connectNumOfSecrets = NULL,
    .connectListSecrets = NULL,
    .secretLookupByUUID = fakeSecretLookupByUUID,
    .secretLookupByUsage = fakeSecretLookupByUsage,
    .secretDefineXML = NULL,
    .secretGetXMLDesc = NULL,
    .secretSetValue = NULL,
    .secretGetValue = fakeSecretGetValue,
    .secretUndefine = NULL,
};


# define STORAGE_POOL_XML_PATH "storagepoolxml2xmlout/"
static const unsigned char fakeUUID[VIR_UUID_BUFLEN] = "fakeuuid";

static virStoragePoolPtr
fakeStoragePoolLookupByName(virConnectPtr conn,
                            const char *name)
{
    g_autofree char *xmlpath = NULL;

    if (STRNEQ(name, "inactive")) {
        xmlpath = g_strdup_printf("%s/%s%s.xml", abs_srcdir,
                                  STORAGE_POOL_XML_PATH, name);

        if (!virFileExists(xmlpath)) {
            virReportError(VIR_ERR_NO_STORAGE_POOL,
                           "File '%s' not found", xmlpath);
            return NULL;
        }
    }

    return virGetStoragePool(conn, name, fakeUUID, NULL, NULL);
}


static virStorageVolPtr
fakeStorageVolLookupByName(virStoragePoolPtr pool,
                           const char *name)
{
    g_auto(GStrv) volinfo = NULL;

    if (STREQ(pool->name, "inactive")) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "storage pool '%s' is not active", pool->name);
        return NULL;
    }

    if (STREQ(name, "nonexistent")) {
        virReportError(VIR_ERR_NO_STORAGE_VOL,
                       "no storage vol with matching name '%s'", name);
        return NULL;
    }

    if (!(volinfo = g_strsplit(name, "+", 2)))
        return NULL;

    if (!volinfo[1]) {
        return virGetStorageVol(pool->conn, pool->name, name, "block", NULL, NULL);
    }

    return virGetStorageVol(pool->conn, pool->name, volinfo[1], volinfo[0],
                           NULL, NULL);
}

static int
fakeStorageVolGetInfo(virStorageVolPtr vol,
                      virStorageVolInfoPtr info)
{
    memset(info, 0, sizeof(*info));

    info->type = virStorageVolTypeFromString(vol->key);

    if (info->type < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Invalid volume type '%s'", vol->key);
        return -1;
    }

    return 0;
}


static char *
fakeStorageVolGetPath(virStorageVolPtr vol)
{
    return g_strdup_printf("/some/%s/device/%s", vol->key, vol->name);
}


static char *
fakeStoragePoolGetXMLDesc(virStoragePoolPtr pool,
                          unsigned int flags_unused G_GNUC_UNUSED)
{
    g_autofree char *xmlpath = NULL;
    char *xmlbuf = NULL;

    if (STREQ(pool->name, "inactive")) {
        virReportError(VIR_ERR_NO_STORAGE_POOL, NULL);
        return NULL;
    }

    xmlpath = g_strdup_printf("%s/%s%s.xml", abs_srcdir, STORAGE_POOL_XML_PATH,
                              pool->name);

    if (virTestLoadFile(xmlpath, &xmlbuf) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "failed to load XML file '%s'",
                       xmlpath);
        return NULL;
    }

    return xmlbuf;
}

static int
fakeStoragePoolIsActive(virStoragePoolPtr pool)
{
    if (STREQ(pool->name, "inactive"))
        return 0;

    return 1;
}

/* Test storage pool implementation
 *
 * These functions aid testing of storage pool related stuff when creating a
 * qemu command line.
 *
 * There are a few "magic" values to pass to these functions:
 *
 * 1) "inactive" as a pool name to create an inactive pool. All other names are
 * interpreted as file names in storagepoolxml2xmlout/ and are used as the
 * definition for the pool. If the file doesn't exist the pool doesn't exist.
 *
 * 2) "nonexistent" returns an error while looking up a volume. Otherwise
 * pattern VOLUME_TYPE+VOLUME_PATH can be used to simulate a volume in a pool.
 * This creates a fake path for this volume. If the '+' sign is omitted, block
 * type is assumed.
 */
static virStorageDriver fakeStorageDriver = {
    .storagePoolLookupByName = fakeStoragePoolLookupByName,
    .storageVolLookupByName = fakeStorageVolLookupByName,
    .storagePoolGetXMLDesc = fakeStoragePoolGetXMLDesc,
    .storageVolGetPath = fakeStorageVolGetPath,
    .storageVolGetInfo = fakeStorageVolGetInfo,
    .storagePoolIsActive = fakeStoragePoolIsActive,
};


/* virNetDevOpenvswitchGetVhostuserIfname mocks a portdev name - handle that */
static virNWFilterBindingPtr
fakeNWFilterBindingLookupByPortDev(virConnectPtr conn,
                                   const char *portdev)
{
    if (STREQ(portdev, "vhost-user0"))
        return virGetNWFilterBinding(conn, "fake_vnet0", "fakeFilterName");

    virReportError(VIR_ERR_NO_NWFILTER_BINDING,
                   "no nwfilter binding for port dev '%s'", portdev);
    return NULL;
}


static int
fakeNWFilterBindingDelete(virNWFilterBindingPtr binding G_GNUC_UNUSED)
{
    return 0;
}


static virNWFilterDriver fakeNWFilterDriver = {
    .nwfilterBindingLookupByPortDev = fakeNWFilterBindingLookupByPortDev,
    .nwfilterBindingDelete = fakeNWFilterBindingDelete,
};


static int
testAddCPUModels(virQEMUCaps *caps, bool skipLegacy)
{
    virArch arch = virQEMUCapsGetArch(caps);
    const char *x86Models[] = {
        "Opteron_G3", "Opteron_G2", "Opteron_G1",
        "Nehalem", "Penryn", "Conroe",
        "Haswell-noTSX", "Haswell",
    };
    const char *x86LegacyModels[] = {
        "n270", "athlon", "pentium3", "pentium2", "pentium",
        "486", "coreduo", "kvm32", "qemu32", "kvm64",
        "core2duo", "phenom", "qemu64",
    };
    const char *armModels[] = {
        "cortex-a9", "cortex-a8", "cortex-a57", "cortex-a53",
    };
    const char *ppc64Models[] = {
        "POWER8", "POWER7",
    };
    const char *s390xModels[] = {
        "z990", "zEC12", "z13",
    };

    if (ARCH_IS_X86(arch)) {
        if (virQEMUCapsAddCPUDefinitions(caps, VIR_DOMAIN_VIRT_KVM, x86Models,
                                         G_N_ELEMENTS(x86Models),
                                         VIR_DOMCAPS_CPU_USABLE_UNKNOWN) < 0 ||
            virQEMUCapsAddCPUDefinitions(caps, VIR_DOMAIN_VIRT_QEMU, x86Models,
                                         G_N_ELEMENTS(x86Models),
                                         VIR_DOMCAPS_CPU_USABLE_UNKNOWN) < 0)
            return -1;

        if (!skipLegacy) {
            if (virQEMUCapsAddCPUDefinitions(caps, VIR_DOMAIN_VIRT_KVM,
                                             x86LegacyModels,
                                             G_N_ELEMENTS(x86LegacyModels),
                                             VIR_DOMCAPS_CPU_USABLE_UNKNOWN) < 0 ||
                virQEMUCapsAddCPUDefinitions(caps, VIR_DOMAIN_VIRT_QEMU,
                                             x86LegacyModels,
                                             G_N_ELEMENTS(x86LegacyModels),
                                             VIR_DOMCAPS_CPU_USABLE_UNKNOWN) < 0)
                return -1;
        }
    } else if (ARCH_IS_ARM(arch)) {
        if (virQEMUCapsAddCPUDefinitions(caps, VIR_DOMAIN_VIRT_KVM, armModels,
                                         G_N_ELEMENTS(armModels),
                                         VIR_DOMCAPS_CPU_USABLE_UNKNOWN) < 0 ||
            virQEMUCapsAddCPUDefinitions(caps, VIR_DOMAIN_VIRT_QEMU, armModels,
                                         G_N_ELEMENTS(armModels),
                                         VIR_DOMCAPS_CPU_USABLE_UNKNOWN) < 0)
            return -1;
    } else if (ARCH_IS_PPC64(arch)) {
        if (virQEMUCapsAddCPUDefinitions(caps, VIR_DOMAIN_VIRT_KVM, ppc64Models,
                                         G_N_ELEMENTS(ppc64Models),
                                         VIR_DOMCAPS_CPU_USABLE_UNKNOWN) < 0 ||
            virQEMUCapsAddCPUDefinitions(caps, VIR_DOMAIN_VIRT_QEMU, ppc64Models,
                                         G_N_ELEMENTS(ppc64Models),
                                         VIR_DOMCAPS_CPU_USABLE_UNKNOWN) < 0)
            return -1;
    } else if (ARCH_IS_S390(arch)) {
        if (virQEMUCapsAddCPUDefinitions(caps, VIR_DOMAIN_VIRT_KVM, s390xModels,
                                         G_N_ELEMENTS(s390xModels),
                                         VIR_DOMCAPS_CPU_USABLE_UNKNOWN) < 0)
            return -1;
    }

    return 0;
}

static void
testUpdateQEMUCapsHostCPUModel(virQEMUCaps *qemuCaps, virArch hostArch)
{
    virQEMUCapsUpdateHostCPUModel(qemuCaps, hostArch, VIR_DOMAIN_VIRT_KVM);
    virQEMUCapsUpdateHostCPUModel(qemuCaps, hostArch, VIR_DOMAIN_VIRT_QEMU);
}

static int
testUpdateQEMUCaps(const struct testQemuInfo *info,
                   virArch arch,
                   virCaps *caps)
{
    if (!caps)
        return -1;

    virQEMUCapsSetArch(info->qemuCaps, arch);

    if (testAddCPUModels(info->qemuCaps,
                         !!(info->flags & FLAG_SKIP_LEGACY_CPUS)) < 0)
        return -1;

    testUpdateQEMUCapsHostCPUModel(info->qemuCaps, caps->host.arch);

    return 0;
}


static int
testCheckExclusiveFlags(int flags)
{
    virCheckFlags(FLAG_EXPECT_FAILURE |
                  FLAG_EXPECT_PARSE_ERROR |
                  FLAG_FIPS_HOST |
                  FLAG_REAL_CAPS |
                  FLAG_SKIP_LEGACY_CPUS |
                  FLAG_SLIRP_HELPER |
                  0, -1);

    VIR_EXCLUSIVE_FLAGS_RET(FLAG_REAL_CAPS, FLAG_SKIP_LEGACY_CPUS, -1);
    return 0;
}


static virCommand *
testCompareXMLToArgvCreateArgs(virQEMUDriver *drv,
                               virDomainObj *vm,
                               const char *migrateURI,
                               struct testQemuInfo *info,
                               unsigned int flags)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    size_t i;

    drv->hostFips = flags & FLAG_FIPS_HOST;

    if (qemuProcessCreatePretendCmdPrepare(drv, vm, migrateURI,
                                           VIR_QEMU_PROCESS_START_COLD) < 0)
        return NULL;

    if (qemuDomainDeviceBackendChardevForeach(vm->def,
                                              testQemuPrepareHostBackendChardevOne,
                                              vm) < 0)
        return NULL;

    if (testQemuPrepareHostBackendChardevOne(NULL, priv->monConfig, vm) < 0)
        return NULL;

    for (i = 0; i < vm->def->ndisks; i++) {
        virDomainDiskDef *disk = vm->def->disks[i];

        /* host cdrom requires special treatment in qemu, mock it */
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM &&
            disk->src->format == VIR_STORAGE_FILE_RAW &&
            virStorageSourceIsBlockLocal(disk->src) &&
            STREQ(disk->src->path, "/dev/cdrom"))
            disk->src->hostcdrom = true;
    }

    if (vm->def->vsock) {
        virDomainVsockDef *vsock = vm->def->vsock;
        qemuDomainVsockPrivate *vsockPriv =
            (qemuDomainVsockPrivate *)vsock->privateData;

        if (vsock->auto_cid == VIR_TRISTATE_BOOL_YES)
            vsock->guest_cid = 42;

        vsockPriv->vhostfd = 6789;
    }

    for (i = 0; i < vm->def->ntpms; i++) {
        if (vm->def->tpms[i]->type != VIR_DOMAIN_TPM_TYPE_EMULATOR)
            continue;

        VIR_FREE(vm->def->tpms[i]->data.emulator.source->data.nix.path);
        vm->def->tpms[i]->data.emulator.source->type = VIR_DOMAIN_CHR_TYPE_UNIX;
        vm->def->tpms[i]->data.emulator.source->data.nix.path = g_strdup("/dev/test");
    }

    for (i = 0; i < vm->def->nvideos; i++) {
        virDomainVideoDef *video = vm->def->videos[i];

        if (video->backend == VIR_DOMAIN_VIDEO_BACKEND_TYPE_VHOSTUSER) {
            qemuDomainVideoPrivate *vpriv = QEMU_DOMAIN_VIDEO_PRIVATE(video);

            vpriv->vhost_user_fd = 1729;
        }
    }

    if (flags & FLAG_SLIRP_HELPER) {
        for (i = 0; i < vm->def->nnets; i++) {
            virDomainNetDef *net = vm->def->nets[i];

            if (net->type == VIR_DOMAIN_NET_TYPE_USER &&
                net->backend.type == VIR_DOMAIN_NET_BACKEND_DEFAULT &&
                virQEMUCapsGet(info->qemuCaps, QEMU_CAPS_DBUS_VMSTATE)) {
                qemuSlirp *slirp = qemuSlirpNew();
                slirp->fd[0] = 42;
                QEMU_DOMAIN_NETWORK_PRIVATE(net)->slirp = slirp;
            }
        }
    }

    return qemuProcessCreatePretendCmdBuild(vm, migrateURI);
}


struct testValidateSchemaCommandData {
    const char *name;
    const char *schema;
    bool allowIncomplete; /* relax validator for commands with incomplete schema */
};


static const struct testValidateSchemaCommandData commands[] = {
    { "-blockdev", "blockdev-add", false },
    { "-netdev", "netdev_add", false },
    { "-object", "object-add", false },
    { "-device", "device_add", true },
};

static int
testCompareXMLToArgvValidateSchemaCommand(GStrv args,
                                          GHashTable *schema)
{
    GStrv arg;

    for (arg = args; *arg; arg++) {
        const char *curcommand = *arg;
        const char *curargs = *(arg + 1);
        size_t i;

        for (i = 0; i < G_N_ELEMENTS(commands); i++) {
            const struct testValidateSchemaCommandData *command = commands + i;
            g_auto(virBuffer) debug = VIR_BUFFER_INITIALIZER;
            g_autoptr(virJSONValue) jsonargs = NULL;

            if (STRNEQ(curcommand, command->name))
                continue;

            if (!curargs) {
                VIR_TEST_VERBOSE("expected arguments for command '%s'",
                                 command->name);
                return -1;
            }

            if (*curargs != '{') {
                arg++;
                break;
            }

            if (!(jsonargs = virJSONValueFromString(curargs)))
                return -1;

            if (testQEMUSchemaValidateCommand(command->schema, jsonargs,
                                              schema, false, false,
                                              command->allowIncomplete,
                                              &debug) < 0) {
                VIR_TEST_VERBOSE("failed to validate '%s %s' against QAPI schema: %s",
                                 command->name, curargs, virBufferCurrentContent(&debug));
                return -1;
            }

            arg++;
        }
    }

    return 0;
}


static int
testCompareXMLToArgvValidateSchema(virCommand *cmd,
                                   struct testQemuInfo *info)
{
    g_auto(GStrv) args = NULL;

    if (!info->qmpSchema)
        return 0;

    if (virCommandGetArgList(cmd, &args) < 0)
        return -1;

    if (testCompareXMLToArgvValidateSchemaCommand(args, info->qmpSchema) < 0)
        return -1;

    return 0;
}


static int
testCompareXMLToArgv(const void *data)
{
    struct testQemuInfo *info = (void *) data;
    g_autofree char *migrateURI = NULL;
    g_auto(virBuffer) actualBuf = VIR_BUFFER_INITIALIZER;
    g_autofree char *actualargv = NULL;
    unsigned int flags = info->flags;
    unsigned int parseFlags = info->parseFlags;
    int ret = -1;
    virDomainObj *vm = NULL;
    virDomainChrSourceDef monitor_chr;
    g_autoptr(virConnect) conn = NULL;
    virError *err = NULL;
    g_autofree char *log = NULL;
    g_autoptr(virCommand) cmd = NULL;
    qemuDomainObjPrivate *priv = NULL;
    g_autoptr(xmlDoc) xml = NULL;
    g_autoptr(xmlXPathContext) ctxt = NULL;
    g_autofree char *archstr = NULL;
    virArch arch = VIR_ARCH_NONE;
    g_autoptr(virIdentity) sysident = virIdentityGetSystem();

    memset(&monitor_chr, 0, sizeof(monitor_chr));

    if (testQemuInfoInitArgs((struct testQemuInfo *) info) < 0)
        goto cleanup;

    if (info->arch != VIR_ARCH_NONE && info->arch != VIR_ARCH_X86_64)
        qemuTestSetHostArch(&driver, info->arch);

    if (info->args.capsHostCPUModel) {
        virCPUDef *hostCPUModel = qemuTestGetCPUDef(info->args.capsHostCPUModel);

        qemuTestSetHostCPU(&driver, driver.hostarch, hostCPUModel);
        testUpdateQEMUCapsHostCPUModel(info->qemuCaps, driver.hostarch);
    }

    if (!(conn = virGetConnect()))
        goto cleanup;

    conn->secretDriver = &fakeSecretDriver;
    conn->storageDriver = &fakeStorageDriver;
    conn->nwfilterDriver = &fakeNWFilterDriver;

    virSetConnectInterface(conn);
    virSetConnectNetwork(conn);
    virSetConnectNWFilter(conn);
    virSetConnectNodeDev(conn);
    virSetConnectSecret(conn);
    virSetConnectStorage(conn);

    if (virIdentitySetCurrent(sysident) < 0)
        goto cleanup;

    if (testCheckExclusiveFlags(info->flags) < 0)
        goto cleanup;

    if (!(xml = virXMLParse(info->infile, NULL, "(domain_definition)",
                            "domain", &ctxt, NULL, false)))
        goto cleanup;

    if ((archstr = virXPathString("string(./os/type[1]/@arch)", ctxt)))
        arch = virArchFromString(archstr);

    if (arch == VIR_ARCH_NONE)
        arch = virArchFromHost();

    if (!(info->flags & FLAG_REAL_CAPS)) {
        if (testUpdateQEMUCaps(info, arch, driver.caps) < 0)
            goto cleanup;
    }

    virFileCacheClear(driver.qemuCapsCache);

    if (qemuTestCapsCacheInsert(driver.qemuCapsCache, info->qemuCaps) < 0)
        goto cleanup;

    if (info->migrateFrom &&
        !(migrateURI = qemuMigrationDstGetURI(info->migrateFrom,
                                              info->migrateFd)))
        goto cleanup;

    if (!(vm = virDomainObjNew(driver.xmlopt)))
        goto cleanup;

    if (!virFileExists(info->infile)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Input file '%s' not found", info->infile);
        goto cleanup;
    }

    parseFlags |= VIR_DOMAIN_DEF_PARSE_INACTIVE;

    if (!(vm->def = virDomainDefParseNode(ctxt, driver.xmlopt, NULL, parseFlags))) {
        err = virGetLastError();
        if (!err) {
            VIR_TEST_DEBUG("no error was reported for expected parse error");
            goto cleanup;
        }
        if (flags & FLAG_EXPECT_PARSE_ERROR) {
            g_autofree char *tmperr = g_strdup_printf("%s\n", NULLSTR(err->message));
            if (virTestCompareToFile(tmperr, info->errfile) >= 0)
                goto ok;
        }
        goto cleanup;
    }
    if (flags & FLAG_EXPECT_PARSE_ERROR) {
        VIR_TEST_DEBUG("passed instead of expected parse error");
        goto cleanup;
    }
    priv = vm->privateData;

    if (info->args.fds) {
        g_clear_pointer(&priv->fds, g_hash_table_unref);
        priv->fds = g_steal_pointer(&info->args.fds);
    }

    if (virBitmapParse("0-3", &priv->autoNodeset, 4) < 0)
        goto cleanup;

    if (!virDomainDefCheckABIStability(vm->def, vm->def, driver.xmlopt)) {
        VIR_TEST_DEBUG("ABI stability check failed on %s", info->infile);
        goto cleanup;
    }

    vm->def->id = -1;

    if (qemuProcessPrepareMonitorChr(&monitor_chr, priv->libDir) < 0)
        goto cleanup;

    virResetLastError();

    if (!(cmd = testCompareXMLToArgvCreateArgs(&driver, vm, migrateURI, info,
                                               flags))) {
        err = virGetLastError();
        if (!err) {
            VIR_TEST_DEBUG("no error was reported for expected failure");
            goto cleanup;
        }
        if (flags & FLAG_EXPECT_FAILURE) {
            g_autofree char *tmperr = g_strdup_printf("%s\n", NULLSTR(err->message));
            if (virTestCompareToFile(tmperr, info->errfile) >= 0)
                goto ok;
        }
        goto cleanup;
    }
    if (flags & FLAG_EXPECT_FAILURE) {
        VIR_TEST_DEBUG("passed instead of expected failure");
        goto cleanup;
    }

    if (testCompareXMLToArgvValidateSchema(cmd, info) < 0)
        goto cleanup;

    if (virCommandToStringBuf(cmd, &actualBuf, true, false) < 0)
        goto cleanup;

    virBufferAddLit(&actualBuf, "\n");
    actualargv = virBufferContentAndReset(&actualBuf);

    if (virTestCompareToFileFull(actualargv, info->outfile, false) < 0)
        goto cleanup;

    ret = 0;

 ok:
    if (ret == 0 && flags & FLAG_EXPECT_FAILURE) {
        ret = -1;
        VIR_TEST_DEBUG("Error expected but there wasn't any.");
        goto cleanup;
    }
    if (flags & FLAG_EXPECT_FAILURE) {
        if ((log = virTestLogContentAndReset()))
            VIR_TEST_DEBUG("Got expected error: \n%s", log);
    }
    virResetLastError();
    ret = 0;

 cleanup:
    virDomainChrSourceDefClear(&monitor_chr);
    virObjectUnref(vm);
    virIdentitySetCurrent(NULL);
    virSetConnectSecret(NULL);
    virSetConnectStorage(NULL);
    if (info->arch != VIR_ARCH_NONE && info->arch != VIR_ARCH_X86_64)
        qemuTestSetHostArch(&driver, VIR_ARCH_NONE);

    return ret;
}

static void
testInfoSetPaths(struct testQemuInfo *info,
                 const char *suffix)
{
    info->infile = g_strdup_printf("%s/qemuxml2argvdata/%s.xml",
                                   abs_srcdir, info->name);
    info->outfile = g_strdup_printf("%s/qemuxml2argvdata/%s%s.args",
                                    abs_srcdir, info->name, suffix ? suffix : "");
    info->errfile = g_strdup_printf("%s/qemuxml2argvdata/%s%s.err",
                                      abs_srcdir, info->name, suffix ? suffix : "");
}

static int
mymain(void)
{
    int ret = 0;
    g_autoptr(GHashTable) capslatest = testQemuGetLatestCaps();
    g_autoptr(GHashTable) qapiSchemaCache = virHashNew((GDestroyNotify) g_hash_table_unref);
    g_autoptr(GHashTable) capscache = virHashNew(virObjectUnref);
    struct testQemuConf testConf = { .capslatest = capslatest,
                                     .capscache = capscache,
                                     .qapiSchemaCache = qapiSchemaCache };

    if (!capslatest)
        return EXIT_FAILURE;

    /* Set the timezone because we are mocking the time() function.
     * If we don't do that, then localtime() may return unpredictable
     * results. In order to detect things that just work by a blind
     * chance, we need to set an virtual timezone that no libvirt
     * developer resides in. */
    if (g_setenv("TZ", "VIR00:30", TRUE) == FALSE) {
        perror("g_setenv");
        return EXIT_FAILURE;
    }

    if (qemuTestDriverInit(&driver) < 0)
        return EXIT_FAILURE;

    virFileWrapperAddPrefix("/sys/devices/system",
                            abs_srcdir "/vircaps2xmldata/linux-basic/system");
    virFileWrapperAddPrefix(SYSCONFDIR "/qemu/firmware",
                            abs_srcdir "/qemufirmwaredata/etc/qemu/firmware");
    virFileWrapperAddPrefix(PREFIX "/share/qemu/firmware",
                            abs_srcdir "/qemufirmwaredata/usr/share/qemu/firmware");
    virFileWrapperAddPrefix("/home/user/.config/qemu/firmware",
                            abs_srcdir "/qemufirmwaredata/home/user/.config/qemu/firmware");

    virFileWrapperAddPrefix(SYSCONFDIR "/qemu/vhost-user",
                            abs_srcdir "/qemuvhostuserdata/etc/qemu/vhost-user");
    virFileWrapperAddPrefix(PREFIX "/share/qemu/vhost-user",
                            abs_srcdir "/qemuvhostuserdata/usr/share/qemu/vhost-user");
    virFileWrapperAddPrefix("/home/user/.config/qemu/vhost-user",
                            abs_srcdir "/qemuvhostuserdata/home/user/.config/qemu/vhost-user");

    virFileWrapperAddPrefix("/usr/libexec/qemu/vhost-user",
                            abs_srcdir "/qemuvhostuserdata/usr/libexec/qemu/vhost-user");

/**
 * The following set of macros allows testing of XML -> argv conversion with a
 * real set of capabilities gathered from a real qemu copy. It is desired to use
 * these for positive test cases as it provides combinations of flags which
 * can be met in real life.
 *
 * The capabilities are taken from the real capabilities stored in
 * tests/qemucapabilitiesdata.
 *
 * It is suggested to use the DO_TEST_CAPS_LATEST macro which always takes the
 * most recent capability set. In cases when the new code would change behaviour
 * the test cases should be forked using DO_TEST_CAPS_VER with the appropriate
 * version.
 */
# define DO_TEST_FULL(_name, _suffix, ...) \
    do { \
        static struct testQemuInfo info = { \
            .name = _name, \
        }; \
        testQemuInfoSetArgs(&info, &testConf, __VA_ARGS__); \
        testInfoSetPaths(&info, _suffix); \
        virTestRunLog(&ret, "QEMU XML-2-ARGV " _name _suffix, testCompareXMLToArgv, &info); \
        testQemuInfoClear(&info); \
    } while (0)

# define DO_TEST_CAPS_INTERNAL(name, arch, ver, ...) \
    DO_TEST_FULL(name, "." arch "-" ver, \
                 ARG_CAPS_ARCH, arch, \
                 ARG_CAPS_VER, ver, \
                 __VA_ARGS__, \
                 ARG_END)

# define DO_TEST_CAPS_ARCH_LATEST_FULL(name, arch, ...) \
    DO_TEST_CAPS_INTERNAL(name, arch, "latest", __VA_ARGS__)

# define DO_TEST_CAPS_ARCH_VER_FULL(name, arch, ver, ...) \
    DO_TEST_CAPS_INTERNAL(name, arch, ver, __VA_ARGS__)

# define DO_TEST_CAPS_ARCH_LATEST(name, arch) \
    DO_TEST_CAPS_ARCH_LATEST_FULL(name, arch, ARG_END)

# define DO_TEST_CAPS_ARCH_LATEST_ABI_UPDATE(name, arch) \
    DO_TEST_CAPS_ARCH_LATEST_FULL(name, arch, \
                                  ARG_PARSEFLAGS, VIR_DOMAIN_DEF_PARSE_ABI_UPDATE, \
                                  ARG_END)

# define DO_TEST_CAPS_ARCH_VER(name, arch, ver) \
    DO_TEST_CAPS_ARCH_VER_FULL(name, arch, ver, ARG_END)

# define DO_TEST_CAPS_LATEST(name) \
    DO_TEST_CAPS_ARCH_LATEST(name, "x86_64")

# define DO_TEST_CAPS_VER(name, ver) \
    DO_TEST_CAPS_ARCH_VER(name, "x86_64", ver)

# define DO_TEST_CAPS_LATEST_PPC64(name) \
    DO_TEST_CAPS_ARCH_LATEST(name, "ppc64")

# define DO_TEST_CAPS_LATEST_PPC64_HOSTCPU(name, hostcpu) \
    DO_TEST_CAPS_ARCH_LATEST_FULL(name, "ppc64", \
                                  ARG_CAPS_HOST_CPU_MODEL, hostcpu)

# define DO_TEST_CAPS_LATEST_PPC64_HOSTCPU_FAILURE(name, hostcpu) \
    DO_TEST_CAPS_ARCH_LATEST_FULL(name, "ppc64", \
                                  ARG_CAPS_HOST_CPU_MODEL, hostcpu, \
                                  ARG_FLAGS, FLAG_EXPECT_FAILURE)

# define DO_TEST_CAPS_ARCH_LATEST_FAILURE(name, arch) \
    DO_TEST_CAPS_ARCH_LATEST_FULL(name, arch, \
                                  ARG_FLAGS, FLAG_EXPECT_FAILURE)

# define DO_TEST_CAPS_ARCH_VER_FAILURE(name, arch, ver) \
    DO_TEST_CAPS_ARCH_VER_FULL(name, arch, ver, \
                               ARG_FLAGS, FLAG_EXPECT_FAILURE)

# define DO_TEST_CAPS_LATEST_FAILURE(name) \
    DO_TEST_CAPS_ARCH_LATEST_FAILURE(name, "x86_64")

# define DO_TEST_CAPS_VER_FAILURE(name, ver) \
    DO_TEST_CAPS_ARCH_VER_FAILURE(name, "x86_64", ver)

# define DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR(name, arch) \
    DO_TEST_CAPS_ARCH_LATEST_FULL(name, arch, \
                                  ARG_FLAGS, FLAG_EXPECT_PARSE_ERROR)

# define DO_TEST_CAPS_ARCH_VER_PARSE_ERROR(name, arch, ver) \
    DO_TEST_CAPS_ARCH_VER_FULL(name, arch, ver, \
                               ARG_FLAGS, FLAG_EXPECT_PARSE_ERROR)

# define DO_TEST_CAPS_LATEST_PARSE_ERROR(name) \
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR(name, "x86_64")

# define DO_TEST_CAPS_VER_PARSE_ERROR(name, ver) \
    DO_TEST_CAPS_ARCH_VER_PARSE_ERROR(name, "x86_64", ver)

/* All the following macros require an explicit QEMU_CAPS_* list
 * at the end of the argument list, or the NONE placeholder.
 * */
# define DO_TEST(name, ...) \
    DO_TEST_FULL(name, "", ARG_QEMU_CAPS, __VA_ARGS__, QEMU_CAPS_LAST, ARG_END)
# define DO_TEST_NOCAPS(name) \
    DO_TEST_FULL(name, "",  ARG_END)

/* GIC tests use 'latest' capabilities */
# define DO_TEST_GIC(name, gic) \
    DO_TEST_FULL(name, "", \
                 ARG_GIC, gic, \
                 ARG_CAPS_ARCH, "aarch64", \
                 ARG_CAPS_VER, "latest", \
                 ARG_END)

# define DO_TEST_FAILURE(name, ...) \
    DO_TEST_FULL(name, "", \
                 ARG_FLAGS, FLAG_EXPECT_FAILURE, \
                 ARG_QEMU_CAPS, __VA_ARGS__, QEMU_CAPS_LAST, ARG_END)
# define DO_TEST_FAILURE_NOCAPS(name) \
    DO_TEST_FULL(name, "", ARG_FLAGS, FLAG_EXPECT_FAILURE, ARG_END)

# define DO_TEST_PARSE_ERROR(name, ...) \
    DO_TEST_FULL(name, "", \
                 ARG_FLAGS, FLAG_EXPECT_PARSE_ERROR | FLAG_EXPECT_FAILURE, \
                 ARG_QEMU_CAPS, __VA_ARGS__, QEMU_CAPS_LAST, ARG_END)
# define DO_TEST_PARSE_ERROR_NOCAPS(name) \
    DO_TEST_FULL(name, "", \
                 ARG_FLAGS, FLAG_EXPECT_PARSE_ERROR | FLAG_EXPECT_FAILURE, \
                 ARG_END)

    /* Unset or set all envvars here that are copied in qemudBuildCommandLine
     * using ADD_ENV_COPY, otherwise these tests may fail due to unexpected
     * values for these envvars */
    g_setenv("PATH", "/bin", TRUE);
    g_setenv("USER", "test", TRUE);
    g_setenv("LOGNAME", "test", TRUE);
    g_setenv("HOME", "/home/test", TRUE);
    g_setenv("LC_ALL", "C", TRUE);
    g_unsetenv("TMPDIR");
    g_unsetenv("LD_PRELOAD");
    g_unsetenv("LD_LIBRARY_PATH");
    g_unsetenv("DYLD_INSERT_LIBRARIES");
    g_unsetenv("DYLD_FORCE_FLAT_NAMESPACE");
    g_unsetenv("QEMU_AUDIO_DRV");
    g_unsetenv("SDL_AUDIODRIVER");

    DO_TEST_NOCAPS("minimal");
    DO_TEST_PARSE_ERROR_NOCAPS("minimal-no-memory");

    DO_TEST_CAPS_LATEST("genid");
    DO_TEST_CAPS_LATEST("genid-auto");

    DO_TEST_NOCAPS("machine-aliases1");
    DO_TEST("machine-aliases2", QEMU_CAPS_KVM);
    DO_TEST_NOCAPS("machine-core-on");
    driver.config->dumpGuestCore = true;
    DO_TEST_NOCAPS("machine-core-off");
    driver.config->dumpGuestCore = false;
    DO_TEST_CAPS_LATEST("machine-smm-on");
    DO_TEST_CAPS_LATEST("machine-smm-off");
    DO_TEST("machine-vmport-opt",
            QEMU_CAPS_MACHINE_VMPORT_OPT);
    DO_TEST_NOCAPS("default-kvm-host-arch");
    DO_TEST_NOCAPS("default-qemu-host-arch");
    DO_TEST_CAPS_LATEST("x86-kvm-32-on-64");
    DO_TEST_CAPS_LATEST("boot-cdrom");
    DO_TEST_CAPS_LATEST("boot-network");
    DO_TEST_CAPS_LATEST("boot-floppy");
    DO_TEST_CAPS_LATEST("boot-floppy-q35");
    DO_TEST_CAPS_LATEST("boot-multi");
    DO_TEST_CAPS_LATEST("boot-menu-enable");
    DO_TEST_CAPS_LATEST("boot-menu-enable-with-timeout");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("boot-menu-enable-with-timeout-invalid");
    DO_TEST_CAPS_LATEST("boot-menu-disable");
    DO_TEST_CAPS_LATEST("boot-menu-disable-drive");

    DO_TEST_CAPS_LATEST_PARSE_ERROR("boot-dev+order");
    DO_TEST_CAPS_LATEST("boot-order");
    DO_TEST_CAPS_LATEST("boot-complex");

    DO_TEST_CAPS_LATEST("audio-none-minimal");
    DO_TEST_CAPS_LATEST("audio-alsa-minimal");
    DO_TEST_CAPS_LATEST("audio-coreaudio-minimal");
    DO_TEST_CAPS_LATEST("audio-jack-minimal");
    DO_TEST_CAPS_LATEST("audio-oss-minimal");
    DO_TEST_CAPS_LATEST("audio-pulseaudio-minimal");
    DO_TEST_CAPS_LATEST("audio-sdl-minimal");
    DO_TEST_CAPS_LATEST("audio-spice-minimal");
    DO_TEST_CAPS_LATEST("audio-file-minimal");

    DO_TEST_CAPS_LATEST("audio-none-best");
    DO_TEST_CAPS_LATEST("audio-alsa-best");
    DO_TEST_CAPS_LATEST("audio-coreaudio-best");
    DO_TEST_CAPS_LATEST("audio-oss-best");
    DO_TEST_CAPS_LATEST("audio-pulseaudio-best");
    DO_TEST_CAPS_LATEST("audio-sdl-best");
    DO_TEST_CAPS_LATEST("audio-spice-best");
    DO_TEST_CAPS_LATEST("audio-file-best");

    DO_TEST_CAPS_LATEST("audio-none-full");
    DO_TEST_CAPS_LATEST("audio-alsa-full");
    DO_TEST_CAPS_LATEST("audio-coreaudio-full");
    DO_TEST_CAPS_LATEST("audio-jack-full");
    DO_TEST_CAPS_LATEST("audio-oss-full");
    DO_TEST_CAPS_LATEST("audio-pulseaudio-full");
    DO_TEST_CAPS_LATEST("audio-sdl-full");
    DO_TEST_CAPS_LATEST("audio-spice-full");
    DO_TEST_CAPS_LATEST("audio-file-full");

    DO_TEST_CAPS_LATEST("audio-many-backends");

    /* Validate auto-creation of <audio> for legacy compat */
    g_setenv("QEMU_AUDIO_DRV", "sdl", TRUE);
    g_setenv("SDL_AUDIODRIVER", "esd", TRUE);
    DO_TEST_CAPS_VER("audio-default-sdl", "4.2.0");
    DO_TEST_CAPS_LATEST("audio-default-sdl");
    g_unsetenv("QEMU_AUDIO_DRV");
    g_unsetenv("SDL_AUDIODRIVER");

    g_setenv("QEMU_AUDIO_DRV", "alsa", TRUE);
    driver.config->vncAllowHostAudio = true;
    DO_TEST_CAPS_VER("audio-default-vnc", "4.2.0");
    DO_TEST_CAPS_LATEST("audio-default-vnc");
    driver.config->vncAllowHostAudio = false;
    g_unsetenv("QEMU_AUDIO_DRV");

    DO_TEST_CAPS_VER("audio-default-spice", "4.2.0");
    DO_TEST_CAPS_LATEST("audio-default-spice");

    g_setenv("QEMU_AUDIO_DRV", "alsa", TRUE);
    driver.config->nogfxAllowHostAudio = true;
    DO_TEST_CAPS_LATEST("audio-default-nographics");
    driver.config->nogfxAllowHostAudio = false;
    g_unsetenv("QEMU_AUDIO_DRV");

    DO_TEST_NOCAPS("reboot-timeout-disabled");
    DO_TEST_NOCAPS("reboot-timeout-enabled");

    DO_TEST_CAPS_LATEST("firmware-manual-bios");
    DO_TEST_CAPS_LATEST("firmware-manual-bios-stateless");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("firmware-manual-bios-not-stateless");
    DO_TEST_CAPS_LATEST("firmware-manual-efi");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-features");
    DO_TEST_CAPS_ARCH_LATEST_FULL("firmware-manual-efi-features", "x86_64",
                                  ARG_FLAGS, FLAG_EXPECT_PARSE_ERROR,
                                  ARG_PARSEFLAGS, VIR_DOMAIN_DEF_PARSE_ABI_UPDATE,
                                  ARG_END);
    DO_TEST_CAPS_LATEST("firmware-manual-efi-rw");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-rw-implicit");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-loader-secure");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("firmware-manual-efi-loader-no-path");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-secboot");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-no-enrolled-keys");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-no-secboot");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-stateless");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-nvram-template");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("firmware-manual-efi-nvram-template-stateless");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-nvram-network-iscsi");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-nvram-network-nbd");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-nvram-file");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("firmware-manual-efi-nvram-stateless");

    /* Make sure all combinations of ACPI and UEFI behave as expected */
    DO_TEST_CAPS_ARCH_LATEST("firmware-manual-efi-acpi-aarch64", "aarch64");
    DO_TEST_CAPS_LATEST("firmware-manual-efi-acpi-q35");
    DO_TEST_CAPS_ARCH_LATEST("firmware-manual-efi-noacpi-aarch64", "aarch64");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("firmware-manual-efi-noacpi-q35");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("firmware-manual-noefi-acpi-aarch64", "aarch64");
    DO_TEST_CAPS_LATEST("firmware-manual-noefi-acpi-q35");
    DO_TEST_CAPS_ARCH_LATEST("firmware-manual-noefi-noacpi-aarch64", "aarch64");
    DO_TEST_CAPS_LATEST("firmware-manual-noefi-noacpi-q35");

    DO_TEST_CAPS_LATEST("firmware-auto-bios");
    DO_TEST_CAPS_LATEST("firmware-auto-bios-stateless");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("firmware-auto-bios-not-stateless");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("firmware-auto-bios-nvram");
    DO_TEST_CAPS_LATEST("firmware-auto-efi");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-stateless");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-nvram");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-loader-secure");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-loader-insecure");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-loader-path");
    DO_TEST_CAPS_LATEST_FAILURE("firmware-auto-efi-loader-path-nonstandard");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-secboot");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-no-secboot");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-enrolled-keys");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-no-enrolled-keys");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("firmware-auto-efi-enrolled-keys-no-secboot");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-smm-off");
    DO_TEST_CAPS_ARCH_LATEST("firmware-auto-efi-aarch64", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST_ABI_UPDATE("firmware-auto-efi-abi-update-aarch64", "aarch64");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-nvram-file");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-nvram-network-nbd");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-nvram-network-iscsi");

    DO_TEST_CAPS_LATEST("firmware-auto-efi-format-loader-qcow2");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-format-nvram-qcow2");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-format-nvram-qcow2-path");
    DO_TEST_CAPS_LATEST("firmware-auto-efi-format-nvram-qcow2-network-nbd");
    DO_TEST_CAPS_ARCH_LATEST("firmware-auto-efi-format-loader-raw", "aarch64");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("firmware-auto-efi-format-mismatch");

    DO_TEST_NOCAPS("clock-utc");
    DO_TEST_NOCAPS("clock-localtime");
    DO_TEST_NOCAPS("clock-localtime-basis-localtime");
    DO_TEST_NOCAPS("clock-variable");
    DO_TEST_NOCAPS("clock-france");
    DO_TEST_NOCAPS("clock-hpet-off");
    DO_TEST("clock-catchup", QEMU_CAPS_KVM_PIT_TICK_POLICY);
    DO_TEST_NOCAPS("cpu-kvmclock");
    DO_TEST_NOCAPS("cpu-host-kvmclock");
    DO_TEST("kvmclock", QEMU_CAPS_KVM);
    DO_TEST("clock-timer-hyperv-rtc", QEMU_CAPS_KVM);
    DO_TEST_NOCAPS("clock-realtime");
    DO_TEST_CAPS_LATEST("clock-absolute");

    DO_TEST_CAPS_LATEST("controller-usb-order");

    DO_TEST("controller-order",
            QEMU_CAPS_KVM,
            QEMU_CAPS_PIIX3_USB_UHCI,
            QEMU_CAPS_CCID_PASSTHRU,
            QEMU_CAPS_SPICE,
            QEMU_CAPS_HDA_DUPLEX,
            QEMU_CAPS_USB_HUB,
            QEMU_CAPS_DEVICE_ISA_SERIAL,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    /* 'eoi' cpu feature with an explicit CPU defined */
    DO_TEST_CAPS_LATEST("cpu-eoi-disabled");
    DO_TEST_CAPS_LATEST("cpu-eoi-enabled");
    /* 'eoi' cpu feature without an explicit CPU defined */
    DO_TEST_CAPS_LATEST("eoi-disabled");
    DO_TEST_CAPS_LATEST("eoi-enabled");
    DO_TEST_CAPS_LATEST("pv-spinlock-disabled");
    DO_TEST_CAPS_LATEST("pv-spinlock-enabled");
    DO_TEST_CAPS_LATEST("kvmclock+eoi-disabled");

    DO_TEST_CAPS_LATEST("hyperv");
    DO_TEST_CAPS_LATEST("hyperv-off");
    DO_TEST_CAPS_LATEST("hyperv-panic");
    DO_TEST_CAPS_VER("hyperv-passthrough", "6.1.0");
    DO_TEST_CAPS_LATEST("hyperv-passthrough");
    DO_TEST_CAPS_LATEST("hyperv-stimer-direct");

    DO_TEST_NOCAPS("kvm-features");
    DO_TEST_NOCAPS("kvm-features-off");

    DO_TEST_NOCAPS("pmu-feature");
    DO_TEST_NOCAPS("pmu-feature-off");

    DO_TEST_CAPS_LATEST("pages-discard");
    DO_TEST_CAPS_LATEST("pages-discard-hugepages");
    DO_TEST_CAPS_LATEST("pages-dimm-discard");
    DO_TEST_CAPS_LATEST("hugepages-default");
    DO_TEST_CAPS_LATEST("hugepages-default-2M");
    DO_TEST_CAPS_LATEST("hugepages-default-system-size");
    DO_TEST_CAPS_LATEST_FAILURE("hugepages-default-5M");
    DO_TEST_PARSE_ERROR_NOCAPS("hugepages-default-1G-nodeset-2M");
    DO_TEST_CAPS_LATEST("hugepages-nodeset");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("hugepages-nodeset-nonexist");
    DO_TEST_CAPS_LATEST("hugepages-numa-default");
    DO_TEST_CAPS_LATEST("hugepages-numa-default-2M");
    DO_TEST_CAPS_LATEST("hugepages-numa-default-dimm");
    DO_TEST_CAPS_LATEST("hugepages-numa-nodeset");
    DO_TEST_CAPS_LATEST("hugepages-numa-nodeset-part");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("hugepages-numa-nodeset-nonexist");
    DO_TEST_CAPS_LATEST("hugepages-shared");
    DO_TEST_PARSE_ERROR_NOCAPS("hugepages-memaccess-invalid");
    DO_TEST_CAPS_LATEST("hugepages-memaccess");
    DO_TEST_CAPS_LATEST("hugepages-memaccess2");
    DO_TEST_PARSE_ERROR_NOCAPS("hugepages-memaccess3");
    DO_TEST_CAPS_LATEST("hugepages-memaccess3");
    DO_TEST_CAPS_LATEST("hugepages-nvdimm");
    DO_TEST_NOCAPS("nosharepages");

    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("non-x86_64-timer-error", "s390x");

    DO_TEST_CAPS_LATEST("disk-cdrom");
    DO_TEST_CAPS_LATEST("disk-cdrom-empty-network-invalid");
    DO_TEST_CAPS_LATEST("disk-cdrom-bus-other");
    DO_TEST_CAPS_LATEST("disk-cdrom-network");
    DO_TEST_CAPS_LATEST("disk-cdrom-tray");
    DO_TEST_CAPS_LATEST("disk-floppy");
    DO_TEST_CAPS_LATEST("disk-floppy-q35");
    DO_TEST_CAPS_ARCH_LATEST_FAILURE("disk-floppy-pseries", "ppc64");
    DO_TEST_CAPS_LATEST("disk-floppy-tray");
    DO_TEST_CAPS_LATEST("disk-virtio");
    DO_TEST_CAPS_ARCH_LATEST("disk-virtio-ccw", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("disk-virtio-ccw-many", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("disk-virtio-s390-zpci", "s390x");
    DO_TEST_CAPS_LATEST("disk-order");
    DO_TEST_CAPS_LATEST("disk-virtio-queues");
    DO_TEST_CAPS_LATEST("disk-boot-disk");
    DO_TEST_CAPS_LATEST("disk-boot-cdrom");
    DO_TEST_CAPS_LATEST("floppy-drive-fat");
    DO_TEST_CAPS_LATEST("disk-readonly-disk");
    DO_TEST_CAPS_LATEST("disk-fmt-qcow");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-fmt-cow");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-fmt-dir");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-fmt-iso");
    DO_TEST_CAPS_LATEST("disk-shared");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-shared-qcow");
    DO_TEST_CAPS_LATEST("disk-error-policy");
    DO_TEST_CAPS_ARCH_LATEST("disk-error-policy-s390x", "s390x");
    DO_TEST_CAPS_LATEST("disk-cache");
    DO_TEST_CAPS_LATEST("disk-metadata-cache");
    DO_TEST_CAPS_LATEST("disk-transient");
    DO_TEST_CAPS_LATEST("disk-network-nbd");
    DO_TEST_CAPS_LATEST("disk-network-iscsi");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-network-iscsi-auth-secrettype-invalid");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-network-iscsi-auth-wrong-secrettype");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-network-source-auth-both");
    DO_TEST_CAPS_LATEST("disk-network-gluster");
    DO_TEST_CAPS_LATEST("disk-network-rbd");
    DO_TEST_CAPS_VER_PARSE_ERROR("disk-network-rbd-encryption", "6.0.0");
    DO_TEST_CAPS_LATEST("disk-network-rbd-encryption");
    DO_TEST_CAPS_VER_PARSE_ERROR("disk-network-rbd-encryption-layering", "7.2.0");
    DO_TEST_CAPS_LATEST("disk-network-rbd-encryption-layering");
    DO_TEST_CAPS_VER_PARSE_ERROR("disk-network-rbd-encryption-luks-any", "7.2.0");
    DO_TEST_CAPS_LATEST("disk-network-rbd-encryption-luks-any");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-encryption-wrong");
    DO_TEST_CAPS_LATEST("disk-network-rbd-no-colon");
    /* qemu-6.0 is the last qemu version supporting sheepdog */
    DO_TEST_CAPS_VER("disk-network-sheepdog", "6.0.0");
    DO_TEST_CAPS_LATEST("disk-network-source-auth");
    DO_TEST_CAPS_LATEST("disk-network-nfs");
    driver.config->vxhsTLS = 1;
    driver.config->nbdTLSx509secretUUID = g_strdup("6fd3f62d-9fe7-4a4e-a869-7acd6376d8ea");
    driver.config->vxhsTLSx509secretUUID = g_strdup("6fd3f62d-9fe7-4a4e-a869-7acd6376d8ea");
    DO_TEST_CAPS_VER("disk-network-tlsx509-nbd", "5.2.0");
    DO_TEST_CAPS_LATEST("disk-network-tlsx509-nbd");
    DO_TEST_CAPS_VER_PARSE_ERROR("disk-network-tlsx509-nbd-hostname", "6.2.0");
    DO_TEST_CAPS_LATEST("disk-network-tlsx509-nbd-hostname");
    DO_TEST_CAPS_VER("disk-network-tlsx509-vxhs", "5.0.0");
    DO_TEST_CAPS_LATEST("disk-network-http");
    VIR_FREE(driver.config->nbdTLSx509secretUUID);
    VIR_FREE(driver.config->vxhsTLSx509secretUUID);
    driver.config->vxhsTLS = 0;
    DO_TEST_CAPS_LATEST("disk-no-boot");
    DO_TEST_CAPS_LATEST("disk-nvme");
    DO_TEST_CAPS_VER("disk-vhostuser-numa", "4.2.0");
    DO_TEST_CAPS_LATEST("disk-vhostuser-numa");
    DO_TEST_CAPS_LATEST("disk-vhostuser");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-device-lun-type-invalid");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-attaching-partition-nosupport");
    DO_TEST_CAPS_LATEST("disk-usb-device");
    DO_TEST_CAPS_LATEST("disk-device-removable");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-usb-pci");
    DO_TEST_CAPS_LATEST("disk-scsi");
    DO_TEST_CAPS_LATEST("disk-scsi-device-auto");
    DO_TEST_CAPS_LATEST("disk-scsi-disk-split");
    DO_TEST_CAPS_LATEST("disk-scsi-disk-wwn");
    DO_TEST_CAPS_LATEST("disk-scsi-disk-vpd");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-scsi-disk-vpd-build-error");
    DO_TEST_CAPS_LATEST("controller-virtio-scsi");
    DO_TEST_CAPS_LATEST("disk-sata-device");
    DO_TEST_CAPS_LATEST("disk-aio");
    DO_TEST_CAPS_LATEST("disk-aio-io_uring");
    DO_TEST_CAPS_LATEST("disk-source-pool");
    DO_TEST_CAPS_LATEST("disk-source-pool-mode");
    DO_TEST_CAPS_LATEST("disk-ioeventfd");
    DO_TEST_CAPS_LATEST("disk-copy_on_read");
    DO_TEST_CAPS_LATEST("disk-discard");
    DO_TEST_CAPS_LATEST("disk-detect-zeroes");
    DO_TEST_CAPS_LATEST("disk-discard_no_unref");
    DO_TEST_CAPS_LATEST("disk-snapshot");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-same-targets");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-missing-target-invalid");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-address-conflict");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-hostdev-scsi-address-conflict");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("hostdevs-drive-address-conflict");
    DO_TEST_CAPS_LATEST("event_idx");
    DO_TEST_CAPS_LATEST("virtio-lun");
    DO_TEST_CAPS_LATEST("disk-scsi-lun-passthrough");
    DO_TEST_CAPS_LATEST("disk-serial");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-fdc-incompatible-address");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-ide-incompatible-address");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-sata-incompatible-address");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("disk-scsi-incompatible-address");
    DO_TEST_CAPS_LATEST("disk-backing-chains-index");
    DO_TEST_CAPS_LATEST("disk-backing-chains-noindex");
    DO_TEST_CAPS_ARCH_LATEST_FULL("disk-source-fd", "x86_64",
                                  ARG_FD_GROUP, "testgroup2", 2, 700, 705,
                                  ARG_FD_GROUP, "testgroup5", 1, 704,
                                  ARG_FD_GROUP, "testgroup6", 2, 777, 778);

    DO_TEST_CAPS_LATEST("disk-slices");
    DO_TEST_CAPS_LATEST("disk-rotation");

    DO_TEST_CAPS_ARCH_LATEST("disk-arm-virtio-sd", "aarch64");

    DO_TEST_CAPS_LATEST("encrypted-disk");
    DO_TEST_CAPS_LATEST("encrypted-disk-usage");
    DO_TEST_CAPS_LATEST("luks-disks");
    DO_TEST_CAPS_LATEST("luks-disks-source");
    DO_TEST_CAPS_VER("luks-disks-source-qcow2", "5.2.0");
    DO_TEST_CAPS_LATEST("luks-disks-source-qcow2");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("luks-disk-invalid");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("luks-disks-source-both");

    DO_TEST_CAPS_LATEST("disk-ide-split");
    DO_TEST_CAPS_LATEST("disk-ide-wwn");
    DO_TEST_CAPS_LATEST("disk-geometry");
    DO_TEST_CAPS_LATEST("disk-blockio");

    DO_TEST_CAPS_VER("disk-virtio-scsi-reservations", "5.2.0");
    DO_TEST_CAPS_LATEST("disk-virtio-scsi-reservations");

    DO_TEST("graphics-egl-headless",
            QEMU_CAPS_DEVICE_CIRRUS_VGA,
            QEMU_CAPS_EGL_HEADLESS);
    DO_TEST_CAPS_LATEST("graphics-egl-headless");
    DO_TEST_CAPS_LATEST("graphics-egl-headless-rendernode");

    DO_TEST_CAPS_LATEST("graphics-vnc");
    DO_TEST_CAPS_LATEST("graphics-vnc-socket");
    DO_TEST_CAPS_LATEST("graphics-vnc-websocket");
    DO_TEST_CAPS_LATEST("graphics-vnc-policy");
    DO_TEST_CAPS_LATEST("graphics-vnc-power");
    DO_TEST_CAPS_LATEST("graphics-vnc-no-listen-attr");
    DO_TEST_CAPS_LATEST("graphics-vnc-remove-generated-socket");
    driver.config->vncAutoUnixSocket = true;
    DO_TEST_CAPS_LATEST("graphics-vnc-auto-socket-cfg");
    driver.config->vncAutoUnixSocket = false;
    DO_TEST_CAPS_LATEST("graphics-vnc-auto-socket");
    DO_TEST_CAPS_LATEST("graphics-vnc-none");
    DO_TEST_CAPS_LATEST("graphics-vnc-socket-new-cmdline");

    driver.config->vncSASL = 1;
    DO_TEST_CAPS_LATEST("graphics-vnc-sasl");
    driver.config->vncTLS = 1;
    driver.config->vncTLSx509verify = 1;
    DO_TEST_CAPS_LATEST("graphics-vnc-tls");
    driver.config->vncTLSx509secretUUID = g_strdup("6fd3f62d-9fe7-4a4e-a869-7acd6376d8ea");
    DO_TEST_CAPS_VER("graphics-vnc-tls-secret", "5.2.0");
    DO_TEST_CAPS_LATEST("graphics-vnc-tls-secret");
    VIR_FREE(driver.config->vncTLSx509secretUUID);
    driver.config->vncSASL = driver.config->vncTLSx509verify = driver.config->vncTLS = 0;
    DO_TEST_CAPS_LATEST("graphics-vnc-egl-headless");

    DO_TEST("graphics-sdl",
            QEMU_CAPS_DEVICE_VGA, QEMU_CAPS_SDL);
    DO_TEST_CAPS_LATEST_PARSE_ERROR("graphics-sdl-egl-headless");
    DO_TEST("graphics-sdl-fullscreen",
            QEMU_CAPS_DEVICE_CIRRUS_VGA, QEMU_CAPS_SDL);

    driver.config->spiceTLS = 1;
    DO_TEST_CAPS_LATEST("graphics-spice");
    DO_TEST_CAPS_LATEST("graphics-spice-no-args");
    driver.config->spiceSASL = 1;
    DO_TEST_CAPS_LATEST("graphics-spice-sasl");
    driver.config->spiceSASL = 0;
    DO_TEST_CAPS_LATEST("graphics-spice-agentmouse");
    DO_TEST_CAPS_LATEST("graphics-spice-compression");
    DO_TEST_CAPS_LATEST("graphics-spice-timeout");
    DO_TEST_CAPS_LATEST("graphics-spice-qxl-vga");
    DO_TEST_CAPS_LATEST("graphics-spice-usb-redir");
    DO_TEST_CAPS_LATEST("graphics-spice-agent-file-xfer");
    DO_TEST_CAPS_LATEST("graphics-spice-socket");
    DO_TEST_CAPS_LATEST("graphics-spice-auto-socket");
    driver.config->spiceAutoUnixSocket = true;
    DO_TEST_CAPS_LATEST("graphics-spice-auto-socket-cfg");
    driver.config->spiceAutoUnixSocket = false;
    DO_TEST_CAPS_LATEST("graphics-spice-egl-headless");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("graphics-spice-invalid-egl-headless");
    DO_TEST_CAPS_LATEST("graphics-spice-gl-auto-rendernode");
    driver.config->spiceTLS = 0;

    DO_TEST("graphics-dbus",
            QEMU_CAPS_DEVICE_CIRRUS_VGA, QEMU_CAPS_DISPLAY_DBUS);
    DO_TEST("graphics-dbus-address",
            QEMU_CAPS_DEVICE_CIRRUS_VGA, QEMU_CAPS_DISPLAY_DBUS);
    DO_TEST("graphics-dbus-p2p",
            QEMU_CAPS_DEVICE_CIRRUS_VGA, QEMU_CAPS_DISPLAY_DBUS);
    DO_TEST("graphics-dbus-audio",
            QEMU_CAPS_DEVICE_CIRRUS_VGA, QEMU_CAPS_DISPLAY_DBUS);
    DO_TEST("graphics-dbus-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL,
            QEMU_CAPS_DEVICE_CIRRUS_VGA,
            QEMU_CAPS_DISPLAY_DBUS);
    DO_TEST("graphics-dbus-usbredir",
            QEMU_CAPS_DEVICE_CIRRUS_VGA,
            QEMU_CAPS_DISPLAY_DBUS,
            QEMU_CAPS_USB_REDIR);

    DO_TEST_NOCAPS("input-usbmouse");
    DO_TEST_NOCAPS("input-usbtablet");
    DO_TEST_NOCAPS("misc-acpi");
    DO_TEST("misc-disable-s3", QEMU_CAPS_PIIX_DISABLE_S3);
    DO_TEST("misc-disable-suspends", QEMU_CAPS_PIIX_DISABLE_S3, QEMU_CAPS_PIIX_DISABLE_S4);
    DO_TEST("misc-enable-s4", QEMU_CAPS_PIIX_DISABLE_S4);
    DO_TEST_PARSE_ERROR_NOCAPS("misc-enable-s4");
    DO_TEST_CAPS_VER("misc-no-reboot", "5.2.0");
    DO_TEST_CAPS_LATEST("misc-no-reboot");
    DO_TEST_NOCAPS("misc-uuid");
    DO_TEST_PARSE_ERROR_NOCAPS("vhost_queues-invalid");
    DO_TEST_NOCAPS("net-vhostuser");
    DO_TEST_CAPS_LATEST("net-vhostuser");
    DO_TEST_NOCAPS("net-vhostuser-multiq");
    DO_TEST_FAILURE_NOCAPS("net-vhostuser-fail");
    DO_TEST_NOCAPS("net-user");
    DO_TEST_CAPS_ARCH_LATEST_FULL("net-user", "x86_64", ARG_FLAGS, FLAG_SLIRP_HELPER);
    DO_TEST_NOCAPS("net-user-addr");
    DO_TEST_CAPS_LATEST("net-user-passt");
    DO_TEST_CAPS_VER("net-user-passt", "7.2.0");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("net-user-slirp-portforward");
    DO_TEST_NOCAPS("net-virtio");
    DO_TEST_NOCAPS("net-virtio-device");
    DO_TEST_NOCAPS("net-virtio-disable-offloads");
    DO_TEST_NOCAPS("net-virtio-netdev");
    DO_TEST_CAPS_ARCH_LATEST("net-virtio-ccw", "s390x");
    DO_TEST_NOCAPS("net-virtio-rxtxqueuesize");
    DO_TEST_PARSE_ERROR_NOCAPS("net-virtio-rxqueuesize-invalid-size");
    DO_TEST("net-virtio-teaming",
            QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST("net-virtio-teaming-hostdev",
            QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST_NOCAPS("net-eth");
    DO_TEST_NOCAPS("net-eth-ifname");
    DO_TEST_NOCAPS("net-eth-names");
    DO_TEST_NOCAPS("net-eth-hostip");
    DO_TEST_NOCAPS("net-eth-unmanaged-tap");
    DO_TEST_NOCAPS("net-client");
    DO_TEST_NOCAPS("net-server");
    DO_TEST_NOCAPS("net-many-models");
    DO_TEST_NOCAPS("net-mcast");
    DO_TEST_NOCAPS("net-udp");
    DO_TEST("net-hostdev", QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST("net-hostdev-bootorder", QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST("net-hostdev-multidomain", QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST("net-hostdev-vfio",
            QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST("net-hostdev-vfio-multidomain",
            QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST_FAILURE("net-hostdev-fail",
                    QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST_CAPS_LATEST("net-vdpa");
    DO_TEST_CAPS_LATEST("net-vdpa-multiqueue");
    DO_TEST_CAPS_LATEST("net-virtio-rss");

    DO_TEST("hostdev-pci-multifunction",
            QEMU_CAPS_KVM,
            QEMU_CAPS_DEVICE_VFIO_PCI);

    DO_TEST("hostdev-pci-address-unassigned",
            QEMU_CAPS_KVM,
            QEMU_CAPS_DEVICE_VFIO_PCI);

    DO_TEST("serial-file-log",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    driver.config->spiceTLS = 1;
    DO_TEST("serial-spiceport",
            QEMU_CAPS_DEVICE_QXL,
            QEMU_CAPS_SPICE,
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    driver.config->spiceTLS = 0;

    DO_TEST("console-compat",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST("console-compat-auto",
            QEMU_CAPS_DEVICE_ISA_SERIAL);

    DO_TEST("serial-vc-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST("serial-pty-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST("serial-dev-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST("serial-dev-chardev-iobase",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST("serial-file-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST("serial-unix-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST_CAPS_LATEST("serial-file-log");
    driver.config->spiceTLS = 1;
    DO_TEST_CAPS_LATEST("serial-spiceport");
    driver.config->spiceTLS = 0;
    DO_TEST_CAPS_LATEST("serial-debugcon");

    DO_TEST_CAPS_LATEST("console-compat");
    DO_TEST_CAPS_LATEST("console-compat-auto");

    DO_TEST_CAPS_LATEST("serial-vc-chardev");
    DO_TEST_CAPS_LATEST("serial-pty-chardev");
    DO_TEST_CAPS_LATEST("serial-dev-chardev");
    DO_TEST_CAPS_LATEST("serial-dev-chardev-iobase");
    DO_TEST_CAPS_LATEST("serial-file-chardev");
    DO_TEST_CAPS_LATEST("serial-unix-chardev");
    DO_TEST("serial-tcp-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST("serial-udp-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST("serial-tcp-telnet-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST_CAPS_LATEST("serial-unix-chardev");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("serial-unix-missing-source");
    DO_TEST_CAPS_LATEST("serial-tcp-chardev");
    DO_TEST_CAPS_LATEST("serial-udp-chardev");
    DO_TEST_CAPS_LATEST("serial-tcp-telnet-chardev");
    driver.config->chardevTLS = 1;
    DO_TEST("serial-tcp-tlsx509-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST_CAPS_LATEST("serial-tcp-tlsx509-chardev");
    driver.config->chardevTLSx509verify = 1;
    DO_TEST("serial-tcp-tlsx509-chardev-verify",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST_CAPS_LATEST("serial-tcp-tlsx509-chardev-verify");
    driver.config->chardevTLSx509verify = 0;
    DO_TEST("serial-tcp-tlsx509-chardev-notls",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST_CAPS_LATEST("serial-tcp-tlsx509-chardev-notls");
    driver.config->chardevTLSx509secretUUID = g_strdup("6fd3f62d-9fe7-4a4e-a869-7acd6376d8ea");
    DO_TEST("serial-tcp-tlsx509-secret-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST_CAPS_LATEST("serial-tcp-tlsx509-secret-chardev");
    VIR_FREE(driver.config->chardevTLSx509secretUUID);
    driver.config->chardevTLS = 0;
    DO_TEST("serial-many-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST_NOCAPS("parallel-tcp-chardev");
    DO_TEST_NOCAPS("parallel-parport-chardev");
    DO_TEST_CAPS_LATEST("serial-many-chardev");
    DO_TEST_CAPS_LATEST("parallel-tcp-chardev");
    DO_TEST_CAPS_LATEST("parallel-parport-chardev");
    DO_TEST_CAPS_LATEST("parallel-unix-chardev");
    DO_TEST("console-compat-chardev",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST("pci-serial-dev-chardev",
            QEMU_CAPS_DEVICE_PCI_SERIAL);
    DO_TEST_CAPS_LATEST("console-compat-chardev");
    DO_TEST_CAPS_LATEST("pci-serial-dev-chardev");

    DO_TEST_NOCAPS("channel-guestfwd");
    DO_TEST_CAPS_LATEST("channel-unix-guestfwd");
    DO_TEST_NOCAPS("channel-virtio");
    DO_TEST_NOCAPS("channel-virtio-state");
    DO_TEST_NOCAPS("channel-virtio-auto");
    DO_TEST_NOCAPS("channel-virtio-autoassign");
    DO_TEST_NOCAPS("channel-virtio-autoadd");
    DO_TEST_NOCAPS("console-virtio");
    DO_TEST("console-virtio-many",
            QEMU_CAPS_DEVICE_ISA_SERIAL);
    DO_TEST_CAPS_ARCH_LATEST("console-virtio-ccw", "s390x");
    DO_TEST_CAPS_LATEST("console-virtio-unix");
    DO_TEST_CAPS_ARCH_LATEST("console-sclp", "s390x");
    driver.config->spiceTLS = 1;
    DO_TEST("channel-spicevmc",
            QEMU_CAPS_SPICE,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    driver.config->spiceTLS = 0;
    DO_TEST_CAPS_LATEST("channel-qemu-vdagent");
    DO_TEST_CAPS_LATEST("channel-qemu-vdagent-features");
    DO_TEST("channel-virtio-default",
            QEMU_CAPS_SPICE,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST_NOCAPS("channel-virtio-unix");

    DO_TEST("smartcard-host",
            QEMU_CAPS_CCID_EMULATED);
    DO_TEST("smartcard-host-certificates",
            QEMU_CAPS_CCID_EMULATED);
    DO_TEST("smartcard-host-certificates-database",
            QEMU_CAPS_CCID_EMULATED);
    DO_TEST("smartcard-passthrough-tcp",
            QEMU_CAPS_CCID_PASSTHRU);
    DO_TEST("smartcard-passthrough-spicevmc",
            QEMU_CAPS_CCID_PASSTHRU,
            QEMU_CAPS_SPICE,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST("smartcard-controller",
            QEMU_CAPS_CCID_EMULATED);
    DO_TEST_CAPS_LATEST("smartcard-passthrough-unix");

    DO_TEST("chardev-reconnect",
            QEMU_CAPS_CHARDEV_RECONNECT,
            QEMU_CAPS_USB_REDIR,
            QEMU_CAPS_DEVICE_VIRTIO_RNG,
            QEMU_CAPS_OBJECT_RNG_EGD,
            QEMU_CAPS_CCID_PASSTHRU);
    DO_TEST_PARSE_ERROR("chardev-reconnect-invalid-timeout",
                        QEMU_CAPS_CHARDEV_RECONNECT);
    DO_TEST_PARSE_ERROR("chardev-reconnect-generated-path",
                        QEMU_CAPS_CHARDEV_RECONNECT);

    DO_TEST_NOCAPS("usb-controller");
    DO_TEST("usb-piix3-controller",
            QEMU_CAPS_PIIX3_USB_UHCI);
    DO_TEST("usb-ich9-ehci-addr",
            QEMU_CAPS_ICH9_USB_EHCI1);
    DO_TEST_NOCAPS("input-usbmouse-addr");
    DO_TEST("usb-ich9-companion",
            QEMU_CAPS_ICH9_USB_EHCI1);
    DO_TEST_PARSE_ERROR("usb-ich9-no-companion",
            QEMU_CAPS_ICH9_USB_EHCI1);
    DO_TEST("usb-ich9-autoassign",
            QEMU_CAPS_ICH9_USB_EHCI1,
            QEMU_CAPS_USB_HUB);
    DO_TEST("usb-hub",
            QEMU_CAPS_USB_HUB);
    DO_TEST("usb-hub-autoadd",
            QEMU_CAPS_USB_HUB);
    DO_TEST("usb-hub-autoadd-deluxe",
            QEMU_CAPS_USB_HUB);
    DO_TEST_PARSE_ERROR("usb-hub-conflict",
            QEMU_CAPS_USB_HUB);
    DO_TEST_PARSE_ERROR("usb-hub-nonexistent",
            QEMU_CAPS_USB_HUB);
    DO_TEST("usb-port-missing",
            QEMU_CAPS_USB_HUB);
    DO_TEST_FAILURE("usb-bus-missing",
                    QEMU_CAPS_USB_HUB);
    DO_TEST("usb-ports",
            QEMU_CAPS_USB_HUB);
    DO_TEST_PARSE_ERROR("usb-ports-out-of-range",
            QEMU_CAPS_USB_HUB);
    DO_TEST("usb-port-autoassign",
            QEMU_CAPS_USB_HUB);
    DO_TEST("usb-redir",
            QEMU_CAPS_USB_HUB,
            QEMU_CAPS_ICH9_USB_EHCI1,
            QEMU_CAPS_USB_REDIR,
            QEMU_CAPS_SPICE,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST("usb-redir-boot",
            QEMU_CAPS_USB_HUB,
            QEMU_CAPS_ICH9_USB_EHCI1,
            QEMU_CAPS_USB_REDIR,
            QEMU_CAPS_SPICE,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST("usb-redir-filter",
            QEMU_CAPS_USB_HUB,
            QEMU_CAPS_ICH9_USB_EHCI1,
            QEMU_CAPS_USB_REDIR,
            QEMU_CAPS_SPICE,
            QEMU_CAPS_USB_REDIR_FILTER,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST("usb-redir-filter-version",
            QEMU_CAPS_USB_REDIR,
            QEMU_CAPS_SPICE,
            QEMU_CAPS_USB_REDIR_FILTER,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST_CAPS_LATEST("usb-redir-unix");
    DO_TEST("usb1-usb2",
            QEMU_CAPS_PIIX3_USB_UHCI,
            QEMU_CAPS_USB_HUB,
            QEMU_CAPS_ICH9_USB_EHCI1);
    DO_TEST_NOCAPS("usb-none");
    DO_TEST_PARSE_ERROR_NOCAPS("usb-none-other");
    DO_TEST_PARSE_ERROR("usb-none-hub",
            QEMU_CAPS_USB_HUB);
    DO_TEST_PARSE_ERROR_NOCAPS("usb-none-usbtablet");
    DO_TEST("usb-controller-default-q35",
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_PCI_OHCI,
            QEMU_CAPS_PIIX3_USB_UHCI,
            QEMU_CAPS_NEC_USB_XHCI);
    DO_TEST_FAILURE("usb-controller-default-unavailable-q35",
                    QEMU_CAPS_DEVICE_IOH3420,
                    QEMU_CAPS_PCI_OHCI,
                    QEMU_CAPS_NEC_USB_XHCI);
    DO_TEST("usb-controller-explicit-q35",
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_PCI_OHCI,
            QEMU_CAPS_PIIX3_USB_UHCI,
            QEMU_CAPS_NEC_USB_XHCI);
    DO_TEST_FAILURE("usb-controller-explicit-unavailable-q35",
                    QEMU_CAPS_DEVICE_IOH3420,
                    QEMU_CAPS_PCI_OHCI,
                    QEMU_CAPS_PIIX3_USB_UHCI);
    DO_TEST("usb-controller-xhci",
            QEMU_CAPS_PIIX3_USB_UHCI,
            QEMU_CAPS_NEC_USB_XHCI);
    DO_TEST("usb-xhci-autoassign",
            QEMU_CAPS_PIIX3_USB_UHCI,
            QEMU_CAPS_NEC_USB_XHCI,
            QEMU_CAPS_USB_HUB);
    DO_TEST_PARSE_ERROR("usb-controller-xhci-limit",
            QEMU_CAPS_PIIX3_USB_UHCI,
            QEMU_CAPS_NEC_USB_XHCI);
    DO_TEST("usb-controller-qemu-xhci", QEMU_CAPS_DEVICE_QEMU_XHCI);
    DO_TEST_FAILURE_NOCAPS("usb-controller-qemu-xhci-unavailable");
    DO_TEST_PARSE_ERROR("usb-controller-qemu-xhci-limit",
                        QEMU_CAPS_DEVICE_QEMU_XHCI);

    DO_TEST_NOCAPS("smbios");
    DO_TEST_PARSE_ERROR_NOCAPS("smbios-date");
    DO_TEST_PARSE_ERROR_NOCAPS("smbios-uuid-match");
    DO_TEST_NOCAPS("smbios-type-fwcfg");

    DO_TEST_CAPS_LATEST("watchdog");
    DO_TEST_CAPS_LATEST("watchdog-device");
    DO_TEST_CAPS_LATEST("watchdog-dump");
    DO_TEST_CAPS_LATEST("watchdog-injectnmi");
    DO_TEST_CAPS_LATEST("watchdog-q35-multiple");
    DO_TEST_CAPS_ARCH_LATEST("watchdog-diag288", "s390x");
    DO_TEST_NOCAPS("balloon-device");
    DO_TEST("balloon-device-deflate",
            QEMU_CAPS_VIRTIO_BALLOON_AUTODEFLATE);
    DO_TEST_CAPS_ARCH_LATEST("balloon-ccw-deflate", "s390x");
    DO_TEST_FULL("balloon-mmio-deflate", ".aarch64-latest",
                 ARG_CAPS_ARCH, "aarch64",
                 ARG_CAPS_VER, "latest",
                 ARG_QEMU_CAPS_DEL,
                 QEMU_CAPS_OBJECT_GPEX, QEMU_CAPS_DEVICE_PCI_BRIDGE,
                 QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE, QEMU_CAPS_DEVICE_IOH3420,
                 QEMU_CAPS_LAST, ARG_END);
    DO_TEST("balloon-device-deflate-off",
            QEMU_CAPS_VIRTIO_BALLOON_AUTODEFLATE);
    DO_TEST_NOCAPS("balloon-device-auto");
    DO_TEST_NOCAPS("balloon-device-period");
    DO_TEST_CAPS_VER("sound-device", "4.2.0");
    DO_TEST_CAPS_LATEST("sound-device");
    DO_TEST_CAPS_LATEST("fs9p");
    DO_TEST_CAPS_ARCH_LATEST("fs9p-ccw", "s390x");

    DO_TEST_CAPS_LATEST("hostdev-usb-address");
    DO_TEST_CAPS_LATEST("hostdev-usb-address-device");
    DO_TEST_CAPS_LATEST("hostdev-usb-address-device-boot");
    DO_TEST_PARSE_ERROR_NOCAPS("hostdev-usb-duplicate");
    DO_TEST_CAPS_LATEST("hostdev-pci-address");
    DO_TEST_CAPS_LATEST("hostdev-pci-address-device");
    DO_TEST_PARSE_ERROR("hostdev-pci-duplicate",
                        QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST("hostdev-vfio",
            QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST("hostdev-vfio-multidomain",
            QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST("hostdev-mdev-precreated",
            QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST_PARSE_ERROR("hostdev-mdev-src-address-invalid",
            QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST_PARSE_ERROR("hostdev-mdev-invalid-target-address",
            QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST_PARSE_ERROR("hostdev-mdev-duplicate",
                        QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST_CAPS_LATEST("hostdev-mdev-display-spice-opengl");
    DO_TEST_CAPS_LATEST("hostdev-mdev-display-spice-egl-headless");
    DO_TEST_CAPS_LATEST("hostdev-mdev-display-vnc");
    DO_TEST_CAPS_LATEST("hostdev-mdev-display-vnc-egl-headless");
    DO_TEST_PARSE_ERROR("hostdev-mdev-display-missing-graphics",
            QEMU_CAPS_DEVICE_VFIO_PCI,
            QEMU_CAPS_VFIO_PCI_DISPLAY);
    DO_TEST_CAPS_LATEST("hostdev-mdev-display-ramfb");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("hostdev-mdev-display-ramfb-multiple");
    DO_TEST_PARSE_ERROR("hostdev-vfio-zpci-wrong-arch",
                        QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST_CAPS_ARCH_LATEST("hostdev-vfio-zpci", "s390x");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("hostdev-vfio-zpci-invalid-uid-valid-fid", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("hostdev-vfio-zpci-multidomain-many", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("hostdev-vfio-zpci-autogenerate", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("hostdev-vfio-zpci-autogenerate-uids", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("hostdev-vfio-zpci-autogenerate-fids", "s390x");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("hostdev-vfio-zpci-uid-set-zero", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("hostdev-vfio-zpci-boundaries", "s390x");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("hostdev-vfio-zpci-duplicate", "s390x");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("hostdev-vfio-zpci-set-zero", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("hostdev-vfio-zpci-ccw-memballoon", "s390x");

    DO_TEST("pci-rom", QEMU_CAPS_DEVICE_VFIO_PCI);
    DO_TEST_NOCAPS("pci-rom-disabled");
    DO_TEST_NOCAPS("pci-rom-disabled-invalid");

    DO_TEST_CAPS_ARCH_LATEST("hostdev-subsys-mdev-vfio-ccw", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("hostdev-subsys-mdev-vfio-ccw-boot", "s390x");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("hostdev-subsys-mdev-vfio-ccw-duplicate-address", "s390x");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("hostdev-subsys-mdev-vfio-ccw-invalid-address", "s390x");

    DO_TEST_CAPS_ARCH_LATEST("hostdev-subsys-mdev-vfio-ap", "s390x");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("hostdev-subsys-mdev-vfio-ap-boot-fail", "s390x");

    DO_TEST_FULL("restore-v2", "",
                 ARG_MIGRATE_FROM, "exec:cat",
                 ARG_MIGRATE_FD, 7,
                 ARG_END);
    DO_TEST_FULL("restore-v2-fd", "",
                 ARG_MIGRATE_FROM, "stdio",
                 ARG_MIGRATE_FD, 7,
                 ARG_END);
    DO_TEST_FULL("restore-v2-fd", "",
                 ARG_MIGRATE_FROM, "fd:7",
                 ARG_MIGRATE_FD, 7, ARG_END);
    DO_TEST_FULL("migrate", "",
                 ARG_MIGRATE_FROM, "tcp:10.0.0.1:5000", ARG_END);

    DO_TEST_FULL("migrate-numa-unaligned", "",
                 ARG_MIGRATE_FROM, "stdio",
                 ARG_MIGRATE_FD, 7,
                 ARG_QEMU_CAPS,
                 QEMU_CAPS_LAST,
                 ARG_END);

    DO_TEST_CAPS_LATEST("qemu-ns");
    DO_TEST_NOCAPS("qemu-ns-no-env");
    DO_TEST_NOCAPS("qemu-ns-alt");

    DO_TEST_NOCAPS("smp");
    DO_TEST("smp-dies", QEMU_CAPS_SMP_DIES);

    DO_TEST_CAPS_LATEST("iothreads-ids");
    DO_TEST_CAPS_LATEST("iothreads-ids-partial");
    DO_TEST_CAPS_LATEST("iothreads-ids-pool-sizes");
    DO_TEST_CAPS_LATEST("iothreads-disk");
    DO_TEST_CAPS_ARCH_VER("iothreads-disk-virtio-ccw", "s390x", "4.2.0");
    DO_TEST_CAPS_VER("iothreads-virtio-scsi-pci", "5.2.0");
    DO_TEST_CAPS_LATEST("iothreads-virtio-scsi-pci");
    DO_TEST_CAPS_ARCH_LATEST("iothreads-virtio-scsi-ccw", "s390x");

    DO_TEST_NOCAPS("cpu-topology1");
    DO_TEST_NOCAPS("cpu-topology2");
    DO_TEST_NOCAPS("cpu-topology3");
    DO_TEST("cpu-minimum1", QEMU_CAPS_KVM);
    DO_TEST("cpu-minimum2", QEMU_CAPS_KVM);
    DO_TEST("cpu-exact1", QEMU_CAPS_KVM);
    DO_TEST("cpu-exact2", QEMU_CAPS_KVM);
    DO_TEST("cpu-exact2-nofallback", QEMU_CAPS_KVM);
    DO_TEST("cpu-fallback", QEMU_CAPS_KVM);
    DO_TEST_FAILURE("cpu-nofallback", QEMU_CAPS_KVM);
    DO_TEST("cpu-strict1", QEMU_CAPS_KVM);
    DO_TEST("cpu-no-removed-features", QEMU_CAPS_KVM);
    DO_TEST_NOCAPS("cpu-numa1");
    DO_TEST_NOCAPS("cpu-numa2");
    DO_TEST_NOCAPS("cpu-numa-no-memory-element");
    DO_TEST_PARSE_ERROR_NOCAPS("cpu-numa3");
    DO_TEST_NOCAPS("cpu-numa-disjoint");
    DO_TEST_NOCAPS("cpu-numa-memshared");
    DO_TEST("cpu-host-model",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_ICH9_USB_EHCI1);

    /* host-model cpu expansion depends on the cpu reported by qemu and thus
     * we invoke it for all real capability dumps we have */
    DO_TEST_CAPS_VER("cpu-host-model", "4.2.0");
    DO_TEST_CAPS_VER("cpu-host-model", "5.0.0");
    DO_TEST_CAPS_VER("cpu-host-model", "5.1.0");
    DO_TEST_CAPS_VER("cpu-host-model", "5.2.0");
    DO_TEST_CAPS_VER("cpu-host-model", "6.0.0");
    DO_TEST_CAPS_VER("cpu-host-model", "6.1.0");

    /* For this specific test we accept the increased likelihood of changes
     * if qemu updates the CPU model */
    DO_TEST_CAPS_LATEST("cpu-host-model");

    DO_TEST_NOCAPS("cpu-host-model-vendor");
    DO_TEST_FULL("cpu-host-model-fallback", "",
                 ARG_FLAGS, FLAG_SKIP_LEGACY_CPUS, ARG_END);
    DO_TEST_FULL("cpu-host-model-nofallback", "",
                 ARG_FLAGS, FLAG_SKIP_LEGACY_CPUS | FLAG_EXPECT_FAILURE,
                 ARG_END);
    DO_TEST("cpu-host-passthrough", QEMU_CAPS_KVM);
    DO_TEST_FAILURE("cpu-qemu-host-passthrough", QEMU_CAPS_KVM);

    DO_TEST_CAPS_ARCH_LATEST("cpu-s390-zEC12", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("cpu-s390-features", "s390x");

    qemuTestSetHostCPU(&driver, driver.hostarch, qemuTestGetCPUDef(QEMU_CPU_DEF_HASWELL));
    DO_TEST("cpu-Haswell", QEMU_CAPS_KVM);
    DO_TEST("cpu-Haswell2", QEMU_CAPS_KVM);
    DO_TEST("cpu-Haswell3", QEMU_CAPS_KVM);
    DO_TEST("cpu-Haswell-noTSX", QEMU_CAPS_KVM);
    DO_TEST_NOCAPS("cpu-host-model-cmt");
    DO_TEST("cpu-tsc-frequency", QEMU_CAPS_KVM);
    qemuTestSetHostCPU(&driver, driver.hostarch, NULL);

    DO_TEST_CAPS_LATEST("cpu-translation");

    DO_TEST_NOCAPS("memtune");
    DO_TEST_NOCAPS("memtune-unlimited");
    DO_TEST_NOCAPS("blkiotune");
    DO_TEST_NOCAPS("blkiotune-device");
    DO_TEST_CAPS_LATEST("cputune");
    DO_TEST_CAPS_LATEST("cputune-zero-shares");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("cputune-iothreadsched-toomuch");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("cputune-vcpusched-overlap");
    DO_TEST_CAPS_LATEST("cputune-numatune");
    DO_TEST_CAPS_LATEST("vcpu-placement-static");
    DO_TEST_CAPS_LATEST("cputune-cpuset-big-id");

    DO_TEST_NOCAPS("numatune-memory");
    DO_TEST_PARSE_ERROR_NOCAPS("numatune-memory-invalid-nodeset");
    DO_TEST_NOCAPS("numatune-memnode");
    DO_TEST_CAPS_VER("numatune-memnode", "5.2.0");
    DO_TEST_CAPS_LATEST("numatune-memnode");
    DO_TEST_PARSE_ERROR_NOCAPS("numatune-memnode-invalid-mode");
    DO_TEST_CAPS_LATEST("numatune-memnode-restrictive-mode");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("numatune-memnode-restrictive-mode-err-mixed");
    DO_TEST_CAPS_LATEST("numatune-system-memory");

    DO_TEST_NOCAPS("numatune-memnode-no-memory");

    DO_TEST_NOCAPS("numatune-distances");
    DO_TEST_NOCAPS("numatune-no-vcpu");
    DO_TEST_CAPS_LATEST("numatune-hmat");
    DO_TEST_CAPS_LATEST("numatune-hmat-none");

    DO_TEST_NOCAPS("numatune-auto-nodeset-invalid");
    DO_TEST_NOCAPS("numatune-auto-prefer");
    DO_TEST_FAILURE_NOCAPS("numatune-static-nodeset-exceed-hostnode");
    DO_TEST_PARSE_ERROR_NOCAPS("numatune-memnode-nocpu");
    DO_TEST_PARSE_ERROR_NOCAPS("numatune-memnodes-problematic");
    DO_TEST_CAPS_LATEST_FAILURE("numatune-memnode-unavailable-strict");
    DO_TEST_CAPS_LATEST_FAILURE("numatune-memnode-unavailable-restrictive");
    DO_TEST_NOCAPS("numad");
    DO_TEST_NOCAPS("numad-auto-vcpu-static-numatune");
    DO_TEST_PARSE_ERROR_NOCAPS("numad-auto-vcpu-static-numatune-no-nodeset");
    DO_TEST_NOCAPS("numad-auto-memory-vcpu-cpuset");
    DO_TEST_NOCAPS("numad-auto-memory-vcpu-no-cpuset-and-placement");
    DO_TEST_NOCAPS("numad-static-memory-auto-vcpu");
    DO_TEST_CAPS_LATEST("blkdeviotune");
    DO_TEST_CAPS_LATEST("blkdeviotune-max");
    DO_TEST_CAPS_LATEST("blkdeviotune-group-num");
    DO_TEST_CAPS_LATEST("blkdeviotune-max-length");

    DO_TEST("multifunction-pci-device",
            QEMU_CAPS_SCSI_LSI);

    DO_TEST_NOCAPS("monitor-json");

    DO_TEST_NOCAPS("seclabel-dynamic");
    DO_TEST_NOCAPS("seclabel-dynamic-baselabel");
    DO_TEST_NOCAPS("seclabel-dynamic-override");
    DO_TEST_NOCAPS("seclabel-dynamic-labelskip");
    DO_TEST_NOCAPS("seclabel-dynamic-relabel");
    DO_TEST_NOCAPS("seclabel-static");
    DO_TEST_NOCAPS("seclabel-static-relabel");
    DO_TEST_NOCAPS("seclabel-static-labelskip");
    DO_TEST_NOCAPS("seclabel-none");
    DO_TEST_NOCAPS("seclabel-dac-none");
    DO_TEST_PARSE_ERROR_NOCAPS("seclabel-multiple");
    DO_TEST_PARSE_ERROR_NOCAPS("seclabel-device-duplicates");
    DO_TEST_PARSE_ERROR_NOCAPS("seclabel-device-relabel-invalid");

    DO_TEST_CAPS_LATEST_PPC64("pseries-basic");
    DO_TEST_CAPS_LATEST_PPC64("pseries-vio");
    DO_TEST_CAPS_LATEST_PPC64("pseries-usb-default");
    DO_TEST_CAPS_LATEST_PPC64("pseries-usb-multi");
    DO_TEST_CAPS_LATEST_PPC64("pseries-vio-user-assigned");
    DO_TEST_CAPS_LATEST_PPC64("pseries-nvram");
    DO_TEST_CAPS_LATEST_PPC64("pseries-usb-kbd");
    DO_TEST_CAPS_LATEST_PPC64("pseries-cpu-exact");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("pseries-no-parallel", "ppc64");
    DO_TEST_CAPS_LATEST_PPC64("pseries-cpu-le");

    qemuTestSetHostArch(&driver, VIR_ARCH_PPC64);
    DO_TEST_CAPS_LATEST_PPC64_HOSTCPU("pseries-cpu-compat",
                                      QEMU_CPU_DEF_POWER9);
    DO_TEST_CAPS_LATEST_PPC64_HOSTCPU_FAILURE("pseries-cpu-compat-power9",
                                              QEMU_CPU_DEF_POWER8);
    DO_TEST_CAPS_LATEST_PPC64_HOSTCPU("pseries-cpu-compat-power9",
                                      QEMU_CPU_DEF_POWER9);
    DO_TEST_CAPS_LATEST_PPC64_HOSTCPU_FAILURE("pseries-cpu-compat-power10",
                                              QEMU_CPU_DEF_POWER9);
    DO_TEST_CAPS_LATEST_PPC64_HOSTCPU("pseries-cpu-compat-power10",
                                      QEMU_CPU_DEF_POWER10);

    qemuTestSetHostArch(&driver, VIR_ARCH_NONE);

    DO_TEST_CAPS_LATEST_PPC64("pseries-panic-missing");
    DO_TEST_CAPS_LATEST_PPC64("pseries-panic-no-address");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("pseries-panic-address", "ppc64");

    DO_TEST_CAPS_LATEST_PPC64("pseries-phb-simple");
    DO_TEST_CAPS_LATEST_PPC64("pseries-phb-default-missing");
    DO_TEST_CAPS_LATEST_PPC64("pseries-phb-numa-node");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("pseries-default-phb-numa-node",
                                         "ppc64");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("pseries-phb-invalid-target-index-1", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("pseries-phb-invalid-target-index-2", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("pseries-phb-invalid-target-index-3", "ppc64");

    DO_TEST_CAPS_LATEST_PPC64("pseries-many-devices");
    DO_TEST_CAPS_LATEST_PPC64("pseries-many-buses-1");
    DO_TEST_CAPS_LATEST_PPC64("pseries-many-buses-2");
    DO_TEST_CAPS_LATEST_PPC64("pseries-hostdevs-1");
    DO_TEST_CAPS_LATEST_PPC64("pseries-hostdevs-2");
    DO_TEST_CAPS_LATEST_PPC64("pseries-hostdevs-3");

    DO_TEST_CAPS_ARCH_VER("pseries-features", "ppc64", "4.2.0");
    DO_TEST_CAPS_LATEST_PPC64("pseries-features");

    DO_TEST_PARSE_ERROR_NOCAPS("pseries-features-invalid-machine");

    DO_TEST_CAPS_LATEST_PPC64("pseries-serial-native");
    DO_TEST_CAPS_LATEST_PPC64("pseries-serial+console-native");
    DO_TEST_CAPS_LATEST_PPC64("pseries-serial-compat");
    DO_TEST_CAPS_LATEST_PPC64("pseries-serial-pci");
    DO_TEST_CAPS_LATEST_PPC64("pseries-serial-usb");
    DO_TEST_CAPS_LATEST_PPC64("pseries-console-native");
    DO_TEST_CAPS_LATEST_PPC64("pseries-console-virtio");
    DO_TEST_PARSE_ERROR_NOCAPS("pseries-serial-invalid-machine");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("pseries-spaprvio-invalid", "ppc64");

    DO_TEST_CAPS_ARCH_LATEST("mach-virt-serial-native", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("mach-virt-serial+console-native", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("mach-virt-serial-compat", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("mach-virt-serial-pci", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("mach-virt-serial-usb", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("mach-virt-console-native", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("mach-virt-console-virtio", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("mach-virt-serial-invalid-machine", "x86_64");

    DO_TEST("video-device-pciaddr-default",
            QEMU_CAPS_KVM,
            QEMU_CAPS_VNC,
            QEMU_CAPS_DEVICE_QXL);
    DO_TEST("video-vga-device", QEMU_CAPS_DEVICE_VGA);
    DO_TEST("video-vga-device-vgamem", QEMU_CAPS_DEVICE_VGA,
            QEMU_CAPS_VGA_VGAMEM);
    DO_TEST("video-qxl-device",
            QEMU_CAPS_DEVICE_QXL);
    DO_TEST("video-qxl-device-vgamem",
            QEMU_CAPS_DEVICE_QXL,
            QEMU_CAPS_QXL_VGAMEM);
    DO_TEST_CAPS_LATEST("video-qxl-device-vram64");
    DO_TEST("video-qxl-sec-device",
            QEMU_CAPS_DEVICE_QXL);
    DO_TEST("video-qxl-sec-device-vgamem",
            QEMU_CAPS_DEVICE_QXL,
            QEMU_CAPS_QXL_VGAMEM);
    DO_TEST_CAPS_LATEST("video-qxl-sec-device-vram64");
    DO_TEST("video-qxl-heads", QEMU_CAPS_DEVICE_QXL);
    DO_TEST("video-vga-qxl-heads", QEMU_CAPS_DEVICE_QXL);
    DO_TEST("video-qxl-noheads", QEMU_CAPS_DEVICE_QXL);
    DO_TEST("video-qxl-resolution",
            QEMU_CAPS_DEVICE_QXL,
            QEMU_CAPS_QXL_VGAMEM);
    DO_TEST("video-virtio-gpu-device",
            QEMU_CAPS_DEVICE_VIRTIO_GPU);
    DO_TEST("video-virtio-gpu-virgl",
            QEMU_CAPS_DEVICE_VIRTIO_GPU,
            QEMU_CAPS_VIRTIO_GPU_VIRGL);
    DO_TEST("video-virtio-gpu-spice-gl",
            QEMU_CAPS_DEVICE_VIRTIO_GPU,
            QEMU_CAPS_VIRTIO_GPU_VIRGL,
            QEMU_CAPS_SPICE,
            QEMU_CAPS_SPICE_GL,
            QEMU_CAPS_SPICE_RENDERNODE);
    DO_TEST("video-virtio-gpu-sdl-gl",
            QEMU_CAPS_DEVICE_VIRTIO_GPU,
            QEMU_CAPS_VIRTIO_GPU_VIRGL,
            QEMU_CAPS_SDL);
    DO_TEST("video-virtio-gpu-secondary",
            QEMU_CAPS_DEVICE_VIRTIO_GPU);
    DO_TEST("video-virtio-vga",
            QEMU_CAPS_DEVICE_VIRTIO_GPU,
            QEMU_CAPS_DEVICE_VIRTIO_VGA);
    DO_TEST_CAPS_LATEST("video-virtio-blob-on");
    DO_TEST_CAPS_LATEST("video-virtio-blob-off");
    DO_TEST_CAPS_LATEST("video-virtio-vga-gpu-gl");
    DO_TEST_CAPS_LATEST("video-bochs-display-device");
    DO_TEST_CAPS_LATEST("video-ramfb-display-device");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("video-ramfb-display-device-pci-address");
    DO_TEST("video-none-device",
            QEMU_CAPS_VNC);
    DO_TEST_PARSE_ERROR_NOCAPS("video-invalid-multiple-devices");
    DO_TEST_PARSE_ERROR_NOCAPS("default-video-type-x86_64-caps-test-0");

    DO_TEST_CAPS_ARCH_LATEST("default-video-type-aarch64", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("default-video-type-ppc64", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("default-video-type-riscv64", "riscv64");
    DO_TEST_CAPS_ARCH_LATEST("default-video-type-s390x", "s390x");

    DO_TEST_PARSE_ERROR("video-multiple-primaries",
                        QEMU_CAPS_DEVICE_QXL,
                        QEMU_CAPS_DEVICE_VGA);

    DO_TEST("virtio-rng-default",
            QEMU_CAPS_DEVICE_VIRTIO_RNG,
            QEMU_CAPS_OBJECT_RNG_RANDOM);
    DO_TEST("virtio-rng-random",
            QEMU_CAPS_DEVICE_VIRTIO_RNG,
            QEMU_CAPS_OBJECT_RNG_RANDOM);
    DO_TEST("virtio-rng-egd",
            QEMU_CAPS_DEVICE_VIRTIO_RNG,
            QEMU_CAPS_OBJECT_RNG_EGD);
    DO_TEST_CAPS_VER("virtio-rng-builtin", "5.2.0");
    DO_TEST_CAPS_LATEST("virtio-rng-builtin");
    DO_TEST_CAPS_VER("virtio-rng-egd-unix", "5.2.0");
    DO_TEST_CAPS_LATEST("virtio-rng-egd-unix");
    DO_TEST("virtio-rng-multiple",
            QEMU_CAPS_DEVICE_VIRTIO_RNG,
            QEMU_CAPS_OBJECT_RNG_EGD,
            QEMU_CAPS_OBJECT_RNG_RANDOM);
    DO_TEST_PARSE_ERROR("virtio-rng-egd-crash",
            QEMU_CAPS_DEVICE_VIRTIO_RNG,
            QEMU_CAPS_OBJECT_RNG_EGD);
    DO_TEST_CAPS_ARCH_LATEST("virtio-rng-ccw", "s390x");

    DO_TEST_CAPS_ARCH_LATEST("s390-allow-bogus-usb-none", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("s390-allow-bogus-usb-controller", "s390x");

    DO_TEST_CAPS_ARCH_LATEST("s390-panic-no-address", "s390x");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("s390-panic-address", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("s390-panic-missing", "s390x");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("s390-no-parallel", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("s390-serial", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("s390-serial-2", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("s390-serial-console", "s390x");

    DO_TEST_CAPS_ARCH_LATEST("ppc-dtb", "ppc");
    DO_TEST_CAPS_ARCH_LATEST("ppce500-serial", "ppc");

    DO_TEST_CAPS_LATEST("tpm-passthrough");
    DO_TEST_CAPS_LATEST("tpm-passthrough-crb");
    DO_TEST_PARSE_ERROR("tpm-no-backend-invalid",
                        QEMU_CAPS_DEVICE_TPM_PASSTHROUGH, QEMU_CAPS_DEVICE_TPM_TIS);
    DO_TEST_CAPS_LATEST("tpm-emulator");
    DO_TEST_CAPS_LATEST("tpm-emulator-tpm2");
    DO_TEST_CAPS_LATEST("tpm-emulator-tpm2-enc");
    DO_TEST_CAPS_LATEST("tpm-emulator-tpm2-pstate");
    DO_TEST_CAPS_LATEST_PPC64("tpm-emulator-spapr");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-tpm", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("aarch64-tpm-wrong-model", "aarch64");
    DO_TEST_CAPS_LATEST("tpm-external");

    g_setenv(TEST_TPM_ENV_VAR, TPM_VER_2_0, true);
    DO_TEST_CAPS_LATEST_PARSE_ERROR("tpm-emulator");
    g_setenv(TEST_TPM_ENV_VAR, TPM_VER_1_2, true);
    DO_TEST_CAPS_LATEST_PARSE_ERROR("tpm-emulator-tpm2");
    unsetenv(TEST_TPM_ENV_VAR);

    DO_TEST_PARSE_ERROR_NOCAPS("pci-domain-invalid");
    DO_TEST_PARSE_ERROR_NOCAPS("pci-bus-invalid");
    DO_TEST_PARSE_ERROR_NOCAPS("pci-slot-invalid");
    DO_TEST_PARSE_ERROR_NOCAPS("pci-function-invalid");

    DO_TEST("pci-bridge",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_VNC,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST("pci-autoadd-addr",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST("pci-autoadd-idx",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST("pci-autofill-addr", QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST("pci-many",
            QEMU_CAPS_DEVICE_CIRRUS_VGA);
    DO_TEST("pci-bridge-many-disks",
            QEMU_CAPS_DEVICE_PCI_BRIDGE);
    DO_TEST("pcie-root",
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_DEVICE_QXL);
    DO_TEST("q35",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_ICH9_USB_EHCI1,
            QEMU_CAPS_DEVICE_QXL);
    DO_TEST_PARSE_ERROR("q35-dmi-bad-address1",
                        QEMU_CAPS_DEVICE_PCI_BRIDGE,
                        QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE,
                        QEMU_CAPS_DEVICE_IOH3420);
    DO_TEST_PARSE_ERROR("q35-dmi-bad-address2",
                        QEMU_CAPS_DEVICE_PCI_BRIDGE,
                        QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE,
                        QEMU_CAPS_DEVICE_IOH3420);
    DO_TEST("q35-pm-disable",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE, QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_PIIX_DISABLE_S3, QEMU_CAPS_PIIX_DISABLE_S4,
            QEMU_CAPS_ICH9_DISABLE_S3, QEMU_CAPS_ICH9_DISABLE_S4);
    DO_TEST("q35-pm-disable-fallback",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE, QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_PIIX_DISABLE_S3, QEMU_CAPS_PIIX_DISABLE_S4);
    DO_TEST_CAPS_LATEST("pc-i440fx-acpi-root-hotplug-disable");
    DO_TEST_CAPS_LATEST("pc-i440fx-acpi-root-hotplug-enable");
    DO_TEST_CAPS_VER_PARSE_ERROR("pc-i440fx-acpi-root-hotplug-disable", "5.1.0");
    DO_TEST_CAPS_VER_PARSE_ERROR("pc-i440fx-acpi-root-hotplug-enable", "5.1.0");
    DO_TEST("q35-usb2",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_ICH9_USB_EHCI1,
            QEMU_CAPS_DEVICE_QXL);
    DO_TEST("q35-usb2-multi",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_ICH9_USB_EHCI1,
            QEMU_CAPS_DEVICE_QXL);
    DO_TEST("q35-usb2-reorder",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_ICH9_USB_EHCI1,
            QEMU_CAPS_DEVICE_QXL);

    /* Note: The real caps versions of the following tests based on qemu-4.2.0
     * were added as a comparison point between fake caps testing and real caps
     * testing and don't have any other specific purpose */
    /* verify that devices with pcie capability are assigned to a pcie slot */
    DO_TEST_CAPS_VER("q35-pcie", "4.2.0");
    DO_TEST_CAPS_LATEST("q35-pcie");
    /* same as q35-pcie, but all PCI controllers are added automatically */
    DO_TEST_CAPS_VER("q35-pcie-autoadd", "4.2.0");
    DO_TEST_CAPS_LATEST("q35-pcie-autoadd");
    DO_TEST_CAPS_VER("q35-default-devices-only", "4.2.0");
    DO_TEST_CAPS_LATEST("q35-default-devices-only");
    DO_TEST_CAPS_VER("q35-multifunction", "4.2.0");
    DO_TEST_CAPS_LATEST("q35-multifunction");
    DO_TEST_CAPS_VER("q35-virt-manager-basic", "4.2.0");
    DO_TEST_CAPS_LATEST("q35-virt-manager-basic");

    /* Test automatic and manual setting of pcie-root-port attributes */
    DO_TEST("pcie-root-port",
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_DEVICE_QXL);

    /* Make sure the default model for PCIe Root Ports is picked correctly
     * based on QEMU binary capabilities. We use x86/q35 for the test, but
     * any PCIe machine type (such as aarch64/virt) will behave the same */
    DO_TEST("pcie-root-port-model-generic",
            QEMU_CAPS_DEVICE_PCIE_ROOT_PORT,
            QEMU_CAPS_DEVICE_IOH3420);
    DO_TEST("pcie-root-port-model-ioh3420",
            QEMU_CAPS_DEVICE_IOH3420);
    DO_TEST_CAPS_LATEST("pcie-root-port-nohotplug");

    DO_TEST("autoindex",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_DEVICE_X3130_UPSTREAM,
            QEMU_CAPS_DEVICE_XIO3130_DOWNSTREAM,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_ICH9_USB_EHCI1,
            QEMU_CAPS_NEC_USB_XHCI);
    /* Make sure the user can always override libvirt's default device
     * placement policy by providing an explicit PCI address */
    DO_TEST("q35-pci-force-address",
            QEMU_CAPS_DEVICE_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE,
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_HDA_DUPLEX);

    DO_TEST_PARSE_ERROR("q35-wrong-root",
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_ICH9_USB_EHCI1,
            QEMU_CAPS_DEVICE_QXL);
    DO_TEST_PARSE_ERROR_NOCAPS("440fx-wrong-root");
    DO_TEST_PARSE_ERROR_NOCAPS("440fx-ide-address-conflict");

    DO_TEST_PARSE_ERROR("pcie-root-port-too-many",
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_DEVICE_QXL);

    DO_TEST("pcie-switch-upstream-port",
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_DEVICE_X3130_UPSTREAM,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_DEVICE_QXL);
    DO_TEST("pcie-switch-downstream-port",
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_DEVICE_X3130_UPSTREAM,
            QEMU_CAPS_DEVICE_XIO3130_DOWNSTREAM,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_DEVICE_QXL);

    DO_TEST("pci-expander-bus",
            QEMU_CAPS_DEVICE_PXB);
    DO_TEST_PARSE_ERROR("pci-expander-bus-bad-node",
                        QEMU_CAPS_DEVICE_PXB);
    DO_TEST_PARSE_ERROR("pci-expander-bus-bad-machine",
                        QEMU_CAPS_DEVICE_PXB);
    DO_TEST_PARSE_ERROR("pci-expander-bus-bad-bus",
                        QEMU_CAPS_DEVICE_PXB);

    DO_TEST("pcie-expander-bus",
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_DEVICE_X3130_UPSTREAM,
            QEMU_CAPS_DEVICE_XIO3130_DOWNSTREAM,
            QEMU_CAPS_DEVICE_PXB_PCIE);
    DO_TEST_PARSE_ERROR("pcie-expander-bus-bad-machine",
                        QEMU_CAPS_DEVICE_IOH3420,
                        QEMU_CAPS_DEVICE_X3130_UPSTREAM,
                        QEMU_CAPS_DEVICE_XIO3130_DOWNSTREAM,
                        QEMU_CAPS_DEVICE_PXB_PCIE);
    DO_TEST_PARSE_ERROR("pcie-expander-bus-bad-bus",
                        QEMU_CAPS_DEVICE_IOH3420,
                        QEMU_CAPS_DEVICE_PXB_PCIE);
    DO_TEST_CAPS_ARCH_LATEST("pcie-expander-bus-aarch64", "aarch64");

    DO_TEST_CAPS_LATEST("hostdev-scsi-lsi");
    DO_TEST_CAPS_LATEST("hostdev-scsi-virtio-scsi");

    DO_TEST_CAPS_ARCH_LATEST("hostdev-scsi-vhost-scsi-ccw", "s390x");
    DO_TEST("hostdev-scsi-vhost-scsi-pci",
            QEMU_CAPS_VIRTIO_SCSI,
            QEMU_CAPS_DEVICE_VHOST_SCSI);
    DO_TEST_CAPS_LATEST_PARSE_ERROR("hostdev-scsi-vhost-scsi-pci-boot-fail");
    DO_TEST_CAPS_VER("hostdev-scsi-vhost-scsi-pcie", "4.2.0");
    DO_TEST_CAPS_LATEST("hostdev-scsi-vhost-scsi-pcie");
    DO_TEST_PARSE_ERROR("hostdev-scsi-duplicate",
                        QEMU_CAPS_VIRTIO_SCSI,
                        QEMU_CAPS_DEVICE_VHOST_SCSI);

    DO_TEST_CAPS_LATEST("mlock-on");
    DO_TEST_CAPS_LATEST("mlock-off");

    DO_TEST_PARSE_ERROR_NOCAPS("pci-bridge-negative-index-invalid");
    DO_TEST_PARSE_ERROR_NOCAPS("pci-bridge-duplicate-index");
    DO_TEST_PARSE_ERROR_NOCAPS("pci-root-nonzero-index");
    DO_TEST_PARSE_ERROR_NOCAPS("pci-root-address");

    DO_TEST("hotplug-base",
            QEMU_CAPS_KVM, QEMU_CAPS_VIRTIO_SCSI);

    DO_TEST_NOCAPS("pcihole64");
    DO_TEST("pcihole64-q35",
            QEMU_CAPS_DEVICE_IOH3420,
            QEMU_CAPS_ICH9_AHCI,
            QEMU_CAPS_DEVICE_QXL);

    DO_TEST_CAPS_ARCH_LATEST("arm-vexpressa9-nodevs", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("arm-vexpressa9-basic", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("arm-vexpressa9-virtio", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("arm-virt-virtio", "aarch64");

    /* test default config if pcie bus is not available */
    DO_TEST_FULL("aarch64-virt-virtio", "-MMIO.aarch64.latest",
                 ARG_CAPS_ARCH, "aarch64",
                 ARG_CAPS_VER, "latest",
                 ARG_QEMU_CAPS_DEL,
                 QEMU_CAPS_OBJECT_GPEX, QEMU_CAPS_DEVICE_PCI_BRIDGE,
                 QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE, QEMU_CAPS_DEVICE_IOH3420,
                 QEMU_CAPS_LAST, ARG_END);

    DO_TEST_CAPS_ARCH_VER("aarch64-virt-virtio", "aarch64", "4.2.0");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-virt-virtio", "aarch64");

    /* Demonstrates the virtio-pci default... namely that there isn't any!
       q35 style PCI controllers will be added if the binary supports it,
       but virtio-mmio is always used unless PCI addresses are manually
       specified. */
    DO_TEST_CAPS_ARCH_VER("aarch64-virtio-pci-default", "aarch64", "4.2.0");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-virtio-pci-default", "aarch64");
    /* Example of using virtio-pci with no explicit PCI controller
       but with manual PCI addresses */
    DO_TEST_CAPS_ARCH_LATEST("aarch64-virtio-pci-manual-addresses", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-video-virtio-gpu-pci", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-video-default", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-aavmf-virtio-mmio", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-virt-default-nic", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-cpu-passthrough", "aarch64");
    DO_TEST_FULL("aarch64-gic-none", ".aarch64-4.2.0",
                 ARG_GIC, GIC_NONE,
                 ARG_CAPS_ARCH, "aarch64",
                 ARG_CAPS_VER, "4.2.0",
                 ARG_END);
    DO_TEST_FULL("aarch64-gic-none", ".aarch64-latest",
                 ARG_GIC, GIC_NONE,
                 ARG_CAPS_ARCH, "aarch64",
                 ARG_CAPS_VER, "latest",
                 ARG_END);
    DO_TEST_GIC("aarch64-gic-none-v2", GIC_V2);
    DO_TEST_GIC("aarch64-gic-none-v3", GIC_V3);
    DO_TEST_GIC("aarch64-gic-none-both", GIC_BOTH);
    DO_TEST_FULL("aarch64-gic-none-tcg", "",
                 ARG_GIC, GIC_BOTH,
                 ARG_CAPS_ARCH, "aarch64",
                 ARG_CAPS_VER, "latest",
                 ARG_QEMU_CAPS_DEL, QEMU_CAPS_KVM, QEMU_CAPS_LAST,
                 ARG_END);
    DO_TEST_FULL("aarch64-gic-default", ".aarch64-4.2.0",
                 ARG_GIC, GIC_NONE,
                 ARG_CAPS_ARCH, "aarch64",
                 ARG_CAPS_VER, "4.2.0",
                 ARG_END);
    DO_TEST_FULL("aarch64-gic-default", ".aarch64-latest",
                 ARG_GIC, GIC_NONE,
                 ARG_CAPS_ARCH, "aarch64",
                 ARG_CAPS_VER, "latest",
                 ARG_END);
    DO_TEST_GIC("aarch64-gic-default-v2", GIC_V2);
    DO_TEST_GIC("aarch64-gic-default-v3", GIC_V3);
    DO_TEST_GIC("aarch64-gic-default-both", GIC_BOTH);
    DO_TEST_GIC("aarch64-gic-v2", GIC_NONE);
    DO_TEST_GIC("aarch64-gic-v2", GIC_V2);
    DO_TEST_GIC("aarch64-gic-v2", GIC_V3);
    DO_TEST_GIC("aarch64-gic-v2", GIC_BOTH);
    DO_TEST_GIC("aarch64-gic-v3", GIC_NONE);
    DO_TEST_GIC("aarch64-gic-v3", GIC_V2);
    DO_TEST_GIC("aarch64-gic-v3", GIC_V3);
    DO_TEST_GIC("aarch64-gic-v3", GIC_BOTH);
    DO_TEST_GIC("aarch64-gic-host", GIC_NONE);
    DO_TEST_GIC("aarch64-gic-host", GIC_V2);
    DO_TEST_GIC("aarch64-gic-host", GIC_V3);
    DO_TEST_GIC("aarch64-gic-host", GIC_BOTH);
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("aarch64-gic-invalid", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("aarch64-gic-not-virt", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("aarch64-gic-not-arm", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-kvm-32-on-64", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-pci-serial", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-traditional-pci", "aarch64");

    /* aarch64 doesn't support the same CPU features as x86 */
    DO_TEST_CAPS_ARCH_LATEST_FAILURE("aarch64-features-wrong", "aarch64");
    /* Can't enable vector lengths when SVE is overall disabled */
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("aarch64-features-sve-disabled", "aarch64");
    /* SVE aarch64 CPU features work on modern QEMU */
    DO_TEST_CAPS_ARCH_LATEST("aarch64-features-sve", "aarch64");

    DO_TEST_CAPS_ARCH_LATEST("clock-timer-armvtimer", "aarch64");

    qemuTestSetHostArch(&driver, VIR_ARCH_NONE);

    DO_TEST("kvm-pit-delay", QEMU_CAPS_KVM_PIT_TICK_POLICY);
    DO_TEST("kvm-pit-discard", QEMU_CAPS_KVM_PIT_TICK_POLICY);

    DO_TEST_CAPS_LATEST("panic");
    DO_TEST_CAPS_LATEST("panic-double");
    DO_TEST_CAPS_LATEST("panic-no-address");

    DO_TEST_CAPS_LATEST("pvpanic-pci-x86_64");
    DO_TEST_CAPS_ARCH_LATEST("pvpanic-pci-aarch64", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("pvpanic-pci-invalid-address-aarch64", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("pvpanic-pci-no-address-aarch64", "aarch64");

    DO_TEST_CAPS_ARCH_VER_FULL("fips-enabled", "x86_64", "5.1.0", ARG_FLAGS, FLAG_FIPS_HOST);
    DO_TEST_CAPS_ARCH_LATEST_FULL("fips-enabled", "x86_64", ARG_FLAGS, FLAG_FIPS_HOST);

    DO_TEST("shmem", QEMU_CAPS_DEVICE_IVSHMEM);
    DO_TEST("shmem-plain-doorbell", QEMU_CAPS_DEVICE_IVSHMEM,
            QEMU_CAPS_DEVICE_IVSHMEM_PLAIN,
            QEMU_CAPS_DEVICE_IVSHMEM_DOORBELL);
    DO_TEST_PARSE_ERROR_NOCAPS("shmem");
    DO_TEST_FAILURE("shmem-invalid-size",
                    QEMU_CAPS_DEVICE_IVSHMEM);
    DO_TEST_FAILURE("shmem-invalid-address",
                    QEMU_CAPS_DEVICE_IVSHMEM);
    DO_TEST_FAILURE("shmem-small-size",
                    QEMU_CAPS_DEVICE_IVSHMEM);
    DO_TEST_PARSE_ERROR_NOCAPS("shmem-msi-only");
    DO_TEST("cpu-host-passthrough-features", QEMU_CAPS_KVM);

    DO_TEST_FAILURE_NOCAPS("memory-align-fail");
    DO_TEST_PARSE_ERROR("memory-hotplug-nonuma", QEMU_CAPS_DEVICE_PC_DIMM);
    DO_TEST_PARSE_ERROR_NOCAPS("memory-hotplug-invalid-targetnode");
    DO_TEST_NOCAPS("memory-hotplug");
    DO_TEST("memory-hotplug", QEMU_CAPS_DEVICE_PC_DIMM);
    DO_TEST("memory-hotplug-dimm", QEMU_CAPS_DEVICE_PC_DIMM);
    DO_TEST_CAPS_LATEST("memory-hotplug-dimm-addr");
    DO_TEST_CAPS_ARCH_LATEST("memory-hotplug-ppc64-nonuma", "ppc64");
    DO_TEST_FULL("memory-hotplug-ppc64-nonuma-abi-update", "",
                 ARG_PARSEFLAGS, VIR_DOMAIN_DEF_PARSE_ABI_UPDATE,
                 ARG_CAPS_ARCH, "ppc64", ARG_CAPS_VER, "latest",
                 ARG_END);
    DO_TEST_CAPS_LATEST("memory-hotplug-nvdimm");
    DO_TEST_CAPS_LATEST("memory-hotplug-nvdimm-access");
    DO_TEST_CAPS_VER("memory-hotplug-nvdimm-label", "5.2.0");
    DO_TEST_CAPS_LATEST("memory-hotplug-nvdimm-label");
    DO_TEST_CAPS_VER("memory-hotplug-nvdimm-align", "5.2.0");
    DO_TEST_CAPS_LATEST("memory-hotplug-nvdimm-align");
    DO_TEST_CAPS_VER("memory-hotplug-nvdimm-pmem", "5.2.0");
    DO_TEST_CAPS_LATEST("memory-hotplug-nvdimm-pmem");
    DO_TEST_CAPS_VER("memory-hotplug-nvdimm-readonly", "5.2.0");
    DO_TEST_CAPS_LATEST("memory-hotplug-nvdimm-readonly");
    DO_TEST_CAPS_ARCH_LATEST("memory-hotplug-nvdimm-ppc64", "ppc64");
    DO_TEST_FULL("memory-hotplug-nvdimm-ppc64-abi-update", "",
                 ARG_PARSEFLAGS, VIR_DOMAIN_DEF_PARSE_ABI_UPDATE,
                 ARG_CAPS_ARCH, "ppc64", ARG_CAPS_VER, "latest",
                 ARG_END);
    DO_TEST_CAPS_VER("memory-hotplug-virtio-pmem", "5.2.0");
    DO_TEST_CAPS_LATEST("memory-hotplug-virtio-pmem");
    DO_TEST_CAPS_LATEST("memory-hotplug-virtio-mem");
    DO_TEST_CAPS_LATEST("memory-hotplug-multiple");

    DO_TEST_CAPS_ARCH_LATEST("machine-aeskeywrap-on-caps", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-aeskeywrap-on-cap", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-aeskeywrap-off-caps", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-aeskeywrap-off-cap", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-deakeywrap-on-caps", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-deakeywrap-on-cap", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-deakeywrap-off-caps", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-deakeywrap-off-cap", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-keywrap-none-caps", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-keywrap-none", "s390x");

    DO_TEST_CAPS_ARCH_LATEST("machine-loadparm-s390", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-loadparm-net-s390", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-loadparm-hostdev", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("machine-loadparm-multiple-disks-nets-s390", "s390x");
    DO_TEST_PARSE_ERROR_NOCAPS("machine-loadparm-s390-char-invalid");
    DO_TEST_PARSE_ERROR_NOCAPS("machine-loadparm-s390-len-invalid");

    DO_TEST_NOCAPS("qemu-ns-domain-ns0");
    DO_TEST_NOCAPS("qemu-ns-domain-commandline");
    DO_TEST_NOCAPS("qemu-ns-domain-commandline-ns0");
    DO_TEST_NOCAPS("qemu-ns-commandline");
    DO_TEST_NOCAPS("qemu-ns-commandline-ns0");
    DO_TEST_NOCAPS("qemu-ns-commandline-ns1");

    DO_TEST("virtio-input", QEMU_CAPS_VIRTIO_KEYBOARD,
            QEMU_CAPS_VIRTIO_MOUSE, QEMU_CAPS_VIRTIO_TABLET);
    DO_TEST("virtio-input-passthrough", QEMU_CAPS_VIRTIO_INPUT_HOST);

    DO_TEST_CAPS_LATEST("input-linux");

    DO_TEST_CAPS_ARCH_LATEST("ppc64-usb-controller", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("ppc64-usb-controller-legacy", "ppc64");
    DO_TEST_FULL("ppc64-usb-controller-qemu-xhci", "",
                 ARG_PARSEFLAGS, VIR_DOMAIN_DEF_PARSE_ABI_UPDATE,
                 ARG_CAPS_ARCH, "ppc64", ARG_CAPS_VER, "latest",
                 ARG_END);

    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("ppc64-tpmproxy-double", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("ppc64-tpm-double", "ppc64");

    DO_TEST_CAPS_LATEST_PPC64("ppc64-tpmproxy-single");
    DO_TEST_CAPS_LATEST_PPC64("ppc64-tpmproxy-with-tpm");

    DO_TEST_CAPS_ARCH_LATEST("aarch64-usb-controller", "aarch64");

    DO_TEST_CAPS_ARCH_LATEST("sparc-minimal", "sparc");

    DO_TEST_CAPS_LATEST_PARSE_ERROR("missing-machine");

    DO_TEST_CAPS_LATEST("name-escape");

    DO_TEST_NOCAPS("master-key");
    DO_TEST("usb-long-port-path",
            QEMU_CAPS_USB_HUB);
    DO_TEST_PARSE_ERROR("usb-too-long-port-path-invalid",
                        QEMU_CAPS_USB_HUB);

    DO_TEST_NOCAPS("acpi-table");

    DO_TEST_CAPS_LATEST("intel-iommu");
    DO_TEST_CAPS_LATEST("intel-iommu-caching-mode");
    DO_TEST_CAPS_LATEST("intel-iommu-eim");
    DO_TEST_CAPS_LATEST("intel-iommu-device-iotlb");
    DO_TEST_CAPS_LATEST("intel-iommu-aw-bits");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("intel-iommu-wrong-machine");
    DO_TEST_CAPS_ARCH_LATEST("iommu-smmuv3", "aarch64");
    DO_TEST_CAPS_LATEST("virtio-iommu-x86_64");
    DO_TEST_CAPS_VER_PARSE_ERROR("virtio-iommu-x86_64", "6.1.0");
    DO_TEST_CAPS_ARCH_LATEST("virtio-iommu-aarch64", "aarch64");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("virtio-iommu-wrong-machine");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("virtio-iommu-no-acpi");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("virtio-iommu-invalid-address-type");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("virtio-iommu-invalid-address");

    DO_TEST("cpu-hotplug-startup", QEMU_CAPS_QUERY_HOTPLUGGABLE_CPUS);
    DO_TEST_CAPS_ARCH_LATEST_PARSE_ERROR("cpu-hotplug-granularity", "ppc64");

    DO_TEST_CAPS_LATEST("virtio-options");
    DO_TEST_CAPS_LATEST("virtio-options-controller-iommu");
    DO_TEST_CAPS_LATEST("virtio-options-disk-iommu");
    DO_TEST_CAPS_LATEST("virtio-options-fs-iommu");
    DO_TEST_CAPS_LATEST("virtio-options-input-iommu");
    DO_TEST_CAPS_LATEST("virtio-options-memballoon-iommu");
    DO_TEST_CAPS_LATEST("virtio-options-net-iommu");
    DO_TEST_CAPS_LATEST("virtio-options-rng-iommu");
    DO_TEST_CAPS_LATEST("virtio-options-video-iommu");
    DO_TEST_CAPS_LATEST("virtio-options-controller-ats");
    DO_TEST_CAPS_LATEST("virtio-options-disk-ats");
    DO_TEST_CAPS_LATEST("virtio-options-fs-ats");
    DO_TEST_CAPS_LATEST("virtio-options-input-ats");
    DO_TEST_CAPS_LATEST("virtio-options-memballoon-ats");
    DO_TEST_CAPS_LATEST("virtio-options-net-ats");
    DO_TEST_CAPS_LATEST("virtio-options-rng-ats");
    DO_TEST_CAPS_LATEST("virtio-options-video-ats");
    DO_TEST_CAPS_LATEST("virtio-options-controller-packed");
    DO_TEST_CAPS_LATEST("virtio-options-disk-packed");
    DO_TEST_CAPS_LATEST("virtio-options-fs-packed");
    DO_TEST_CAPS_LATEST("virtio-options-input-packed");
    DO_TEST_CAPS_LATEST("virtio-options-memballoon-packed");
    DO_TEST_CAPS_LATEST("virtio-options-memballoon-freepage-reporting");
    DO_TEST_CAPS_LATEST("virtio-options-net-packed");
    DO_TEST_CAPS_LATEST("virtio-options-rng-packed");
    DO_TEST_CAPS_LATEST("virtio-options-video-packed");
    DO_TEST_PARSE_ERROR_NOCAPS("virtio-options-memballoon-freepage-reporting");

    DO_TEST("fd-memory-numa-topology", QEMU_CAPS_KVM);
    DO_TEST("fd-memory-numa-topology2", QEMU_CAPS_KVM);
    DO_TEST("fd-memory-numa-topology3", QEMU_CAPS_KVM);
    DO_TEST_CAPS_LATEST("fd-memory-numa-topology4");

    DO_TEST("fd-memory-no-numa-topology", QEMU_CAPS_KVM);

    DO_TEST_CAPS_LATEST("memfd-memory-numa");
    DO_TEST_CAPS_LATEST("memfd-memory-default-hugepage");

    DO_TEST("cpu-check-none", QEMU_CAPS_KVM);
    DO_TEST("cpu-check-partial", QEMU_CAPS_KVM);
    DO_TEST("cpu-check-full", QEMU_CAPS_KVM);
    DO_TEST("cpu-check-default-none", QEMU_CAPS_KVM);
    DO_TEST_NOCAPS("cpu-check-default-none2");
    DO_TEST("cpu-check-default-partial", QEMU_CAPS_KVM);
    DO_TEST("cpu-check-default-partial2", QEMU_CAPS_KVM);

    DO_TEST("cpu-cache-disable", QEMU_CAPS_KVM, QEMU_CAPS_CPU_CACHE);
    DO_TEST("cpu-cache-disable2", QEMU_CAPS_KVM);
    DO_TEST("cpu-cache-disable3", QEMU_CAPS_KVM, QEMU_CAPS_CPU_CACHE);
    DO_TEST("cpu-cache-passthrough", QEMU_CAPS_KVM, QEMU_CAPS_CPU_CACHE);
    DO_TEST("cpu-cache-passthrough2", QEMU_CAPS_KVM);
    DO_TEST("cpu-cache-emulate-l3", QEMU_CAPS_KVM, QEMU_CAPS_CPU_CACHE);
    DO_TEST_PARSE_ERROR("cpu-cache-emulate-l2", QEMU_CAPS_KVM);
    DO_TEST_PARSE_ERROR("cpu-cache-passthrough3", QEMU_CAPS_KVM);
    DO_TEST_PARSE_ERROR("cpu-cache-passthrough-l3", QEMU_CAPS_KVM);
    DO_TEST("vmcoreinfo", QEMU_CAPS_DEVICE_VMCOREINFO);

    DO_TEST_CAPS_LATEST("user-aliases");
    DO_TEST_CAPS_LATEST("user-aliases2");
    DO_TEST_CAPS_LATEST("user-aliases-usb");

    DO_TEST_CAPS_LATEST("tseg-explicit-size");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("tseg-i440fx");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("tseg-invalid-size");

    DO_TEST_CAPS_ARCH_LATEST("video-virtio-gpu-ccw", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("input-virtio-ccw", "s390x");

    DO_TEST_CAPS_LATEST("vhost-vsock");
    DO_TEST_CAPS_LATEST("vhost-vsock-auto");
    DO_TEST_CAPS_ARCH_LATEST("vhost-vsock-ccw", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("vhost-vsock-ccw-auto", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("vhost-vsock-ccw-iommu", "s390x");

    DO_TEST_CAPS_VER("launch-security-sev", "6.0.0");
    DO_TEST_CAPS_VER("launch-security-sev-missing-platform-info", "6.0.0");
    DO_TEST_CAPS_ARCH_LATEST_FULL("launch-security-sev-direct",
                                  "x86_64",
                                  ARG_QEMU_CAPS,
                                  QEMU_CAPS_SEV_GUEST,
                                  QEMU_CAPS_LAST);

    DO_TEST_CAPS_ARCH_LATEST("launch-security-s390-pv", "s390x");

    DO_TEST_CAPS_LATEST("vhost-user-fs-fd-memory");
    DO_TEST_CAPS_LATEST("vhost-user-fs-hugepages");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("vhost-user-fs-readonly");

    /* The generic pcie bridge emulation device can be compiled out of qemu. */
    DO_TEST_CAPS_ARCH_LATEST_FULL("riscv64-virt", "riscv64",
                                  ARG_QEMU_CAPS_DEL,
                                  QEMU_CAPS_OBJECT_GPEX,
                                  QEMU_CAPS_LAST);
    DO_TEST_CAPS_ARCH_LATEST("riscv64-virt-pci", "riscv64");

    DO_TEST_CAPS_LATEST("virtio-transitional");
    DO_TEST_CAPS_LATEST("virtio-non-transitional");
    DO_TEST_CAPS_LATEST_PARSE_ERROR("virtio-transitional-not-supported");

    /* Simple headless guests for various architectures */
    DO_TEST_CAPS_ARCH_LATEST("aarch64-virt-headless", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("ppc64-pseries-headless", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("riscv64-virt-headless", "riscv64");
    DO_TEST_CAPS_ARCH_LATEST("s390x-ccw-headless", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("x86_64-pc-headless", "x86_64");
    DO_TEST_CAPS_ARCH_LATEST("x86_64-q35-headless", "x86_64");

    /* Simple guests with graphics for various architectures */
    DO_TEST_CAPS_ARCH_LATEST("aarch64-virt-graphics", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("ppc64-pseries-graphics", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("riscv64-virt-graphics", "riscv64");
    DO_TEST_CAPS_ARCH_LATEST("s390x-ccw-graphics", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("x86_64-pc-graphics", "x86_64");
    DO_TEST_CAPS_ARCH_LATEST("x86_64-q35-graphics", "x86_64");

    DO_TEST_CAPS_LATEST("vhost-user-vga");
    DO_TEST_CAPS_LATEST("vhost-user-gpu-secondary");

    DO_TEST_CAPS_LATEST("cpu-Icelake-Server-pconfig");

    DO_TEST_CAPS_ARCH_LATEST("aarch64-default-cpu-kvm-virt-4.2", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("aarch64-default-cpu-tcg-virt-4.2", "aarch64");
    DO_TEST_CAPS_ARCH_LATEST("ppc64-default-cpu-kvm-pseries-2.7", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("ppc64-default-cpu-tcg-pseries-2.7", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("ppc64-default-cpu-kvm-pseries-3.1", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("ppc64-default-cpu-tcg-pseries-3.1", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("ppc64-default-cpu-kvm-pseries-4.2", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("ppc64-default-cpu-tcg-pseries-4.2", "ppc64");
    DO_TEST_CAPS_ARCH_LATEST("s390-default-cpu-kvm-ccw-virtio-2.7", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("s390-default-cpu-tcg-ccw-virtio-2.7", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("s390-default-cpu-kvm-ccw-virtio-4.2", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("s390-default-cpu-tcg-ccw-virtio-4.2", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("x86_64-default-cpu-kvm-pc-4.2", "x86_64");
    DO_TEST_CAPS_ARCH_LATEST("x86_64-default-cpu-tcg-pc-4.2", "x86_64");
    DO_TEST_CAPS_ARCH_LATEST("x86_64-default-cpu-kvm-q35-4.2", "x86_64");
    DO_TEST_CAPS_ARCH_LATEST("x86_64-default-cpu-tcg-q35-4.2", "x86_64");
    DO_TEST_CAPS_ARCH_LATEST("x86_64-default-cpu-tcg-features", "x86_64");

    DO_TEST_CAPS_LATEST("virtio-9p-multidevs");
    DO_TEST_CAPS_LATEST("virtio-9p-createmode");

    DO_TEST_CAPS_LATEST("devices-acpi-index");

    DO_TEST_CAPS_ARCH_LATEST_FULL("hvf-x86_64-q35-headless", "x86_64", ARG_CAPS_VARIANT, "+hvf", ARG_END);
    DO_TEST_CAPS_ARCH_LATEST_FULL("hvf-aarch64-virt-headless", "aarch64", ARG_CAPS_VARIANT, "+hvf", ARG_END);
    /* HVF guests should not work on Linux with KVM */
    DO_TEST_CAPS_LATEST_PARSE_ERROR("hvf-x86_64-q35-headless");

    DO_TEST("cpu-phys-bits-passthrough", QEMU_CAPS_KVM);
    DO_TEST("cpu-phys-bits-emulate", QEMU_CAPS_KVM);
    DO_TEST("cpu-phys-bits-emulate2", QEMU_CAPS_KVM);
    DO_TEST_PARSE_ERROR("cpu-phys-bits-passthrough2", QEMU_CAPS_KVM);
    DO_TEST_CAPS_LATEST("cpu-phys-bits-limit");
    DO_TEST_CAPS_LATEST("cpu-phys-bits-emulate-bare");

    DO_TEST_CAPS_VER("sgx-epc", "7.0.0");

    DO_TEST_CAPS_LATEST("crypto-builtin");

    DO_TEST_CAPS_LATEST("async-teardown");
    DO_TEST_CAPS_ARCH_LATEST("s390-async-teardown", "s390x");
    DO_TEST_CAPS_ARCH_LATEST("s390-async-teardown-no-attrib", "s390x");
    DO_TEST_CAPS_ARCH_VER_PARSE_ERROR("s390-async-teardown", "s390x", "6.0.0");
    DO_TEST_CAPS_ARCH_LATEST("s390-async-teardown-disabled", "s390x");
    DO_TEST_CAPS_ARCH_VER("s390-async-teardown-disabled", "s390x", "6.0.0");

    qemuTestDriverFree(&driver);
    virFileWrapperClearPrefixes();

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN_PRELOAD(mymain,
                      VIR_TEST_MOCK("qemuxml2argv"),
                      VIR_TEST_MOCK("domaincaps"),
                      VIR_TEST_MOCK("virrandom"),
                      VIR_TEST_MOCK("qemucpu"),
                      VIR_TEST_MOCK("virpci"),
                      VIR_TEST_MOCK("virnuma"))

#else

int main(void)
{
    return EXIT_AM_SKIP;
}

#endif /* WITH_QEMU */
