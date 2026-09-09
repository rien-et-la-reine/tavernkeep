#include <stdint.h>
#include "storage/block_device.h"
#include "test_check.h"

static struct {
    size_t calls;
    void *context;
    uint64_t lba;
    const void *buffer;
    size_t count;
    block_device_result_t result;
} spy;

static block_device_result_t lifecycle(void *context)
{
    spy.calls++;
    spy.context = context;
    return spy.result;
}
static block_device_result_t read_blocks(void *context, uint64_t lba,
    void *buffer, size_t count)
{
    spy.lba = lba; spy.buffer = buffer; spy.count = count;
    return lifecycle(context);
}
static block_device_result_t write_blocks(void *context, uint64_t lba,
    const void *buffer, size_t count)
{
    spy.lba = lba; spy.buffer = buffer; spy.count = count;
    return lifecycle(context);
}
static block_device_result_t get_info(void *context, block_device_info_t *info)
{
    spy.buffer = info;
    return lifecycle(context);
}

static void rejects_all(const block_device_t *device)
{
    unsigned char buffer = 0;
    block_device_info_t info = {0};
    size_t before = spy.calls;
    REQUIRE(block_device_init(device) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    REQUIRE(block_device_deinit(device) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    REQUIRE(block_device_read_blocks(device, 0, &buffer, 1) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    REQUIRE(block_device_write_blocks(device, 0, &buffer, 1) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    REQUIRE(block_device_get_info(device, &info) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    REQUIRE(spy.calls == before);
}

int main(void)
{
    int context = 7;
    block_device_operations_t operations = {
        lifecycle, lifecycle, read_blocks, write_blocks, get_info
    };
    block_device_t device = { &context, &operations };
    const block_device_t malformed[] = {
        {NULL, &operations}, {&context, NULL}, {NULL, NULL}
    };
    rejects_all(NULL);
    REQUIRE(!block_device_is_valid(NULL));
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        REQUIRE(!block_device_is_valid(&malformed[i]));
        rejects_all(&malformed[i]);
    }
    block_device_operations_t empty = {0};
    block_device_info_t missing_info = {0};
    block_device_t partial = { &context, &empty };
    REQUIRE(block_device_is_valid(&partial));
    rejects_all(&partial);
    /* One missing callback must reject only its own operation. The second half
     * of that claim needs asserting too: a NULL `init` must not stop `read`
     * from dispatching, or a partially populated table would silently disable
     * more than it should. Each iteration nulls one entry, checks that call is
     * rejected without dispatching, then checks the other four still reach the
     * backend. */
    spy.result = BLOCK_DEVICE_RESULT_OK;
    for (unsigned int i = 0; i < 5; ++i) {
        empty = operations;
        size_t before = spy.calls;
        switch (i) {
        case 0: empty.init = NULL; REQUIRE(block_device_init(&partial) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT); break;
        case 1: empty.deinit = NULL; REQUIRE(block_device_deinit(&partial) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT); break;
        case 2: empty.read_blocks = NULL; REQUIRE(block_device_read_blocks(&partial, 0, &context, 1) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT); break;
        case 3: empty.write_blocks = NULL; REQUIRE(block_device_write_blocks(&partial, 0, &context, 1) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT); break;
        default: empty.get_info = NULL; REQUIRE(block_device_get_info(&partial, &missing_info) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT); break;
        }
        /* The rejected call never reached the backend. */
        REQUIRE(spy.calls == before);
        /* Every other operation still does. */
        if (i != 0) { REQUIRE(block_device_init(&partial) == BLOCK_DEVICE_RESULT_OK); }
        if (i != 1) { REQUIRE(block_device_deinit(&partial) == BLOCK_DEVICE_RESULT_OK); }
        if (i != 2) { REQUIRE(block_device_read_blocks(&partial, 0, &context, 1) == BLOCK_DEVICE_RESULT_OK); }
        if (i != 3) { REQUIRE(block_device_write_blocks(&partial, 0, &context, 1) == BLOCK_DEVICE_RESULT_OK); }
        if (i != 4) { REQUIRE(block_device_get_info(&partial, &missing_info) == BLOCK_DEVICE_RESULT_OK); }
        REQUIRE(spy.calls == before + 4);
    }
    REQUIRE(spy.calls == 20);
    spy.calls = 0;
    REQUIRE(block_device_read_blocks(&device, 0, NULL, 1) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    REQUIRE(block_device_read_blocks(&device, 0, &context, 0) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    REQUIRE(block_device_write_blocks(&device, 0, NULL, 1) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    REQUIRE(block_device_write_blocks(&device, 0, &context, 0) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    REQUIRE(block_device_get_info(&device, NULL) == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    REQUIRE(spy.calls == 0);

    const block_device_result_t results[] = {
        BLOCK_DEVICE_RESULT_OK, BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        BLOCK_DEVICE_RESULT_NOT_INITIALIZED, BLOCK_DEVICE_RESULT_OUT_OF_RANGE,
        BLOCK_DEVICE_RESULT_IO_ERROR, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT,
        BLOCK_DEVICE_RESULT_INVALID_DEVICE, BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED
    };
    /* The generic wrapper must not truncate, impose SD limits, or remap errors. */
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        block_device_info_t info = {0};
        spy.result = results[i];
        size_t before = spy.calls;
        REQUIRE(block_device_init(&device) == results[i]);
        REQUIRE(block_device_deinit(&device) == results[i]);
        REQUIRE(block_device_read_blocks(&device, UINT64_MAX, &context, SIZE_MAX) == results[i]);
        REQUIRE(spy.lba == UINT64_MAX && spy.buffer == &context && spy.count == SIZE_MAX);
        REQUIRE(block_device_write_blocks(&device, UINT64_MAX - 1, &context, SIZE_MAX - 1) == results[i]);
        REQUIRE(spy.lba == UINT64_MAX - 1 && spy.buffer == &context && spy.count == SIZE_MAX - 1);
        REQUIRE(block_device_get_info(&device, &info) == results[i]);
        REQUIRE(spy.buffer == &info && spy.context == &context && spy.calls == before + 5);
    }
    puts("PASS block-device validation, forwarding and all result categories");
    return 0;
}
