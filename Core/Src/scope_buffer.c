#include "scope_buffer.h"

#include "scope.h"
#include "main.h"

enum
{
    SCOPE_DMA_BUFFER_COUNT = 2U
};

typedef struct
{
    uint8_t order[SCOPE_DMA_BUFFER_COUNT];
    uint8_t head;
    uint8_t tail;
    uint8_t pending;
    uint32_t overruns;
} ScopeDmaFrameQueue;

static ScopeDmaFrameQueue scope_dma_queue = {0};
static uint16_t adc_dma_buf[SCOPE_DMA_BUFFER_COUNT][SCOPE_FRAME_SAMPLES];
static volatile uint8_t scope_frame_ready = 0U;

void ScopeBuffer_Init(void)
{
    scope_dma_queue.head = 0U;
    scope_dma_queue.tail = 0U;
    scope_dma_queue.pending = 0U;
    scope_dma_queue.overruns = 0U;
    scope_frame_ready = 0U;
}

void ScopeBuffer_EnqueueFromISR(uint8_t buffer_index)
{
    if (scope_dma_queue.pending == SCOPE_DMA_BUFFER_COUNT)
    {
        scope_dma_queue.tail = (uint8_t)((scope_dma_queue.tail + 1U) % SCOPE_DMA_BUFFER_COUNT);
        scope_dma_queue.pending--;
        scope_dma_queue.overruns++;
    }

    scope_dma_queue.order[scope_dma_queue.head] = buffer_index;
    scope_dma_queue.head = (uint8_t)((scope_dma_queue.head + 1U) % SCOPE_DMA_BUFFER_COUNT);
    scope_dma_queue.pending++;
    scope_frame_ready = 1U;
}

uint16_t *ScopeBuffer_Dequeue(uint16_t *frame_samples)
{
    uint16_t *buffer = NULL;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (scope_dma_queue.pending > 0U)
    {
        uint8_t buffer_index = scope_dma_queue.order[scope_dma_queue.tail];
        scope_dma_queue.tail = (uint8_t)((scope_dma_queue.tail + 1U) % SCOPE_DMA_BUFFER_COUNT);
        scope_dma_queue.pending--;
        buffer = adc_dma_buf[buffer_index];
    }

    if (scope_dma_queue.pending == 0U)
    {
        scope_frame_ready = 0U;
    }

    if (primask == 0U)
    {
        __enable_irq();
    }

    if (frame_samples != NULL && buffer != NULL)
    {
        *frame_samples = SCOPE_FRAME_SAMPLES;
    }

    return buffer;
}

uint32_t ScopeBuffer_GetOverrunCount(void)
{
    uint32_t count;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    count = scope_dma_queue.overruns;
    if (primask == 0U)
    {
        __enable_irq();
    }
    return count;
}

uint8_t ScopeBuffer_HasPending(void)
{
    return scope_frame_ready;
}

uint16_t *ScopeBuffer_GetDmaBaseAddress(void)
{
    return &adc_dma_buf[0][0];
}
