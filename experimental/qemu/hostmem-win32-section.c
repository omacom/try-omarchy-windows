/* SPDX-License-Identifier: MIT */
/* Independent Windows section backend for the disk-free WDDM bridge lab. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "system/hostmem.h"
#include "migration/blocker.h"

#define TYPE_MEMORY_BACKEND_WIN32_SECTION "memory-backend-win32-section"
OBJECT_DECLARE_SIMPLE_TYPE(HostMemoryBackendWin32Section,
                           MEMORY_BACKEND_WIN32_SECTION)

struct HostMemoryBackendWin32Section {
    HostMemoryBackend parent_obj;
    char *section_name;
    HANDLE section;
    void *view;
    Error *migration_blocker;
};

static bool valid_section_name(const char *name)
{
    const char *prefix = "Local\\7Wdev-WDDM-";
    size_t length = strlen(prefix);
    unsigned i;

    if (!name || strncmp(name, prefix, length) || strlen(name + length) != 32) {
        return false;
    }
    for (i = 0; i < 32; ++i) {
        char c = name[length + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool section_alloc(HostMemoryBackend *backend, Error **errp)
{
    HostMemoryBackendWin32Section *s = MEMORY_BACKEND_WIN32_SECTION(backend);
    g_autofree gunichar2 *wide_name = NULL;
    g_autofree char *ram_name = NULL;
    DWORD error;

    if (!valid_section_name(s->section_name)) {
        error_setg(errp, "section-name must be Local\\7Wdev-WDDM- plus 32 lowercase hex digits");
        return false;
    }
    if (!backend->size || backend->size > GiB || backend->size % 4096 ||
        !backend->share || backend->guest_memfd) {
        error_setg(errp, "section memory requires share=on, page-aligned size 4K..1G, no guest-memfd");
        return false;
    }
    wide_name = g_utf8_to_utf16(s->section_name, -1, NULL, NULL, NULL);
    SetLastError(ERROR_SUCCESS);
    s->section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                   (DWORD)(backend->size >> 32), (DWORD)backend->size,
                                   (LPCWSTR)wide_name);
    error = GetLastError();
    if (!s->section || error == ERROR_ALREADY_EXISTS) {
        error_setg(errp, "Cannot create a new Windows section (error %lu)", error);
        return false;
    }
    s->view = MapViewOfFile(s->section, FILE_MAP_READ | FILE_MAP_WRITE,
                           0, 0, backend->size);
    if (!s->view) {
        error_setg(errp, "Cannot map Windows section (error %lu)", GetLastError());
        return false;
    }
    error_setg(&s->migration_blocker, "Windows WDDM shared-memory lab is not migratable");
    if (migrate_add_blocker(&s->migration_blocker, errp)) {
        return false;
    }
    ram_name = host_memory_backend_get_name(backend);
    backend->aligned = true;
    memory_region_init_ram_ptr_flags(&backend->mr, OBJECT(backend), ram_name,
                                     backend->size, s->view, RAM_SHARED);
    return true;
}

static char *get_section_name(Object *object, Error **errp)
{
    return g_strdup(MEMORY_BACKEND_WIN32_SECTION(object)->section_name);
}

static void set_section_name(Object *object, const char *name, Error **errp)
{
    HostMemoryBackendWin32Section *s = MEMORY_BACKEND_WIN32_SECTION(object);
    if (host_memory_backend_mr_inited(MEMORY_BACKEND(object))) {
        error_setg(errp, "Cannot change section-name after allocation");
        return;
    }
    g_free(s->section_name);
    s->section_name = g_strdup(name);
}

static void section_init(Object *object)
{
    MEMORY_BACKEND(object)->share = true;
}

static void section_finalize(Object *object)
{
    HostMemoryBackendWin32Section *s = MEMORY_BACKEND_WIN32_SECTION(object);
    /* QOM has finalized child memory regions before the owner's finalizer.
     * RAM_PREALLOC prevents QEMU from freeing the externally owned view. */
    if (s->view) {
        UnmapViewOfFile(s->view);
    }
    if (s->section) {
        CloseHandle(s->section);
    }
    if (s->migration_blocker) {
        migrate_del_blocker(&s->migration_blocker);
    }
    g_free(s->section_name);
}

static void section_class_init(ObjectClass *class, const void *data)
{
    MEMORY_BACKEND_CLASS(class)->alloc = section_alloc;
    object_class_property_add_str(class, "section-name", get_section_name,
                                  set_section_name);
}

static const TypeInfo section_info = {
    .name = TYPE_MEMORY_BACKEND_WIN32_SECTION,
    .parent = TYPE_MEMORY_BACKEND,
    .instance_size = sizeof(HostMemoryBackendWin32Section),
    .instance_init = section_init,
    .instance_finalize = section_finalize,
    .class_init = section_class_init,
};

static void register_section(void)
{
    type_register_static(&section_info);
}

type_init(register_section);
