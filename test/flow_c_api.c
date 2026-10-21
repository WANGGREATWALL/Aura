#include <stdlib.h>
#include <string.h>

#include "flow/xflow_api.h"
static int  releases = 0;
static int  body(void* data) { return *(int*)data; }
static void destroy(void* data)
{
    ++releases;
    free(data);
}
static void tile(size_t begin, size_t end, void* data)
{
    size_t i;
    for (i = begin; i < end; ++i)
        ((int*)data)[i] = 1;
}
int main(void)
{
    int*             value = (int*)malloc(sizeof(int));
    XFlowTaskHandle  first, second;
    XFlowGroupHandle group;
    int              output[17] = {0};
    if (!value)
        return 1;
    *value = -123;
    if (AURA_FLOW_ABI_VERSION != 3u || XFlow_GetWorkerCount() != 0)
        return 2;
    first  = XFlow_CreateTask("first", body, value, destroy);
    second = XFlow_CreateTask("second", body, NULL, NULL);
    group  = XFlow_CreateGroup("C99");
    if (!first || !second || !group)
        return 3;
    if (XFlow_AddDependency(first, second) || XFlow_GroupAddTask(group, first) || XFlow_GroupAddTask(group, second))
        return 4;
    XFlow_RetainTask(first);
    XFlow_ReleaseTask(first);
    XFlow_RetainGroup(group);
    XFlow_ReleaseGroup(group);
    if (XFlow_EnqueueGroup(group) || XFlow_EnqueueGroup(group))
        return 5;
    if (XFlow_WaitTask(second) != -123 || XFlow_WaitGroup(group) != -123 || XFlow_GetTaskResult(second) != -123 ||
        XFlow_WaitAll() != -123)
        return 6;
    if (!strstr(XFlow_DumpGroupDot(group), "first") || releases != 1)
        return 7;
    XFlow_ReleaseTask(first);
    XFlow_ReleaseTask(second);
    XFlow_ReleaseGroup(group);
    XFlow_ParallelizeTile(17, 3, tile, output);
    {
        size_t i;
        for (i = 0; i < 17; ++i)
            if (output[i] != 1)
                return 8;
    }
    return XFlow_GetWorkerCount() > 8;
}
