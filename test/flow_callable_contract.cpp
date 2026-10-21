#include "flow/xflow.h"

// Compile only: ordinary async tasks must return exactly int, not void/long.
int main()
{
#if FLOW_RETURN_KIND == 0
    (void)au::flow::XFlow::get().enqueue([]() {});
#elif FLOW_RETURN_KIND == 1
    (void)au::flow::XFlow::get().enqueue([]() -> long { return 0; });
#else
    (void)au::flow::XFlow::get().enqueue([]() -> int { return 0; });
#endif
}
