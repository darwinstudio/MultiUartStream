/**
 * @file multi_uart_stream.h
 * @brief MultiUartStream - 可复用的 UART DMA 流缓冲区驱动库
 *
 * 基于 FreeRTOS StreamBuffer + DMA + IDLE 检测实现异步收发。
 * 通过配置表抽象硬件，TX/RX 独立可配置。
 *
 * 宿主项目需提供:
 *   - multi_uart_stream_config.h: 定义 MUS_COUNT, MUS_Id_e
 *   - mus_hw_table[]: 硬件配置表（UART 句柄、TX/RX 使能）
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

/** @name 默认配置（宿主项目可在 multi_uart_stream_config.h 中覆盖） */
/** @{ */

#ifndef MUS_RX_BUFFER_SIZE
#define MUS_RX_BUFFER_SIZE 256 /**< DMA 接收缓冲区大小（字节） */
#endif

#ifndef MUS_STREAM_BUFF_SIZE
#define MUS_STREAM_BUFF_SIZE 512 /**< 流缓冲区大小（字节），须 >= 最大单帧长度 */
#endif

#ifndef MUS_RX_TASK_STACK_SIZE
#define MUS_RX_TASK_STACK_SIZE 256 /**< RX 任务栈大小（word），批量模式需容纳 MUS_RX_READ_SIZE 缓冲区；逐字节模式可覆盖为 128 节省内存 */
#endif

#ifndef MUS_TX_TASK_STACK_SIZE
#define MUS_TX_TASK_STACK_SIZE 256 /**< TX 任务栈大小（word），缓冲区读取，栈需求较大 */
#endif

#ifndef MUS_RX_TASK_PRIORITY
#define MUS_RX_TASK_PRIORITY 3 /**< 接收任务优先级 */
#endif

#ifndef MUS_TX_TASK_PRIORITY
#define MUS_TX_TASK_PRIORITY 4 /**< 发送任务优先级 */
#endif

#ifndef MUS_TX_READ_SIZE
#define MUS_TX_READ_SIZE 256 /**< TX 任务单次读取缓冲区大小（字节） */
#endif

#ifndef MUS_RX_READ_SIZE
#define MUS_RX_READ_SIZE 256 /**< RX 批量模式单次读取缓冲区大小（字节） */
#endif

#ifndef MUS_TX_DELAY_MS
#define MUS_TX_DELAY_MS 50 /**< 写入发送流前的延时（ms），防止帧粘连 */
#endif

/** @} */

/**
 * @brief 硬件配置结构体
 *
 * 由宿主项目在 mus_hw_table[] 中填充，每个条目对应一个 UART 实例。
 */
typedef struct
{
    UART_HandleTypeDef *huart; /**< UART 句柄（CubeMX 生成） */
    uint8_t enable_rx;         /**< 1=启用接收（DMA+IDLE+RX 流+RX 任务） */
    uint8_t enable_tx;         /**< 1=启用发送（TX 流+TX 任务） */
    uint8_t use_bulk_rx;       /**< 1=批量接收模式（调用 MUS_ParseData），0=逐字节模式（调用 MUS_ParseByte） */
} MUS_HwConfig_t;

/** @brief 硬件配置表，宿主项目必须定义，大小为 MUS_COUNT */
extern const MUS_HwConfig_t mus_hw_table[MUS_COUNT];

/**
 * @brief 初始化所有 UART 实例
 *
 * 为每个实例注册 HAL 回调、创建流缓冲区、创建收发任务。
 * enable_rx/enable_tx 为 0 的方向将跳过对应资源的创建。
 */
void MUS_Init(void);

/**
 * @brief 向发送流中写入数据（支持中断/任务上下文）
 *
 * 数据先写入流缓冲区，由内部 TX 任务读取后通过 DMA 发送。
 * 任务上下文调用时会延时 MUS_TX_DELAY_MS 毫秒以防止帧粘连。
 *
 * @param id    实例 ID
 * @param pData 待写入数据
 * @param len   数据长度
 */
void MUS_PutDataToTxStream(MUS_Id_e id, const uint8_t *pData, uint16_t len);

/**
 * @brief 字节解析回调（__weak，宿主项目强覆盖以实现协议解析）
 *
 * 在 RX 任务上下文中逐字节调用，适用于状态机式协议解析。
 * 仅在 enable_rx=1 且 use_bulk_rx=0 时生效。
 *
 * @param id   数据来源的实例 ID
 * @param byte 接收到的单个字节
 */
void MUS_ParseByte(MUS_Id_e id, uint8_t byte);

/**
 * @brief 批量数据解析回调（__weak，宿主项目强覆盖以实现协议解析）
 *
 * 在 RX 任务上下文中一次性接收所有可用数据后调用，适用于整帧处理。
 * 仅在 enable_rx=1 且 use_bulk_rx=1 时生效。
 *
 * @param id   数据来源的实例 ID
 * @param data 接收到的数据指针
 * @param len  数据长度（字节）
 */
void MUS_ParseData(MUS_Id_e id, const uint8_t *data, size_t len);

#endif /* __MULTI_UART_STREAM_H_ */
