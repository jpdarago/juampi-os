#include <kmodule.h>
#include <limine.h>
#include <str.h>

#include <stdbool.h>

__attribute__((used, section(".limine_requests"))) static volatile struct
        limine_module_request module_request = {.id = LIMINE_MODULE_REQUEST,
                                                .revision = 0};

static struct limine_module_response* resp(void)
{
    return module_request.response;
}

size_t kmodule_count(void)
{
    return resp() != NULL ? resp()->module_count : 0;
}

const char* kmodule_path(size_t i)
{
    return i < kmodule_count() ? resp()->modules[i]->path : "";
}

const void* kmodule_data(size_t i, size_t* size)
{
    if (i >= kmodule_count()) {
        return NULL;
    }
    struct limine_file* f = resp()->modules[i];
    if (size) {
        *size = f->size;
    }
    return f->address;
}
const void* kmodule_find(const char* name, size_t* size)
{
    for (size_t i = 0; i < kmodule_count(); i++) {
        if (str_has_suffix(str_from(resp()->modules[i]->path),
                           str_from(name))) {
            return kmodule_data(i, size);
        }
    }
    return NULL;
}
