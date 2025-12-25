## Hardware Configuration

Key peripherals (configured in `oscil.ioc`):
- **ADC1 + DMA2_Stream0**: Continuous circular mode, triggered by TIM3, sampling on PA3 (ADC_IN3)
  - Sample rate controlled by TIM3 period/prescaler
  - Dual-buffer DMA for uninterrupted acquisition
- **TIM3**: ADC trigger generator (TRGO on update event)
- **TIM1_CH1 (PE9)**: PWM output (1kHz, 50% duty cycle by default)
- **SPI1**: ILI9341 display communication (24 Mbits/s)
  - CS: PD14, DC: PF14, RST: PE13
- **USART3 (PD8/PD9)**: ST-Link virtual COM port
- **USB_OTG_FS**: USB device interface
- **Buttons**: K1-K8 on GPIO with EXTI interrupts, 80ms software debounce

System clock: 96 MHz (from 8 MHz HSE via PLL)

## Architecture

The codebase follows a layered architecture:

### Signal Acquisition Layer
- **scope_buffer.c/h**: DMA buffer management with circular queue
  - Manages dual-buffer DMA (320 samples per buffer = ILI9341_WIDTH)
  - ISR callbacks enqueue completed buffers
  - Main loop dequeues for processing
  - Tracks overruns when acquisition outpaces display

### Signal Processing Layer
- **scope_signal.c/h**: Waveform analysis algorithms
  - Trigger detection (rising edge with configurable threshold)
  - Period estimation from zero crossings
  - ADC-to-millivolt conversion (3.3V reference, 12-bit ADC)

### Display Layer
- **scope_display.c/h**: Visualization on ILI9341
  - Grid rendering with configurable spacing
  - Waveform plotting with vertical/horizontal windowing
  - Measurement overlay (Vmin, Vmax, frequency)
- **ili9341.c/h**: Low-level LCD driver
  - SPI-based communication
  - Hardware abstraction for CS/DC/RST pins
  - Primitive drawing functions (pixels, lines, text)

### Application Layer
- **scope.c/h**: Main oscilloscope logic
  - Display settings management (voltage/time scale, offset)
  - User request processing (zoom, autoset, offset shift)
  - Coordinates signal processing and display updates
- **input_handler.c/h**: GPIO button handling
  - EXTI interrupt routing
  - Software debouncing (80ms window)
  - Maps button presses to scope functions

### Control Flow
```
ADC1 DMA IRQ → ScopeBuffer_EnqueueFromISR()
     ↓
main loop: ScopeBuffer_HasPending()
     ↓
ScopeBuffer_Dequeue() → Scope_ProcessFrame()
     ↓
ScopeSignal_FindTriggerIndex() → ScopeDisplay_DrawWaveform()
```

Button presses (K1-K8) → `HAL_GPIO_EXTI_Callback()` → `InputHandler_ProcessGpioInterrupt()` → Scope request functions (e.g., `Scope_RequestMoreCycles()`)

## Key Design Patterns

1. **Double Buffering**: ADC DMA writes to two alternating buffers while main loop processes the other
2. **Volatile Request Flags**: User inputs set flags (checked/consumed in main loop) to avoid direct ISR processing
3. **Windowing System**: Separate vertical (voltage) and horizontal (time) window settings allow zoom/pan
4. **Scale Target Toggle**: K8 switches whether K1/K2 adjust voltage scale or time scale

## Button Mapping

- **USER_Btn (PC13)**: Auto-set (auto-adjust voltage/time ranges to signal)
- **K1**: Zoom out (voltage or time, depending on scale target)
- **K2**: Zoom in (voltage or time, depending on scale target)
- **K3**: Decrease offset (shift waveform down or left)
- **K4**: Increase offset (shift waveform up or right)
- **K5-K7**: Currently unmapped in `input_handler.c`
- **K8**: Toggle scale target (voltage ↔ time)
