#include "input_handler.h"

#include "main.h"
#include "scope.h"

enum
{
    BUTTON_DEBOUNCE_MS = 80U
};

typedef struct
{
    uint16_t gpio_pin;
    uint32_t last_tick;
} ButtonDebounceState;
static ButtonDebounceState button_debounce_states[] = {
    {USER_Btn_Pin, 0U},
    {K1_Pin, 0U},
    {K2_Pin, 0U},
    {K3_Pin, 0U},
    {K4_Pin, 0U},
    {K5_Pin, 0U},
    {K6_Pin, 0U},
    {K7_Pin, 0U},
    {K8_Pin, 0U}
};

static uint8_t InputHandler_IsButtonDebounced(uint16_t gpio_pin);

void InputHandler_Init(void)
{
    const uint32_t state_count = sizeof(button_debounce_states) / sizeof(button_debounce_states[0]);
    for (uint32_t idx = 0U; idx < state_count; idx++)
    {
        button_debounce_states[idx].last_tick = 0U;
    }
}

void InputHandler_ProcessGpioInterrupt(uint16_t gpio_pin)
{
    if (!InputHandler_IsButtonDebounced(gpio_pin))
    {
        return;
    }

    if (gpio_pin == USER_Btn_Pin)
    {
        HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
        Scope_RequestAutoSet();
    }
    else if (gpio_pin == K1_Pin)
    {
        if (Scope_IsWaveformHoldEnabled())
        {
            Scope_ToggleCursorAutoShift(-1);
        }
        else if (Scope_GetScaleTarget() == SCOPE_SCALE_TARGET_VOLTAGE)
        {
            Scope_RequestMoreVoltageScale();
        }
        else
        {
            Scope_RequestMoreCycles();
        }
    }
    else if (gpio_pin == K2_Pin)
    {
        if (Scope_IsWaveformHoldEnabled())
        {
            Scope_ToggleCursorAutoShift(1);
        }
        else if (Scope_GetScaleTarget() == SCOPE_SCALE_TARGET_VOLTAGE)
        {
            Scope_RequestLessVoltageScale();
        }
        else
        {
            Scope_RequestFewerCycles();
        }
    }
    else if (gpio_pin == K3_Pin)
    {
        Scope_RequestOffsetDecrease();
    }
    else if (gpio_pin == K4_Pin)
    {
        Scope_RequestOffsetIncrease();
    }
    else if (gpio_pin == K5_Pin)
    {
        if (Scope_IsWaveformHoldEnabled())
        {
            Scope_RequestCursorShift(-1);
        }
    }
    else if (gpio_pin == K6_Pin)
    {
        if (Scope_IsWaveformHoldEnabled())
        {
            Scope_RequestCursorShift(1);
        }
    }
    else if (gpio_pin == K7_Pin)
    {
        Scope_ToggleWaveformHold();
    }
    else if (gpio_pin == K8_Pin)
    {
        if (Scope_IsWaveformHoldEnabled())
        {
            Scope_RequestCursorSelectNext();
        }
        else
        {
            Scope_ToggleScaleTarget();
        }
    }
}

static uint8_t InputHandler_IsButtonDebounced(uint16_t gpio_pin)
{
    const uint32_t now = HAL_GetTick();
    ButtonDebounceState *state = NULL;
    const uint32_t state_count = sizeof(button_debounce_states) / sizeof(button_debounce_states[0]);

    for (uint32_t idx = 0U; idx < state_count; idx++)
    {
        if (button_debounce_states[idx].gpio_pin == gpio_pin)
        {
            state = &button_debounce_states[idx];
            break;
        }
    }

    if (state == NULL)
    {
        return 1U;
    }

    if (state->last_tick != 0U)
    {
        if ((now - state->last_tick) < BUTTON_DEBOUNCE_MS)
        {
            return 0U;
        }
    }

    state->last_tick = now;
    return 1U;
}
