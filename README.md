# MultiUartStream

可复用的 UART DMA 流缓冲区驱动库，基于 FreeRTOS，适用于 STM32 平台。

## 特性

- 基于 FreeRTOS StreamBuffer + DMA + IDLE 检测的异步收发
- TX/RX 独立可配置（如日志串口仅 TX、调试串口仅 RX）
- 通过配置表抽象硬件，零直接硬件引用
- 静态内存分配（无 malloc）
- `__weak` 接收回调，方便宿主项目处理协议解析
- 支持多实例并发，互斥锁保护 DMA 发送

## 文件结构

```
multi_uart_stream.h              # 公共 API
multi_uart_stream.c              # 实现
multi_uart_stream_config_template.h  # 配置模板
```

## 集成步骤

### 1. 添加为 git submodule

```bash
git submodule add <repo_url> MultiUartStream
```

### 2. 创建项目配置文件

复制 `multi_uart_stream_config_template.h` 到宿主项目的 include 目录，重命名为 `multi_uart_stream_config.h`：

```c
#ifndef __MULTI_UART_STREAM_CONFIG_H_
#define __MULTI_UART_STREAM_CONFIG_H_

#define MUS_COUNT 2

typedef enum {
    MUS_ID_HOST,     // 上位机串口（TX+RX）
    MUS_ID_SUBCOM,   // 子板串口（TX+RX）
    MUS_ID_NUMS,
} MUS_Id_e;

// 可选覆盖
// #define MUS_RX_BUFFER_SIZE   256
// #define MUS_STREAM_BUFF_SIZE 256
// #define MUS_TASK_STACK_SIZE  256
// #define MUS_RX_TASK_PRIORITY 4
// #define MUS_TX_TASK_PRIORITY 3
// #define MUS_TX_READ_SIZE     256
// #define MUS_TX_DELAY_MS      50

#endif
```

**重要**: 确保 `multi_uart_stream_config.h` 所在目录在 include 路径中优先于 `MultiUartStream/` 目录。

### 3. 定义硬件配置表

在宿主项目的某个 `.c` 文件中定义 `mus_hw_table[]`：

```c
#include "multi_uart_stream.h"
#include "usart.h"

const MUS_HwConfig_t mus_hw_table[MUS_COUNT] = {
    [MUS_ID_HOST] = {
        .huart = &huart3,
        .enable_rx = 1,
        .enable_tx = 1,
    },
    [MUS_ID_SUBCOM] = {
        .huart = &huart2,
        .enable_rx = 1,
        .enable_tx = 1,
    },
};
```

### 4. CMake 集成

在 CMakeLists.txt 中添加源文件和 include 路径：

```cmake
# 源文件
${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/MultiUartStream/multi_uart_stream.c

# Include 路径（放在项目 include 目录之后）
${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/MultiUartStream
```

### 5. 初始化和使用

```c
#include "multi_uart_stream.h"

// 系统启动时
MUS_Init();

// 写入发送流（异步，由内部 TX 任务通过 DMA 发送）
MUS_PutDataToTxStream(MUS_ID_HOST, frame, frame_len);

// 强覆盖字节解析回调（逐字节调用，适用于状态机解析）
void MUS_ParseByte(MUS_Id_e id, uint8_t byte) {
    if (id == MUS_ID_HOST) {
        // 状态机解析上位机协议帧
    } else if (id == MUS_ID_SUBCOM) {
        // 状态机处理子板数据
    }
}
```

## TX-only 示例（日志串口）

对于只需要发送的场景（如调试日志），设置 `enable_rx = 0`：

```c
typedef enum {
    MUS_ID_HOST,
    MUS_ID_SUBCOM,
    MUS_ID_LOG,      // 仅 TX
    MUS_ID_NUMS,
} MUS_Id_e;

const MUS_HwConfig_t mus_hw_table[MUS_COUNT] = {
    // ...
    [MUS_ID_LOG] = {
        .huart = &huart4,
        .enable_rx = 0,          // 不创建 RX 资源
        .enable_tx = 1,
    },
};

// 使用
MUS_PutDataToTxStream(MUS_ID_LOG, (uint8_t *)"Hello\n", 6);
```

## API 参考

| 函数 | 说明 |
|------|------|
| `MUS_Init()` | 初始化所有实例（回调注册、流缓冲区、收发任务） |
| `MUS_PutDataToTxStream(id, pData, len)` | 写入发送流（支持中断上下文） |
| `MUS_RxStreamRead(id, pvRxData, len)` | 从接收流读取（非阻塞） |
| `MUS_ParseByte(id, byte)` | `__weak` 字节解析回调 |

## 架构说明

```
┌─────────────────────────────────────────────────────┐
│                  宿主项目 (App 层)                    │
│  MUS_RxCallback() ← 处理接收到的数据                   │
│  MUS_PutDataToTxStream() → 写入待发送数据              │
└─────────────┬───────────────────────┬───────────────┘
              │                       │
    ┌─────────▼─────────┐   ┌────────▼────────┐
    │  RX Task (blocked) │   │ TX Task (blocked) │
    │ StreamBuffer → CB  │   │ StreamBuffer→DMA │
    └─────────┬─────────┘   └────────┬────────┘
              │                       │
    ┌─────────▼─────────┐   ┌────────▼────────┐
    │  DMA + IDLE 检测    │   │   DMA 发送      │
    │  (HAL 回调)        │   │  (HAL 回调)      │
    └─────────┬─────────┘   └────────┬────────┘
              │                       │
              └───────┬───────────────┘
                      │
              ┌───────▼───────┐
              │  UART 硬件     │
              │ (huart2/3/4)  │
              └───────────────┘
```
