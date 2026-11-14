/* SPDX-License-Identifier: MIT */
/* Read-only PCI aperture for an owned parent's WDDM fence page. Lab only. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/wddm-fence-lab.h"
#include "system/whpx.h"
#include "migration/blocker.h"

#define TYPE_WDDM_FENCE_LAB "wddm-fence-lab"
OBJECT_DECLARE_SIMPLE_TYPE(WddmFenceLab, WDDM_FENCE_LAB)

struct WddmFenceLab {
    PCIDevice parent_obj;
    MemoryRegion fence;
    uint64_t source_process;
    uint64_t source_address;
    uint64_t expected;
    HANDLE process;
    uint32_t emulated_reads;
    Error *migration_blocker;
};

bool wddm_fence_lab_mapping(MemoryRegion *mr, void **process, void **source)
{
    Object *owner = memory_region_owner(mr);
    WddmFenceLab *s = owner ? (WddmFenceLab *)
        object_dynamic_cast(owner, TYPE_WDDM_FENCE_LAB) : NULL;

    if (!s || mr != &s->fence || !s->process) {
        return wddm_fence_hub_mapping(mr, process, source);
    }
    *process = s->process;
    *source = (void *)(uintptr_t)(s->source_address & ~4095ULL);
    return true;
}

static uint64_t fence_read(void *opaque, hwaddr address, unsigned size)
{
    WddmFenceLab *s = opaque;
    uint64_t value = 0;
    SIZE_T copied = 0;
    uintptr_t base = s->source_address & ~4095ULL;

    ++s->emulated_reads;
    if (address > 4096 || size > 4096 - address ||
        !ReadProcessMemory(s->process, (void *)(base + address),
                           &value, size, &copied) || copied != size) {
        error_report("WDDM fence lab: emulated fence read failed");
        return UINT64_MAX;
    }
    return value;
}

static void fence_write(void *opaque, hwaddr address, uint64_t value,
                        unsigned size)
{
    /* Read-only mapping; never forward an emulated write to the driver. */
}

static const MemoryRegionOps fence_ops = {
    .read = fence_read,
    .write = fence_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static uint32_t fence_config_read(PCIDevice *pci, uint32_t address, int size)
{
    WddmFenceLab *s = WDDM_FENCE_LAB(pci);
    pci_set_long(pci->config + 0x50, s->emulated_reads);
    return pci_default_read_config(pci, address, size);
}

static void fence_realize(PCIDevice *pci, Error **errp)
{
    WddmFenceLab *s = WDDM_FENCE_LAB(pci);
    HMODULE whp = GetModuleHandleW(L"WinHvPlatform.dll");
    SIZE_T copied = 0;
    uint64_t observed = 0;

    if (!whpx_enabled() || !whp || !GetProcAddress(whp, "WHvMapGpaRange2")) {
        error_setg(errp, "WDDM fence lab requires WHPX and WHvMapGpaRange2");
        return;
    }
    if (!s->source_process || !s->source_address || s->source_address % 8 ||
        (s->source_address & 4095) > 4088) {
        error_setg(errp, "WDDM fence lab requires a source handle and aligned fence address");
        return;
    }
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)(uintptr_t)s->source_process,
                         GetCurrentProcess(), &s->process,
                         PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                         FALSE, 0)) {
        error_setg(errp, "Cannot duplicate the inherited fence-owner process handle");
        return;
    }
    if (!ReadProcessMemory(s->process, (void *)(uintptr_t)s->source_address,
                           &observed, sizeof observed, &copied) ||
        copied != sizeof observed || observed != s->expected) {
        error_setg(errp, "Fence owner does not expose the expected idle value");
        return;
    }
    error_setg(&s->migration_blocker, "WDDM fence lab mappings are not migratable");
    if (migrate_add_blocker(&s->migration_blocker, errp)) {
        return;
    }
    memory_region_init_io(&s->fence, OBJECT(s), &fence_ops, s,
                          "wddm-fence-lab", 4096);
    memory_region_set_readonly(&s->fence, true);
    pci_register_bar(pci, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->fence);
    /* Read-only lab metadata, not a production ABI. No host address exposed. */
    pci_set_long(pci->config + 0x40, 0x31434e46); /* FNC1 */
    pci_set_long(pci->config + 0x44, s->source_address & 4095);
    pci_set_quad(pci->config + 0x48, s->expected);
}

static void fence_finalize(Object *object)
{
    WddmFenceLab *s = WDDM_FENCE_LAB(object);
    /* Region children are finalized before their owning object. The harness
     * also holds driver objects until the entire QEMU process has exited. */
    if (s->process) {
        CloseHandle(s->process);
    }
    if (s->migration_blocker) {
        migrate_del_blocker(&s->migration_blocker);
    }
}

static const Property fence_properties[] = {
    DEFINE_PROP_UINT64("source-process", WddmFenceLab, source_process, 0),
    DEFINE_PROP_UINT64("source-address", WddmFenceLab, source_address, 0),
    DEFINE_PROP_UINT64("expected", WddmFenceLab, expected, 0),
};

static void fence_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    pc->realize = fence_realize;
    pc->config_read = fence_config_read;
    /* Private lab identifiers. These are not assigned production IDs. */
    pc->vendor_id = 0x1234;
    pc->device_id = 0x11fe;
    pc->class_id = PCI_CLASS_OTHERS;
    dc->desc = "Read-only Windows driver fence diagnostic";
    dc->hotpluggable = false;
    device_class_set_props(dc, fence_properties);
}

static const TypeInfo fence_info = {
    .name = TYPE_WDDM_FENCE_LAB,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(WddmFenceLab),
    .instance_finalize = fence_finalize,
    .class_init = fence_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void register_fence(void)
{
    type_register_static(&fence_info);
}
type_init(register_fence);
