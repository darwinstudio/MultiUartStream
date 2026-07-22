/**
 * @file multi_uart_stream.c
 * @brief MultiUartStream 库实现
 */

#include "multi_uart_stream.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* ========== 编译期安全校验 ========== */

/** 配置表中的 rx_buff_size 不得超过静态分配的 DMA 缓冲区 */
#define MUS_CHECK_RX_BUFF_SIZE(idx)                                                                                    \
    _Static_assert((idx) >= MUS_COUNT || mus_hw_table[idx].rx_buff_size <= MUS_RX_BUFFER_SIZE,                         \
                   "mus_hw_table[" #idx "].rx_buff_size exceeds MUS_RX_BUFFER_SIZE")

/** 配置表中的 stream_buff_size 不得超过静态分配的流缓冲区 */
#define MUS_CHECK_STREAM_SIZE(idx)                                                                                     \
    _Static_assert((idx) >= MUS_COUNT || mus_hw_table[idx].stream_buff_size <= MUS_STREAM_BUFF_SIZE,                   \
                   "mus_hw_table[" #idx "].stream_buff_size exceeds MUS_STREAM_BUFF_SIZE")

/* 展开最多 16 个实例的编译期检查（无运行时开销） */
MUS_CHECK_RX_BUFF_SIZE(0);
MUS_CHECK_RX_BUFF_SIZE(1);
MUS_CHECK_RX_BUFF_SIZE(2);
MUS_CHECK_RX_BUFF_SIZE(3);
MUS_CHECK_RX_BUFF_SIZE(4);
MUS_CHECK_RX_BUFF_SIZE(5);
MUS_CHECK_RX_BUFF_SIZE(6);
MUS_CHECK_RX_BUFF_SIZE(7);
MUS_CHECK_STREAM_SIZE(0);
MUS_CHECK_STREAM_SIZE(1);
MUS_CHECK_STREAM_SIZE(2);
MUS_CHECK_STREAM_SIZE(3);
MUS_CHECK_STREAM_SIZE(4);
MUS_CHECK_STREAM_SIZE(5);
MUS_CHECK_STREAM_SIZE(6);
MUS_CHECK_STREAM_SIZE(7);

/* ========== 内部实例结构体 ========== */

typedef struct
{
    const MUS_HwConfig_t *config;

    /* RX 相关（enable_rx=1 时有效） */
    StreamBufferHandle_t rx_stream;
    StaticStreamBuffer_t rx_stream_cb;
    uint8_t rx_stream_buf[MUS_STREAM_BUFF_SIZE + 1];
    uint8_t rx_dma_buf[MUS_RX_BUFFER_SIZE];

    /* TX 相关（enable_tx=1 时有效） */
    StreamBufferHandle_t tx_stream;
    StaticStreamBuffer_t tx_stream_cb;
    uint8_t tx_stream_buf[MUS_STREAM_BUFF_SIZE + 1];
    SemaphoreHandle_t tx_sem;
    StaticSemaphore_t tx_sem_cb;
    SemaphoreHandle_t tx_mutex;
    StaticSemaphore_t tx_mutex_cb;
} MUS_Instance_t;

static MUS_Instance_t s_instances[MUS_COUNT];

/* ========== 任务静态分配 ========== */

static StackType_t rx_task_stack[MUS_COUNT][MUS_TASK_STACK_SIZE];
static StaticTask_t rx_task_tcb[MUS_COUNT];
static StackType_t tx_task_stack[MUS_COUNT][MUS_TASK_STACK_SIZE];
static StaticTask_t tx_task_tcb[MUS_COUNT];
static char rx_task_name[MUS_COUNT][configMAX_TASK_NAME_LEN];
static char tx_task_name[MUS_COUNT][configMAX_TASK_NAME_LEN];

/* ========== 内部辅助函数 ========== */

/**
 * @brief 启动 DMA + IDLE 检测接收
 */
static void open_rx_idle(MUS_Instance_t *inst)
{
    UART_HandleTypeDef *huart = inst->config->huart;

    __HAL_UART_CLEAR_OREFLAG(huart);

    if (huart->RxState != HAL_UART_STATE_READY)
    {
        HAL_UART_AbortReceive(huart);
    }

    HAL_StatusTypeDef status =
        HAL_UARTEx_ReceiveToIdle_DMA(huart, inst->rx_dma_buf, inst->config->rx_buff_size);
    if (status != HAL_OK)
    {
        HAL_UART_AbortReceive(huart);
        HAL_UARTEx_ReceiveToIdle_DMA(huart, inst->rx_dma_buf, inst->config->rx_buff_size);
        return;
    }
    __HAL_DMA_DISABLE_IT(huart->hdmarx, DMA_IT_HT);
}

/* ========== HAL 回调 ========== */

/**
 * @brief DMA 接收完成回调（IDLE 检测触发）
 */
static void rx_event_callback(UART_HandleTypeDef *huart, uint16_t len)
{
    for (uint8_t i = 0; i < MUS_COUNT; i++)
    {
        if (s_instances[i].config != NULL && s_instances[i].config->huart == huart)
        {
            if (s_instances[i].rx_stream == NULL)
            {
                return;
            }

            BaseType_t xTaskWoken = pdFALSE;
            xStreamBufferSendFromISR(s_instances[i].rx_stream, s_instances[i].rx_dma_buf, len, &xTaskWoken);

            open_rx_idle(&s_instances[i]);

            portEND_SWITCHING_ISR(xTaskWoken);
            return;
        }
    }
}

/**
 * @brief DMA 发送完成回调
 */
static void tx_cplt_callback(UART_HandleTypeDef *huart)
{
    for (uint8_t i = 0; i < MUS_COUNT; i++)
    {
        if (s_instances[i].config != NULL && s_instances[i].config->huart == huart)
        {
            if (s_instances[i].tx_sem == NULL)
            {
                return;
            }

            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(s_instances[i].tx_sem, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
            return;
        }
    }
}

/**
 * @brief UART 错误回调
 */
static void uart_error_callback(UART_HandleTypeDef *huart)
{
    for (uint8_t i = 0; i < MUS_COUNT; i++)
    {
        if (s_instances[i].config != NULL && s_instances[i].config->huart == huart)
        {
            if (s_instances[i].config->enable_rx)
            {
                HAL_UART_AbortReceive(huart);
                open_rx_idle(&s_instances[i]);
            }
            return;
        }
    }
}

/* ========== RX 任务 ========== */

static void rx_task_entry(void *para)
{
    MUS_Id_e id = (MUS_Id_e)(uint32_t)para;
    uint8_t buf[MUS_RX_BUFFER_SIZE];

    for (;;)
    {
        size_t len = xStreamBufferReceive(s_instances[id].rx_stream, buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (len > 0)
        {
            MUS_RxCallback(id, buf, len);
        }
    }
}

/* ========== TX 任务 ========== */

static void tx_task_entry(void *para)
{
    MUS_Id_e id = (MUS_Id_e)(uint32_t)para;
    uint8_t buf[MUS_STREAM_BUFF_SIZE];

    for (;;)
    {
        size_t len = xStreamBufferReceive(s_instances[id].tx_stream, buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (len > 0)
        {
            if (xSemaphoreTake(s_instances[id].tx_mutex, pdMS_TO_TICKS(2000)) == pdTRUE)
            {
                if (HAL_UART_Transmit_DMA(s_instances[id].config->huart, buf, (uint16_t)len) == HAL_OK)
                {
                    if (xSemaphoreTake(s_instances[id].tx_sem, pdMS_TO_TICKS(2000)) != pdTRUE)
                    {
                        HAL_UART_AbortTransmit(s_instances[id].config->huart);
                    }
                }
                xSemaphoreGive(s_instances[id].tx_mutex);
            }
        }
    }
}

/* ========== 单实例初始化 ========== */

static uint8_t init_instance(MUS_Id_e id)
{
    MUS_Instance_t *inst = &s_instances[id];
    inst->config = &mus_hw_table[id];

    /* 注册 HAL 回调（TX 完成 + 错误始终注册，RX 事件仅在启用时注册） */
    if (HAL_UART_RegisterCallback(inst->config->huart, HAL_UART_TX_COMPLETE_CB_ID, tx_cplt_callback) != HAL_OK)
    {
        return 0;
    }
    if (HAL_UART_RegisterCallback(inst->config->huart, HAL_UART_ERROR_CB_ID, uart_error_callback) != HAL_OK)
    {
        return 0;
    }

    /* RX 初始化 */
    if (inst->config->enable_rx)
    {
        if (HAL_UART_RegisterRxEventCallback(inst->config->huart, rx_event_callback) != HAL_OK)
        {
            return 0;
        }

        /* 静态创建永不失败，无需判空 */
        uint16_t stream_size = inst->config->stream_buff_size;
        inst->rx_stream = xStreamBufferCreateStatic(stream_size, 1, inst->rx_stream_buf, &inst->rx_stream_cb);

        open_rx_idle(inst);

        snprintf(rx_task_name[id], configMAX_TASK_NAME_LEN, "mus%u_rx", (unsigned)id);
        xTaskCreateStatic(rx_task_entry, rx_task_name[id], MUS_TASK_STACK_SIZE, (void *)(uint32_t)id,
                          MUS_RX_TASK_PRIORITY, rx_task_stack[id], &rx_task_tcb[id]);
    }

    /* TX 初始化 */
    if (inst->config->enable_tx)
    {
        /* 静态创建永不失败，无需判空 */
        uint16_t stream_size = inst->config->stream_buff_size;
        inst->tx_stream = xStreamBufferCreateStatic(stream_size, 1, inst->tx_stream_buf, &inst->tx_stream_cb);
        inst->tx_sem = xSemaphoreCreateBinaryStatic(&inst->tx_sem_cb);
        inst->tx_mutex = xSemaphoreCreateMutexStatic(&inst->tx_mutex_cb);

        snprintf(tx_task_name[id], configMAX_TASK_NAME_LEN, "mus%u_tx", (unsigned)id);
        xTaskCreateStatic(tx_task_entry, tx_task_name[id], MUS_TASK_STACK_SIZE, (void *)(uint32_t)id,
                          MUS_TX_TASK_PRIORITY, tx_task_stack[id], &tx_task_tcb[id]);
    }

    return 1;
}

/* ========== 公共 API 实现 ========== */

void MUS_Init(void)
{
    for (MUS_Id_e id = 0; id < MUS_COUNT; id++)
    {
        init_instance(id);
    }
}

void MUS_TxData(MUS_Id_e id, const uint8_t *data, uint16_t len)
{
    if (id >= MUS_COUNT || s_instances[id].config == NULL)
    {
        return;
    }
    if (!s_instances[id].config->enable_tx)
    {
        return;
    }
    if (s_instances[id].tx_mutex == NULL || s_instances[id].tx_sem == NULL)
    {
        return;
    }
    if (xPortIsInsideInterrupt())
    {
        return;
    }

    if (xSemaphoreTake(s_instances[id].tx_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        return;
    }

    if (HAL_UART_Transmit_DMA(s_instances[id].config->huart, data, len) != HAL_OK)
    {
        xSemaphoreGive(s_instances[id].tx_mutex);
        return;
    }

    if (xSemaphoreTake(s_instances[id].tx_sem, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        HAL_UART_AbortTransmit(s_instances[id].config->huart);
    }

    xSemaphoreGive(s_instances[id].tx_mutex);
}

void MUS_PutDataToTxStream(MUS_Id_e id, const uint8_t *pData, uint16_t len)
{
    if (id >= MUS_COUNT || s_instances[id].config == NULL)
    {
        return;
    }
    if (!s_instances[id].config->enable_tx)
    {
        return;
    }
    if (pData == NULL || s_instances[id].tx_stream == NULL || len >= s_instances[id].config->stream_buff_size)
    {
        return;
    }

    if (xPortIsInsideInterrupt())
    {
        BaseType_t xTaskWoken = pdFALSE;
        xStreamBufferSendFromISR(s_instances[id].tx_stream, (void *)pData, len, &xTaskWoken);
        portEND_SWITCHING_ISR(xTaskWoken);
    }
    else
    {
        xStreamBufferSend(s_instances[id].tx_stream, (void *)pData, len, 0);
    }
}

size_t MUS_RxStreamRead(MUS_Id_e id, void *pvRxData, size_t xBufferLengthBytes)
{
    if (id >= MUS_COUNT || s_instances[id].config == NULL)
    {
        return 0;
    }
    if (!s_instances[id].config->enable_rx)
    {
        return 0;
    }
    if (pvRxData == NULL || s_instances[id].rx_stream == NULL || xPortIsInsideInterrupt())
    {
        return 0;
    }

    return xStreamBufferReceive(s_instances[id].rx_stream, pvRxData, xBufferLengthBytes, 0);
}

__weak void MUS_RxCallback(MUS_Id_e id, const uint8_t *data, size_t len)
{
    UNUSED(id);
    UNUSED(data);
    UNUSED(len);
}
