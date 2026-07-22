# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

A reusable UART DMA stream buffer driver library for STM32 + FreeRTOS. Designed to be included as a **git submodule** in host embedded projects — it has no build system of its own.

## Architecture

**Three files only:**
- `multi_uart_stream.h` — public API and default config macros
- `multi_uart_stream.c` — implementation (RX/TX tasks, DMA callbacks, static allocation)
- `multi_uart_stream_config_template.h` — template for host project configuration

**Runtime flow:**
1. Host project defines `MUS_Id_e` enum + `MUS_COUNT` in `multi_uart_stream_config.h`
2. Host project defines `mus_hw_table[]` mapping each instance to a UART handle + TX/RX enables
3. `MUS_Init()` creates per-instance RX/TX FreeRTOS tasks and registers HAL DMA callbacks
4. RX path: DMA+IDLE interrupt → `rx_event_callback` (ISR) → StreamBuffer → RX task → `MUS_ParseByte` (`__weak`, called per byte)
5. TX path: `MUS_PutDataToTxStream` → StreamBuffer → TX task → static `MUS_TxData` → `HAL_UART_Transmit_DMA`

**Key design decisions:**
- All memory is statically allocated (`xStreamBufferCreateStatic`, `xTaskCreateStatic`, etc.) — no malloc
- TX/RX are independently configurable per instance (e.g., log UART = TX-only)
- Compile-time `_Static_assert` validates buffer sizes against config table (unrolled for up to 8 instances)
- `MUS_PutDataToTxStream` is the sole public TX API (ISR-safe, writes to stream buffer with anti-frame-sticking delay)
- `mus_tx_data` is static, called only by the TX task (semaphore-synced DMA send)

## Integration Requirements

The host project must provide:
1. `multi_uart_stream_config.h` — defines `MUS_COUNT` and `MUS_Id_e`; optionally overrides buffer/priority macros. **Must be on include path before this library's directory.**
2. `mus_hw_table[MUS_COUNT]` — hardware config (UART handle, buffer sizes, enable flags)
3. FreeRTOS + STM32 HAL headers reachable via include paths

## Coding Conventions

- C99 with STM32 HAL idioms (`HAL_UART_`, `__HAL_UART_`)
- Chinese comments throughout (this is a Chinese-authored embedded library)
- `__weak` callback pattern for host override (`MUS_ParseByte` — per-byte parser)
- All public API prefixed with `MUS_`
- Instance ID type is `MUS_Id_e` (enum defined by host)
