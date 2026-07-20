// The smallest lab binary: prove the build -> ship -> load -> ring-0-call ->
// api->print path works end to end. Run it with lab.run("hello.elf").
#include <lab.h>

long bench(const lab_api* api, long arg)
{
    (void)arg;
    api->print("LAB_OK\n");
    return 0;
}
