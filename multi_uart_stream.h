/**
 * @file multi_uart_stream.h
 * @brief MultiUartStream - 可复用的 UART DMA 流缓冲区驱动库
 *
 * 基于 FreeRTOS StreamBuffer + DMA + IDLE 检测实现异步收发。
 * 通过配置表抽象硬件，TX/RX 独立可配置。
 *
 * 宿主项目需提供:
 *   - multi_uart_stream_config.h: 定义 MUS_COUNT, MUS_Id_e
 *   - mus_hw_table[]: 硬件配置表（UART句柄、缓冲区大小、TX/RX使能）
 */

#ifndef __MULTI_UART_STREAM_H_
#define __MULTI_UART_STREAM_H_

#include "multi_uart_stream_config.h"
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "usart.h"

/* ========== 默认配置（宿主项目可在 multi_uart_stream_config.h 中覆盖） ========== */

#ifndef MUS_RX_BUFFER_SIZE
#define MUS_RX_BUFFER_SIZE 256 /**< DMA 接收缓冲区大小（字节） */
#endif

#ifndef MUS_STREAM_BUFF_SIZE
#define MUS_STREAM_BUFF_SIZE 256 /**< 流缓冲区大小（字节），须 >= 最大单帧长度 */
#endif

#ifndef MUS_TASK_STACK_SIZE
#define MUS_TASK_STACK_SIZE 256 /**< 收发任务栈大小（word） */
#endif

#ifndef MUS_RX_TASK_PRIORITY
#define MUS_RX_TASK_PRIORITY 4 /**< 接收任务优先级 */
#endif

#ifndef MUS_TX_TASK_PRIORITY
#define MUS_TX_TASK_PRIORITY 3 /**< 发送任务优先级 */
#endif

/* ========== 类型定义 ========== */

/** @brief 硬件配置结构体（由宿主项目在 mus_hw_table[] 中填充） */
typedef struct
{
    UART_HandleTypeDef *huart; /**< UART 句柄（CubeMX 生成） */
    uint16_t rx_buff_size;     /**< DMA 接收缓冲区大小 */
    uint16_t stream_buff_size; /**< 收发流缓冲区大小 */
    uint8_t enable_rx;         /**< 1=启用接收（DMA+IDLE+RX流+RX任务） */
    uint8_t enable_tx;         /**< 1=启用发送（TX流+TX任务） */
} MUS_HwConfig_t;

/* ========== 外部配置表（宿主项目定义） ========== */

/** @brief 硬件配置表，宿主项目必须定义，大小为 MUS_COUNT */
extern const MUS_HwConfig_t mus_hw_table[MUS_COUNT];

/* ========== 公共 API ========== */

/**
 * @brief 初始化所有 UART 实例
 *
 * 为每个实例注册 HAL 回调、创建流缓冲区、创建收发任务。
 * enable_rx/enable_tx 为 0 的方向将跳过对应资源的创建。
 */
void MUS_Init(void);

/**
 * @brief 阻塞式 DMA 发送（互斥锁保护，支持多任务并发）
 * @param id 实例 ID
 * @param data 待发送数据
 * @param len 数据长度
 */
void MUS_TxData(MUS_Id_e id, const uint8_t *data, uint16_t len);

/**
 * @brief 向发送流中写入数据（支持中断/任务上下文）
 *
 * 数据先写入流缓冲区，由内部 TX 任务读取后通过 DMA 发送。
 * 适用于协议层组帧后异步发送的场景。
 *
 * @param id 实例 ID
 * @param pData 待写入数据
 * @param len 数据长度
 */
void MUS_PutDataToTxStream(MUS_Id_e id, const uint8_t *pData, uint16_t len);

/**
 * @brief 从接收流中读取数据（非阻塞）
 * @param id 实例 ID
 * @param pvRxData 读取目标缓冲区
 * @param xBufferLengthBytes 最大读取字节数
 * @return 实际读取的字节数
 */
size_t MUS_RxStreamRead(MUS_Id_e id, void *pvRxData, size_t xBufferLengthBytes);

/**
 * @brief 接收数据回调（__weak，宿主项目强覆盖以处理接收到的数据）
 *
 * 在 RX 任务上下文中调用，当接收到 IDLE 中断数据时触发。
 * 仅在 enable_rx=1 时生效。
 *
 * @param id 数据来源的实例 ID
 * @param data 接收到的数据指针
 * @param len 数据长度
 */
void MUS_RxCallback(MUS_Id_e id, const uint8_t *data, size_t len);

#endif /* __MULTI_UART_STREAM_H_ */
