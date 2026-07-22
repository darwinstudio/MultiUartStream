/**
 * @file multi_uart_stream.c
 * @brief MultiUartStream 库实现
 *
 * 基于 FreeRTOS StreamBuffer + DMA + IDLE 检测的多实例 UART 异步收发驱动。
 */

#include "multi_uart_stream.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/** @brief 每个 UART 实例的运行时状态 */
typedef struct
{
    const MUS_HwConfig_t *config; /**< 指向硬件配置表条目 */

    StreamBufferHandle_t rx_stream;            /**< 接收流缓冲区句柄 */
    StaticStreamBuffer_t rx_stream_cb;         /**< 接收流缓冲区控制块 */
    uint8_t rx_stream_buf[MUS_STREAM_BUFF_SIZE + 1]; /**< 接收流缓冲区 */
    uint8_t rx_dma_buf[MUS_RX_BUFFER_SIZE];    /**< DMA 接收缓冲区 */

    StreamBufferHandle_t tx_stream;            /**< 发送流缓冲区句柄 */
    StaticStreamBuffer_t tx_stream_cb;         /**< 发送流缓冲区控制块 */
    uint8_t tx_stream_buf[MUS_STREAM_BUFF_SIZE + 1]; /**< 发送流缓冲区 */
    SemaphoreHandle_t tx_sem;                  /**< TX 完成信号量 */
    StaticSemaphore_t tx_sem_cb;               /**< TX 信号量控制块 */
} MUS_Instance_t;

static MUS_Instance_t s_instances[MUS_COUNT];

static StackType_t rx_task_stack[MUS_COUNT][MUS_TASK_STACK_SIZE];
static StaticTask_t rx_task_tcb[MUS_COUNT];
static StackType_t tx_task_stack[MUS_COUNT][MUS_TASK_STACK_SIZE];
static StaticTask_t tx_task_tcb[MUS_COUNT];
static char rx_task_name[MUS_COUNT][configMAX_TASK_NAME_LEN];
static char tx_task_name[MUS_COUNT][configMAX_TASK_NAME_LEN];

/**
 * @brief 启动 DMA + IDLE 检测接收
 * @param inst 目标实例指针
 */
static void open_rx_idle(MUS_Instance_t *inst)
{
    UART_HandleTypeDef *huart = inst->config->huart;

    __HAL_UART_CLEAR_OREFLAG(huart);

    if (huart->RxState != HAL_UART_STATE_READY)
    {
        HAL_UART_AbortReceive(huart);
    }

    HAL_StatusTypeDef status = HAL_UARTEx_ReceiveToIdle_DMA(huart, inst->rx_dma_buf, MUS_RX_BUFFER_SIZE);
    if (status != HAL_OK)
    {
        HAL_UART_AbortReceive(huart);
        HAL_UARTEx_ReceiveToIdle_DMA(huart, inst->rx_dma_buf, MUS_RX_BUFFER_SIZE);
        return;
    }
    __HAL_DMA_DISABLE_IT(huart->hdmarx, DMA_IT_HT);
}

/**
 * @brief 内部 DMA 发送（由 TX 任务调用）
 * @param id   实例 ID
 * @param data 待发送数据
 * @param len  数据长度
 */
static void mus_tx_data(MUS_Id_e id, const uint8_t *data, uint16_t len)
{
    if (HAL_UART_Transmit_DMA(s_instances[id].config->huart, data, len) == HAL_OK)
    {
        if (xSemaphoreTake(s_instances[id].tx_sem, pdMS_TO_TICKS(2000)) != pdTRUE)
        {
            HAL_UART_AbortTransmit(s_instances[id].config->huart);
        }
    }
}

/**
 * @brief DMA 接收完成回调（IDLE 检测触发）
 * @param huart 触发回调的 UART 句柄
 * @param len   本次接收到的字节数
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
 * @param huart 触发回调的 UART 句柄
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
 * @param huart 触发回调的 UART 句柄
 */
static void uart_error_callback(UART_HandleTypeDef *huart)
{
    for (uint8_t i = 0; i < MUS_COUNT; i++)
    {
        if (s_instances[i].config != NULL && s_instances[i].config->huart == huart)
        {
            if (s_instances[i].config->enable_tx && s_instances[i].tx_sem != NULL)
            {
                HAL_UART_AbortTransmit(huart);
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                xSemaphoreGiveFromISR(s_instances[i].tx_sem, &xHigherPriorityTaskWoken);
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
            }
            if (s_instances[i].config->enable_rx)
            {
                HAL_UART_AbortReceive(huart);
                open_rx_idle(&s_instances[i]);
            }
            return;
        }
    }
}

/**
 * @brief RX 任务入口（逐字节接收并调用解析回调）
 * @param para 任务参数，转换为 MUS_Id_e 实例 ID
 */
static void rx_task_entry(void *para)
{
    MUS_Id_e id = (MUS_Id_e)(uint32_t)para;
    uint8_t byte;

    for (;;)
    {
        if (xStreamBufferReceive(s_instances[id].rx_stream, &byte, 1, portMAX_DELAY) == 1)
        {
            MUS_ParseByte(id, byte);
        }
    }
}

/**
 * @brief TX 任务入口（从发送流读取并通过 DMA 发送）
 * @param para 任务参数，转换为 MUS_Id_e 实例 ID
 */
static void tx_task_entry(void *para)
{
    MUS_Id_e id = (MUS_Id_e)(uint32_t)para;
    uint8_t buf[MUS_TX_READ_SIZE];

    for (;;)
    {
        size_t len = xStreamBufferReceive(s_instances[id].tx_stream, buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (len > 0)
        {
            mus_tx_data(id, buf, (uint16_t)len);
        }
    }
}

/**
 * @brief 初始化单个 UART 实例
 * @param id 实例 ID
 * @return 1=成功，0=失败
 */
static uint8_t init_instance(MUS_Id_e id)
{
    MUS_Instance_t *inst = &s_instances[id];
    inst->config = &mus_hw_table[id];

    if (HAL_UART_RegisterCallback(inst->config->huart, HAL_UART_ERROR_CB_ID, uart_error_callback) != HAL_OK)
    {
        return 0;
    }

    if (inst->config->enable_rx)
    {
        if (HAL_UART_RegisterRxEventCallback(inst->config->huart, rx_event_callback) != HAL_OK)
        {
            return 0;
        }

        inst->rx_stream = xStreamBufferCreateStatic(MUS_STREAM_BUFF_SIZE, 1, inst->rx_stream_buf, &inst->rx_stream_cb);
        open_rx_idle(inst);

        snprintf(rx_task_name[id], configMAX_TASK_NAME_LEN, "mus%u_rx", (unsigned)id);
        xTaskCreateStatic(rx_task_entry, rx_task_name[id], MUS_TASK_STACK_SIZE, (void *)(uint32_t)id,
                          MUS_RX_TASK_PRIORITY, rx_task_stack[id], &rx_task_tcb[id]);
    }

    if (inst->config->enable_tx)
    {
        if (HAL_UART_RegisterCallback(inst->config->huart, HAL_UART_TX_COMPLETE_CB_ID, tx_cplt_callback) != HAL_OK)
        {
            return 0;
        }

        inst->tx_stream = xStreamBufferCreateStatic(MUS_STREAM_BUFF_SIZE, 1, inst->tx_stream_buf, &inst->tx_stream_cb);
        inst->tx_sem = xSemaphoreCreateBinaryStatic(&inst->tx_sem_cb);

        snprintf(tx_task_name[id], configMAX_TASK_NAME_LEN, "mus%u_tx", (unsigned)id);
        xTaskCreateStatic(tx_task_entry, tx_task_name[id], MUS_TASK_STACK_SIZE, (void *)(uint32_t)id,
                          MUS_TX_TASK_PRIORITY, tx_task_stack[id], &tx_task_tcb[id]);
    }

    return 1;
}

/**
 * @brief 初始化所有 UART 实例
 * @see mus_hw_table
 */
void MUS_Init(void)
{
    for (MUS_Id_e id = 0; id < MUS_COUNT; id++)
    {
        init_instance(id);
    }
}

/**
 * @brief 向发送流中写入数据（支持中断/任务上下文）
 * @param id    实例 ID
 * @param pData 待写入数据
 * @param len   数据长度
 */
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
    if (pData == NULL || s_instances[id].tx_stream == NULL || len >= MUS_STREAM_BUFF_SIZE)
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
        vTaskDelay(pdMS_TO_TICKS(MUS_TX_DELAY_MS));
        xStreamBufferSend(s_instances[id].tx_stream, (void *)pData, len, 0);
    }
}

/**
 * @brief 从接收流中读取数据（非阻塞，仅任务上下文）
 * @param id                 实例 ID
 * @param pvRxData           读取目标缓冲区
 * @param xBufferLengthBytes 最大读取字节数
 * @return 实际读取的字节数
 */
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

/**
 * @brief 字节解析回调（__weak，宿主项目强覆盖以实现协议解析）
 * @param id   数据来源的实例 ID
 * @param byte 接收到的单个字节
 */
__weak void MUS_ParseByte(MUS_Id_e id, uint8_t byte)
{
    UNUSED(id);
    UNUSED(byte);
}
