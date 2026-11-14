/* SPDX-License-Identifier: MIT */
/* Bounded writable mappings of native-owned Lock2 memory. Diagnostic only. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/wddm-allocation-hub.h"
#include "hw/misc/wddm-allocation-map.h"
#include "system/whpx.h"
#include "migration/blocker.h"
#define TYPE_WDDM_ALLOCATION_HUB "wddm-allocation-hub"
OBJECT_DECLARE_SIMPLE_TYPE(WddmAllocationHub, WDDM_ALLOCATION_HUB)
typedef struct WddmAllocationSlot {
    MemoryRegion region;
    struct WddmAllocationHub *hub;
    uint64_t source, bytes, generation;
} WddmAllocationSlot;
struct WddmAllocationHub {
    PCIDevice parent_obj;
    MemoryRegion bar;
    WddmAllocationSlot slots[WDDM_ALLOCATION_HUB_MAX_SLOTS];
    uint32_t slot_count;
    uint64_t source_process, last_generation, mapped_bytes;
    HANDLE process;
    uint32_t emulated_reads, emulated_writes, mapped;
    Error *migration_blocker;
};
bool wddm_allocation_hub_mapping(MemoryRegion *mr, void **process,
                                  void **source, uint64_t *bytes)
{
    Object *owner = memory_region_owner(mr);
    WddmAllocationHub *s = owner ? (WddmAllocationHub *)object_dynamic_cast(owner, TYPE_WDDM_ALLOCATION_HUB) : NULL;
    if (!s || !s->process) {
        return false;
    }
    for (unsigned n = 0; n < s->slot_count; ++n) {
        WddmAllocationSlot *slot = &s->slots[n];
        if (mr == &slot->region && slot->source && slot->bytes) {
            *process = s->process;
            *source = (void *)(uintptr_t)slot->source;
            *bytes = slot->bytes;
            return true;
        }
    }
    return false;
}
static uint64_t slot_read(void *opaque, hwaddr address, unsigned size)
{
    WddmAllocationSlot *slot = opaque;
    uint64_t value = 0;
    SIZE_T copied = 0;
    ++slot->hub->emulated_reads;
    if (!slot->source || !slot->bytes || address > slot->bytes || size > slot->bytes - address ||
        !ReadProcessMemory(slot->hub->process, (void *)(uintptr_t)(slot->source + address),
                           &value, size, &copied) || copied != size) {
        error_report("WDDM allocation hub: emulated read failed");
        abort();
    }
    return value;
}
static void slot_write(void *opaque, hwaddr address, uint64_t value, unsigned size)
{
    WddmAllocationSlot *slot = opaque;
    SIZE_T copied = 0;
    ++slot->hub->emulated_writes;
    if (!slot->source || !slot->bytes || address > slot->bytes || size > slot->bytes - address ||
        !WriteProcessMemory(slot->hub->process, (void *)(uintptr_t)(slot->source + address),
                            &value, size, &copied) || copied != size) {
        error_report("WDDM allocation hub: emulated write failed");
        abort();
    }
}
static const MemoryRegionOps slot_ops = {
    .read = slot_read, .write = slot_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};
static void hub_set_mapping(Object *object, const char *value, Error **errp)
{
    WddmAllocationHub *s = WDDM_ALLOCATION_HUB(object);
    WddmAllocationCommand command;
    SIZE_T copied = 0;
    uint64_t observed = 0;
    if (!DEVICE(s)->realized || !s->process || !wddm_allocation_command_for_slots(value, s->slot_count, &command)) {
        error_setg(errp, "Invalid host allocation mapping command or unrealized device");
        return;
    }
    WddmAllocationSlot *slot = &s->slots[command.slot];
    if (!wddm_allocation_transition(&command, s->last_generation, slot->generation)) {
        error_setg(errp, "Allocation slot occupied, absent or generation stale");
        return;
    }
    if (command.map) {
        if (!ReadProcessMemory(s->process, (void *)(uintptr_t)command.source,
                               &observed, sizeof observed, &copied) || copied != sizeof observed ||
            !ReadProcessMemory(s->process, (void *)(uintptr_t)(command.source + command.bytes - sizeof observed),
                               &observed, sizeof observed, &copied) || copied != sizeof observed) {
            error_setg(errp, "Host allocation range is not readable in its fixed owner process");
            return;
        }
        for (unsigned n = 0; n < s->slot_count; ++n) {
            WddmAllocationSlot *other = &s->slots[n];
            if (other->source && command.source < other->source + other->bytes &&
                other->source < command.source + command.bytes) {
                error_setg(errp, "Host allocation source overlaps an active mapping");
                return;
            }
        }
        slot->source = command.source; slot->bytes = command.bytes;
        slot->generation = command.generation; s->last_generation = command.generation;
        memory_region_transaction_begin();
        memory_region_set_size(&slot->region, command.bytes);
        memory_region_set_enabled(&slot->region, true);
        memory_region_transaction_commit();
        ++s->mapped; s->mapped_bytes += command.bytes;
    } else {
        /* Source identity survives until WHPX removes its foreign GPA mapping.
         * The QMP acknowledgement occurs after the transaction commits. */
        memory_region_transaction_begin();
        memory_region_set_enabled(&slot->region, false);
        memory_region_transaction_commit();
        --s->mapped; s->mapped_bytes -= slot->bytes;
        slot->source = slot->bytes = slot->generation = 0;
    }
}
static char *hub_get_state(Object *object, Error **errp)
{
    WddmAllocationHub *s = WDDM_ALLOCATION_HUB(object);
    return g_strdup_printf("%u:%" PRIu64 ":%" PRIu64 ":%u:%u", s->mapped, s->mapped_bytes,
                           s->last_generation, s->emulated_reads, s->emulated_writes);
}
static uint32_t hub_config_read(PCIDevice *pci, uint32_t address, int size)
{
    WddmAllocationHub *s = WDDM_ALLOCATION_HUB(pci);
    pci_set_long(pci->config + 0x4c, s->emulated_reads);
    pci_set_long(pci->config + 0x50, s->emulated_writes);
    return pci_default_read_config(pci, address, size);
}
static void hub_realize(PCIDevice *pci, Error **errp)
{
    WddmAllocationHub *s = WDDM_ALLOCATION_HUB(pci);
    HMODULE whp = GetModuleHandleW(L"WinHvPlatform.dll");
    if (!wddm_allocation_slots_valid(s->slot_count)) {
        error_setg(errp, "WDDM allocation slot-count must be 16, 32, 64, 128, 256, 512 or 1024");
        return;
    }
    if (!whpx_enabled() || !whp || !GetProcAddress(whp, "WHvMapGpaRange2")) {
        error_setg(errp, "WDDM allocation hub requires WHPX and WHvMapGpaRange2");
        return;
    }
    if (!s->source_process ||
        !DuplicateHandle(GetCurrentProcess(), (HANDLE)(uintptr_t)s->source_process, GetCurrentProcess(),
                         &s->process, PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, 0)) {
        error_setg(errp, "Cannot duplicate the inherited allocation-owner process handle");
        return;
    }
    error_setg(&s->migration_blocker, "WDDM allocation hub mappings are not migratable");
    if (migrate_add_blocker(&s->migration_blocker, errp)) {
        return;
    }
    memory_region_init(&s->bar, OBJECT(s), "wddm-allocation-hub",
                       s->slot_count * WDDM_ALLOCATION_SLOT_BYTES);
    for (unsigned n = 0; n < s->slot_count; ++n) {
        WddmAllocationSlot *slot = &s->slots[n];
        char name[48]; snprintf(name, sizeof name, "wddm-allocation-slot-%u", n);
        slot->hub = s;
        memory_region_init_io(&slot->region, OBJECT(s), &slot_ops, slot, name, WDDM_ALLOCATION_SLOT_BYTES);
        memory_region_set_enabled(&slot->region, false);
        memory_region_add_subregion(&s->bar, n * WDDM_ALLOCATION_SLOT_BYTES, &slot->region);
    }
    /* The explicit 1 GiB aperture must fit beside RAM and legacy PCI BARs.
     * Preserve the published 32-bit BAR layout for all smaller profiles. */
    pci_register_bar(pci, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
        (s->slot_count==1024 ? PCI_BASE_ADDRESS_MEM_TYPE_64 : 0), &s->bar);
    pci_set_long(pci->config + 0x40, 0x31484c41); /* ALH1 */
    pci_set_long(pci->config + 0x44, s->slot_count);
    pci_set_long(pci->config + 0x48, WDDM_ALLOCATION_SLOT_BYTES);
}
static void hub_finalize(Object *object)
{
    WddmAllocationHub *s = WDDM_ALLOCATION_HUB(object);
    if (s->process) { CloseHandle(s->process); }
    if (s->migration_blocker) { migrate_del_blocker(&s->migration_blocker); }
}
static const Property hub_properties[] = {
    DEFINE_PROP_UINT64("source-process", WddmAllocationHub, source_process, 0),
    DEFINE_PROP_UINT32("slot-count", WddmAllocationHub, slot_count, WDDM_ALLOCATION_HUB_DEFAULT_SLOTS),
};
static void hub_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass); PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    pc->realize = hub_realize; pc->config_read = hub_config_read;
    pc->vendor_id = 0x1234; pc->device_id = 0x11fc; pc->class_id = PCI_CLASS_OTHERS;
    dc->desc = "Bounded writable Windows allocation hub"; dc->hotpluggable = false;
    device_class_set_props(dc, hub_properties);
    object_class_property_add_str(klass, "allocation-mapping", NULL, hub_set_mapping);
    object_class_property_add_str(klass, "allocation-state", hub_get_state, NULL);
}
static const TypeInfo hub_info = {
    .name = TYPE_WDDM_ALLOCATION_HUB, .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(WddmAllocationHub), .instance_finalize = hub_finalize,
    .class_init = hub_class_init,
    .interfaces = (const InterfaceInfo[]) { { INTERFACE_CONVENTIONAL_PCI_DEVICE }, { } },
};
static void register_hub(void) { type_register_static(&hub_info); }
type_init(register_hub);
