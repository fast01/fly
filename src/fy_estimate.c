#include "fy_estimate.h"

void fy_conn_estimate(fy_conn_pool *pool, fy_module *module)
{
    static int cnt = 0;

    cnt += 1;
    cnt %= 100;

    if (cnt == 0) {
        fy_log_info("module %s free conn: %d, usage_percent %.2f%%\n",
                module->module_name, fy_free_conns(pool), fy_conn_usage_percent(pool));
    }
}

static size_t max_interval = 0;
static size_t min_interval = 1000 * 1000 * 1000;
static size_t total_interval;
static size_t total_request;

void fy_request_estimate_begin(fy_request *r)
{
    gettimeofday(&r->info->request_begin, NULL);
}

void fy_request_estimate_end(fy_request *r)
{
    static int cnt = 0;

    size_t interval; // unit: micro second

    gettimeofday(&r->info->request_end, NULL);

    interval = (r->info->request_end.tv_sec - r->info->request_begin.tv_sec) * 1000 +
        (r->info->request_end.tv_usec - r->info->request_end.tv_usec) / 1000;

    if (interval > max_interval) {
        max_interval = interval;
    }
    if (interval < min_interval) {
        min_interval = interval;
    }
    total_interval += interval;
    ++total_request;

    ++cnt;
    cnt %= 100;

    if (cnt == 0) {
        fy_log_info("max_interval: %ld ms\n", max_interval);
        fy_log_info("min_interval: %ld ms\n", min_interval);
        fy_log_info("avg_interval: %.2f ms\n", total_interval * 1.0 / total_request);
    }
}

