/* SPDX-License-Identifier: MIT */
/* Dynamic read-only driver pages. Controlled by the owning host over QMP. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/wddm-fence-lab.h"
#include "hw/misc/wddm-fence-map.h"
#include "system/whpx.h"
#include "migration/blocker.h"

#define TYPE_WDDM_FENCE_HUB "wddm-fence-hub"
OBJECT_DECLARE_SIMPLE_TYPE(WddmFenceHub, WDDM_FENCE_HUB)
typedef struct WddmFenceSlot {
    MemoryRegion region;
    struct WddmFenceHub *hub;
    uint64_t source, generation;
} WddmFenceSlot;
struct WddmFenceHub {
    PCIDevice parent_obj;
    MemoryRegion bar;
    WddmFenceSlot slots[WDDM_FENCE_HUB_MAX_PAGES];
    uint64_t source_process, last_generation;
    HANDLE process;
    uint32_t emulated_reads, mapped, slot_count;
    Error *migration_blocker;
};

bool wddm_fence_hub_mapping(MemoryRegion *mr, void **process, void **source)
{
    Object *owner = memory_region_owner(mr);
    WddmFenceHub *s = owner ? (WddmFenceHub *)object_dynamic_cast(owner, TYPE_WDDM_FENCE_HUB) : NULL;
    if (!s || !s->process) {
        return false;
    }
    for (unsigned n = 0; n < s->slot_count; ++n) {
        WddmFenceSlot *slot = &s->slots[n];
        if (mr == &slot->region && slot->source) {
            *process = s->process;
            *source = (void *)(uintptr_t)(slot->source & ~4095ULL);
            return true;
        }
    }
    return false;
}

static uint64_t slot_read(void *opaque, hwaddr address, unsigned size)
{
    WddmFenceSlot *slot = opaque;
    uint64_t value = 0;
    SIZE_T copied = 0;
    ++slot->hub->emulated_reads;
    if (!slot->source || address > 4096 || size > 4096 - address ||
        !ReadProcessMemory(slot->hub->process,
                           (void *)(uintptr_t)((slot->source & ~4095ULL) + address),
                           &value, size, &copied) || copied != size) {
        error_report("WDDM fence hub: emulated read failed");
        return UINT64_MAX;
    }
    return value;
}
static void slot_write(void *opaque, hwaddr address, uint64_t value, unsigned size)
{
    /* Guest writes cannot update a driver page, including through emulation. */
}
static const MemoryRegionOps slot_ops = {
    .read = slot_read, .write = slot_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void hub_set_mapping(Object *object, const char *value, Error **errp)
{
    WddmFenceHub *s = WDDM_FENCE_HUB(object);
    WddmFenceCommand command;
    SIZE_T copied = 0;
    uint64_t observed = 0;
    if (!DEVICE(s)->realized || !s->process || !wddm_fence_command_for_slots(value, s->slot_count, &command)) {
        error_setg(errp, "Invalid host fence mapping command or unrealized device");
        return;
    }
    WddmFenceSlot *slot = &s->slots[command.slot];
    if (!wddm_fence_transition(&command, s->last_generation, slot->generation)) {
        error_setg(errp, "Fence slot occupied, absent or generation stale");
        return;
    }
    if (command.map) {
        if (!ReadProcessMemory(s->process, (void *)(uintptr_t)command.source,
                               &observed, sizeof observed, &copied) || copied != sizeof observed) {
            error_setg(errp, "Host fence source is not readable in its fixed owner process");
            return;
        }
        slot->source = command.source;
        slot->generation = command.generation;
        s->last_generation = command.generation;
        memory_region_transaction_begin();
        memory_region_set_enabled(&slot->region, true);
        memory_region_transaction_commit();
        ++s->mapped;
    } else {
        /* Keep source identity valid until the WHPX listener has removed its
         * foreign mapping. The QMP reply is sent after this transaction. */
        memory_region_transaction_begin();
        memory_region_set_enabled(&slot->region, false);
        memory_region_transaction_commit();
        slot->source = slot->generation = 0;
        --s->mapped;
    }
}
static char *hub_get_state(Object *object, Error **errp)
{
    WddmFenceHub *s = WDDM_FENCE_HUB(object);
    /* Host diagnostic only. No source process addresses appear here. */
    return g_strdup_printf("%u:%" PRIu64 ":%u", s->mapped, s->last_generation, s->emulated_reads);
}
static uint32_t hub_config_read(PCIDevice *pci, uint32_t address, int size)
{
    WddmFenceHub *s = WDDM_FENCE_HUB(pci);
    pci_set_long(pci->config + 0x48, s->emulated_reads);
    return pci_default_read_config(pci, address, size);
}

static void hub_realize(PCIDevice *pci, Error **errp)
{
    WddmFenceHub *s = WDDM_FENCE_HUB(pci);
    HMODULE whp = GetModuleHandleW(L"WinHvPlatform.dll");
    if (!wddm_fence_slots_valid(s->slot_count)) {
        error_setg(errp, "WDDM fence slot-count must be 64, 128, 256 or 512");
        return;
    }
    if (!whpx_enabled() || !whp || !GetProcAddress(whp, "WHvMapGpaRange2")) {
        error_setg(errp, "WDDM fence hub requires WHPX and WHvMapGpaRange2");
        return;
    }
    if (!s->source_process ||
        !DuplicateHandle(GetCurrentProcess(), (HANDLE)(uintptr_t)s->source_process,
                         GetCurrentProcess(), &s->process,
                         PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, 0)) {
        error_setg(errp, "Cannot duplicate the inherited fence-owner process handle");
        return;
    }
    error_setg(&s->migration_blocker, "WDDM fence hub mappings are not migratable");
    if (migrate_add_blocker(&s->migration_blocker, errp)) {
        return;
    }
    memory_region_init(&s->bar, OBJECT(s), "wddm-fence-hub", s->slot_count * 4096);
    for (unsigned n = 0; n < s->slot_count; ++n) {
        WddmFenceSlot *slot = &s->slots[n];
        char name[48]; snprintf(name, sizeof name, "wddm-fence-slot-%u", n);
        slot->hub = s;
        memory_region_init_io(&slot->region, OBJECT(s), &slot_ops, slot, name, 4096);
        memory_region_set_readonly(&slot->region, true);
        memory_region_set_enabled(&slot->region, false);
        memory_region_add_subregion(&s->bar, n * 4096, &slot->region);
    }
    pci_register_bar(pci, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);
    pci_set_long(pci->config + 0x40, 0x31484e46); /* FNH1 */
    pci_set_long(pci->config + 0x44, s->slot_count);
}
static void hub_finalize(Object *object)
{
    WddmFenceHub *s = WDDM_FENCE_HUB(object);
    if (s->process) {
        CloseHandle(s->process);
    }
    if (s->migration_blocker) {
        migrate_del_blocker(&s->migration_blocker);
    }
}
static const Property hub_properties[] = {
    DEFINE_PROP_UINT64("source-process", WddmFenceHub, source_process, 0),
    DEFINE_PROP_UINT32("slot-count", WddmFenceHub, slot_count, WDDM_FENCE_HUB_PAGES),
};
static void hub_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    pc->realize = hub_realize; pc->config_read = hub_config_read;
    pc->vendor_id = 0x1234; pc->device_id = 0x11fd; pc->class_id = PCI_CLASS_OTHERS;
    dc->desc = "Dynamic read-only Windows driver fence hub";
    dc->hotpluggable = false;
    device_class_set_props(dc, hub_properties);
    object_class_property_add_str(klass, "fence-mapping", NULL, hub_set_mapping);
    object_class_property_add_str(klass, "fence-state", hub_get_state, NULL);
}
static const TypeInfo hub_info = {
    .name = TYPE_WDDM_FENCE_HUB, .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(WddmFenceHub), .instance_finalize = hub_finalize,
    .class_init = hub_class_init,
    .interfaces = (const InterfaceInfo[]) { { INTERFACE_CONVENTIONAL_PCI_DEVICE }, { } },
};
static void register_hub(void) { type_register_static(&hub_info); }
type_init(register_hub);
