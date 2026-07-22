# 代码评审要求

## 1. 中断安全

- ISR 回调中所有 FreeRTOS API 必须使用 `FromISR` 后缀版本（`xStreamBufferSendFromISR`、`xSemaphoreGiveFromISR`）
- ISR 中调用 `portEND_SWITCHING_ISR` / `portYIELD_FROM_ISR` 处理上下文切换
- `MUS_PutDataToTxStream` 必须区分中断/任务上下文（`xPortIsInsideInterrupt()`），中断中不能调用 `vTaskDelay`
- ISR 中的防御性 NULL 检查必须保留（`config`、`rx_stream`、`tx_sem`）

## 2. 缓冲区安全

- `MUS_RX_BUFFER_SIZE` 定义所有实例统一的 DMA 接收缓冲区大小，必须 >= 实际最大接收长度
- `MUS_STREAM_BUFF_SIZE` 定义所有实例统一的流缓冲区大小，必须 >= 最大单帧长度
- `MUS_TX_READ_SIZE` 必须 <= `MUS_STREAM_BUFF_SIZE`，确保 TX 读取缓冲区能容纳流中的完整数据
- `MUS_PutDataToTxStream` 写入前必须校验 `len < MUS_STREAM_BUFF_SIZE`，防止溢出
- `xStreamBufferCreateStatic` 的触发阈值参数固定为 1（逐字节通知）

## 3. 资源生命周期

- `init_instance` 中必须先注册 HAL 回调，再创建 StreamBuffer / 启动 DMA
- `open_rx_idle` 中 DMA 启动失败时必须 `AbortReceive` 后重试一次，重试后仍失败则直接返回
- `open_rx_idle` 启动 DMA 后必须禁用半传输中断（`__HAL_DMA_DISABLE_IT(DMA_IT_HT)`），防止 IDLE 前误触发
- 错误回调中 TX/RX 必须分别处理：TX 给信号量解阻塞，RX 重启接收
- `enable_rx=0` 的实例不得注册 RX 回调、创建 RX 资源

## 4. 公共 API 边界检查

- 所有公共 API 首行必须校验 `id < MUS_COUNT && config != NULL`
- 功能开关检查必须在资源检查之前（先查 `enable_tx`，再操作 `tx_stream`）
- `MUS_RxStreamRead` 禁止在中断上下文调用（`xStreamBufferReceive` 非 ISR 版本）
- `MUS_PutDataToTxStream` 任务上下文调用时必须延时 `MUS_TX_DELAY_MS` 以防止帧粘连

## 5. 配置表一致性

- `MUS_COUNT` 必须等于 `MUS_Id_e` 枚举的有效 ID 数量
- `mus_hw_table[]` 大小必须为 `MUS_COUNT`，不多不少
- `enable_rx` / `enable_tx` 只允许 0 或 1
- `MUS_TX_READ_SIZE` 必须 <= `MUS_STREAM_BUFF_SIZE`（可加编译期检查）
- `huart` 不得为 NULL

## 6. 静态分配

- 所有 FreeRTOS 对象必须使用 `Static` 版本创建（`xStreamBufferCreateStatic`、`xTaskCreateStatic`、`xSemaphoreCreateBinaryStatic`）
- 不得出现 `malloc` / `free` / `calloc`
- 任务栈、TCB、StreamBuffer 控制块必须为 `static` 或模块级数组

## 7. 公共 API 设计

- 对外接口只暴露 `MUS_` 前缀函数，内部函数全部 `static`
- `static` 函数命名全小写（`open_rx_idle`、`mus_tx_data`、`init_instance`）
- 所有函数必须有 Doxygen `@brief` / `@param` / `@return` 注释
- 不得引入 `stdio.h` 等重型标准库
- `MUS_ParseByte` 为 `__weak`，宿主项目必须强覆盖实现协议解析逻辑

## 8. 可重入性

- 同一实例的 TX 任务是唯一 DMA 消费者，不需要 mutex
- `MUS_PutDataToTxStream` 写入 StreamBuffer 本身是线程安全的
- ISR 回调通过遍历 `s_instances` 查找匹配的 `huart`，遍历期间不得有实例增删
