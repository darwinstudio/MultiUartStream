/**
 * @file multi_uart_stream_config_template.h
 * @brief MultiUartStream 配置模板
 *
 * 使用方法:
 *   1. 将此文件复制到宿主项目中，重命名为 multi_uart_stream_config.h
 *   2. 修改 MUS_COUNT 和 MUS_Id_e 为项目实际值
 *   3. 可选覆盖各项参数宏
 *   4. 确保 multi_uart_stream_config.h 所在目录在 include 路径中优先于本目录
 *   5. 在宿主项目中定义硬件配置表 mus_hw_table[]
 */

#ifndef __MULTI_UART_STREAM_CONFIG_H_
#define __MULTI_UART_STREAM_CONFIG_H_

/** @brief UART 实例数量 */
#ifndef MUS_COUNT
#define MUS_COUNT 2
#endif

/**
 * @brief UART 实例 ID 枚举（必须定义）
 *
 * MUS_COUNT 必须等于枚举中的有效 ID 数量（不含 _NUMS 哨兵值）。
 *
 * 示例:
 * @code
 *   typedef enum {
 *       MUS_ID_HOST,
 *       MUS_ID_SUBCOM,
 *       MUS_ID_NUMS  // = MUS_COUNT
 *   } MUS_Id_e;
 * @endcode
 */

/** @name 可选覆盖（默认值在 multi_uart_stream.h 中定义） */
/** @{ */

// #define MUS_RX_BUFFER_SIZE   256  ///< DMA 接收缓冲区大小（字节），默认 256
// #define MUS_STREAM_BUFF_SIZE 256  ///< 收发流缓冲区大小（字节），默认 256
// #define MUS_RX_TASK_STACK_SIZE 128  ///< RX 任务栈大小（word），默认 128
// #define MUS_TX_TASK_STACK_SIZE 256  ///< TX 任务栈大小（word），默认 256
// #define MUS_RX_TASK_PRIORITY 4    ///< 接收任务优先级，默认 4
// #define MUS_TX_TASK_PRIORITY 3    ///< 发送任务优先级，默认 3
// #define MUS_TX_READ_SIZE     256  ///< TX 任务单次读取缓冲区大小（字节），默认 256
// #define MUS_TX_DELAY_MS      50   ///< 写入发送流前的延时（ms），防止帧粘连，默认 50

/** @} */

#endif /* __MULTI_UART_STREAM_CONFIG_H_ */
