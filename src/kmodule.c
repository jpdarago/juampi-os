#include <kmodule.h>
#include <limine.h>

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

static size_t slen(const char* s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

// True if `s` ends with `suffix`.
static bool ends_with(const char* s, const char* suffix)
{
    size_t ls = slen(s), lf = slen(suffix);
    if (lf > ls) {
        return false;
    }
    s += ls - lf;
    for (size_t i = 0; i < lf; i++) {
        if (s[i] != suffix[i]) {
            return false;
        }
    }
    return true;
}

const void* kmodule_find(const char* name, size_t* size)
{
    for (size_t i = 0; i < kmodule_count(); i++) {
        if (ends_with(resp()->modules[i]->path, name)) {
            return kmodule_data(i, size);
        }
    }
    return NULL;
}
