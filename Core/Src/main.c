/* USER CODE BEGIN Header */
/**
  * @file           : main.c
  * @brief          : STM32F103C8T6 Automatic Power Factor Correction
  *
  * CONNECTIONS
  * PA0 -> ZMPT Voltage Sensor OUT
  * PA1 -> ACS712 OUT
  * PA3 -> Relay 1 -> 2.5 uF
  * PA4 -> Relay 2 -> 6 uF
  * PA5 -> Relay 3 -> 10 uF
  * PA6 -> Relay 4 -> 20 uF
  * PB6 -> I2C SCL
  * PB7 -> I2C SDA
  */
/* USER CODE END Header */

#include "main.h"
#include "lcd_i2c.h"
#include <stdio.h>
#include <math.h>

ADC_HandleTypeDef hadc1;
I2C_HandleTypeDef hi2c1;

/* ========================== SETTINGS ========================== */

#define ADC_REF                 3.3f
#define ADC_MAX                 4095.0f

#define CYCLE_SAMPLES           2000U
#define OFFSET_SAMPLES          1000U

#define VOLTAGE_GAIN            1883.0f
#define ACS712_SENSITIVITY      0.100f

/* Relay pins */
#define RELAY1_PIN              GPIO_PIN_3
#define RELAY2_PIN              GPIO_PIN_4
#define RELAY3_PIN              GPIO_PIN_5
#define RELAY4_PIN              GPIO_PIN_6

/* Active LOW relay module */
#define RELAY_ON                GPIO_PIN_RESET
#define RELAY_OFF               GPIO_PIN_SET

/* Strict PF limits */
#define PF_LOW_LIMIT            0.85f
#define PF_HIGH_LIMIT           0.95f

/* Capacitor values */
#define CAP1_UF                 2.5f
#define CAP2_UF                 6.0f
#define CAP3_UF                 10.0f
#define CAP4_UF                 20.0f

#define MAINS_FREQUENCY         50.0f

/* ========================== VARIABLES ========================== */

char lcd[21];

float Voltage = 0.0f;
float Current = 0.0f;
float RealPower = 0.0f;
float ApparentPower = 0.0f;
float PowerFactor = 0.0f;

float VoltageOffset = 1.65f;
float CurrentOffset = 2.50f;

uint8_t capacitorState = 0;

/* ========================== PROTOTYPES ========================== */

void SystemClock_Config(void);

static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2C1_Init(void);

static void CalibrateOffsets(void);
static void ReadPowerParameters(void);

static void ApplyCapacitorState(uint8_t state);
static float GetCapacitanceFromState(uint8_t state);

static float CalculateRequiredCapacitance(void);
static uint8_t FindBestCapacitorCombination(float required_uF);

static void CorrectPowerFactor(void);

/* ========================== OFFSET CALIBRATION ========================== */

static void CalibrateOffsets(void)
{
    uint32_t adcV;
    uint32_t adcI;

    float sumV = 0.0f;
    float sumI = 0.0f;

    HAL_ADC_Start(&hadc1);

    for (uint32_t i = 0; i < OFFSET_SAMPLES; i++)
    {
        /* PA0 - Voltage */
        if (HAL_ADC_PollForConversion(&hadc1, 100) != HAL_OK)
        {
            continue;
        }

        adcV = HAL_ADC_GetValue(&hadc1);

        /* PA1 - Current */
        if (HAL_ADC_PollForConversion(&hadc1, 100) != HAL_OK)
        {
            continue;
        }

        adcI = HAL_ADC_GetValue(&hadc1);

        sumV += ((float)adcV * ADC_REF) / ADC_MAX;
        sumI += ((float)adcI * ADC_REF) / ADC_MAX;
    }

    HAL_ADC_Stop(&hadc1);

    VoltageOffset = sumV / OFFSET_SAMPLES;
    CurrentOffset = sumI / OFFSET_SAMPLES;
}

/* ========================== MEASUREMENT ========================== */

static void ReadPowerParameters(void)
{
    uint32_t adcV;
    uint32_t adcI;

    float vSample;
    float iSample;

    float sumV = 0.0f;
    float sumI = 0.0f;

    float sumV2 = 0.0f;
    float sumI2 = 0.0f;

    float sumP = 0.0f;

    uint32_t validSamples = 0;

    HAL_ADC_Start(&hadc1);

    for (uint32_t n = 0; n < CYCLE_SAMPLES; n++)
    {
        /* PA0 - Voltage */

        if (HAL_ADC_PollForConversion(&hadc1, 100) != HAL_OK)
        {
            continue;
        }

        adcV = HAL_ADC_GetValue(&hadc1);

        /* PA1 - Current */

        if (HAL_ADC_PollForConversion(&hadc1, 100) != HAL_OK)
        {
            continue;
        }

        adcI = HAL_ADC_GetValue(&hadc1);

        /* Voltage conversion */

        vSample =
            (
                ((float)adcV * ADC_REF) / ADC_MAX
                - VoltageOffset
            ) * VOLTAGE_GAIN;

        /* Current conversion */

        iSample =
            (
                ((float)adcI * ADC_REF) / ADC_MAX
                - CurrentOffset
            ) / ACS712_SENSITIVITY;

        /* Accumulate */

        sumV += vSample;
        sumI += iSample;

        sumV2 += vSample * vSample;
        sumI2 += iSample * iSample;

        validSamples++;
    }

    HAL_ADC_Stop(&hadc1);

    if (validSamples == 0)
    {
        Voltage = 0.0f;
        Current = 0.0f;
        RealPower = 0.0f;
        ApparentPower = 0.0f;
        PowerFactor = 0.0f;

        return;
    }

    /* Mean values */

    float meanV = sumV / validSamples;
    float meanI = sumI / validSamples;

    /* RMS */

    float varV =
        (sumV2 / validSamples)
        - (meanV * meanV);

    float varI =
        (sumI2 / validSamples)
        - (meanI * meanI);

    if (varV < 0.0f)
    {
        varV = 0.0f;
    }

    if (varI < 0.0f)
    {
        varI = 0.0f;
    }

    Voltage = sqrtf(varV);
    Current = sqrtf(varI);

    /* ================= REAL POWER ================= */

    HAL_ADC_Start(&hadc1);

    uint32_t powerSamples = 0;

    for (uint32_t n = 0; n < CYCLE_SAMPLES; n++)
    {
        if (HAL_ADC_PollForConversion(&hadc1, 100) != HAL_OK)
        {
            continue;
        }

        adcV = HAL_ADC_GetValue(&hadc1);

        if (HAL_ADC_PollForConversion(&hadc1, 100) != HAL_OK)
        {
            continue;
        }

        adcI = HAL_ADC_GetValue(&hadc1);

        vSample =
            (
                ((float)adcV * ADC_REF) / ADC_MAX
                - VoltageOffset
            ) * VOLTAGE_GAIN;

        iSample =
            (
                ((float)adcI * ADC_REF) / ADC_MAX
                - CurrentOffset
            ) / ACS712_SENSITIVITY;

        sumP +=
            (vSample - meanV) *
            (iSample - meanI);

        powerSamples++;
    }

    HAL_ADC_Stop(&hadc1);

    if (powerSamples > 0)
    {
        RealPower = sumP / powerSamples;
    }
    else
    {
        RealPower = 0.0f;
    }

    if (RealPower < 0.0f)
    {
        RealPower = -RealPower;
    }

    /* Apparent power */

    ApparentPower = Voltage * Current;

    /* Power factor */

    if (ApparentPower > 1.0f)
    {
        PowerFactor =
            fabsf(RealPower) / ApparentPower;

        if (PowerFactor > 1.0f)
        {
            PowerFactor = 1.0f;
        }

        if (PowerFactor < 0.0f)
        {
            PowerFactor = 0.0f;
        }
    }
    else
    {
        PowerFactor = 0.0f;
    }
}

/* ========================== RELAY CONTROL ========================== */

static void ApplyCapacitorState(uint8_t state)
{
    HAL_GPIO_WritePin(
        GPIOA,
        RELAY1_PIN,
        (state & 0x01) ? RELAY_ON : RELAY_OFF
    );

    HAL_GPIO_WritePin(
        GPIOA,
        RELAY2_PIN,
        (state & 0x02) ? RELAY_ON : RELAY_OFF
    );

    HAL_GPIO_WritePin(
        GPIOA,
        RELAY3_PIN,
        (state & 0x04) ? RELAY_ON : RELAY_OFF
    );

    HAL_GPIO_WritePin(
        GPIOA,
        RELAY4_PIN,
        (state & 0x08) ? RELAY_ON : RELAY_OFF
    );

    capacitorState = state;
}

/* ========================== CAPACITANCE ========================== */

static float GetCapacitanceFromState(uint8_t state)
{
    float total = 0.0f;

    if (state & 0x01)
    {
        total += CAP1_UF;
    }

    if (state & 0x02)
    {
        total += CAP2_UF;
    }

    if (state & 0x04)
    {
        total += CAP3_UF;
    }

    if (state & 0x08)
    {
        total += CAP4_UF;
    }

    return total;
}

/* ========================== REQUIRED CAPACITANCE ========================== */

static float CalculateRequiredCapacitance(void)
{
    if (Voltage < 100.0f)
    {
        return 0.0f;
    }

    if (Current < 0.05f)
    {
        return 0.0f;
    }

    if (PowerFactor >= PF_HIGH_LIMIT)
    {
        return 0.0f;
    }

    float S = ApparentPower;
    float P = fabsf(RealPower);

    if (S <= P)
    {
        return 0.0f;
    }

    float Qsquare =
        (S * S) -
        (P * P);

    if (Qsquare <= 0.0f)
    {
        return 0.0f;
    }

    float Qload = sqrtf(Qsquare);

    /*
     * Target PF = 0.95
     */

    float targetAngle = acosf(0.95f);

    float Qdesired =
        P * tanf(targetAngle);

    float Qc =
        Qload - Qdesired;

    if (Qc <= 0.0f)
    {
        return 0.0f;
    }

    /*
     * Qc = V^2 * 2*pi*f*C
     *
     * C = Qc / (V^2 * 2*pi*f)
     */

    float denominator =
        Voltage *
        Voltage *
        2.0f *
        3.14159265f *
        MAINS_FREQUENCY;

    if (denominator <= 0.0f)
    {
        return 0.0f;
    }

    float C_F = Qc / denominator;

    return C_F * 1000000.0f;
}

/* ========================== CAPACITOR COMBINATION ========================== */

static uint8_t FindBestCapacitorCombination(float required_uF)
{
    uint8_t bestState = 0;

    float bestDifference = 999999.0f;

    for (uint8_t state = 0; state < 16; state++)
    {
        float available =
            GetCapacitanceFromState(state);

        float difference =
            fabsf(available - required_uF);

        if (difference < bestDifference)
        {
            bestDifference = difference;
            bestState = state;
        }
    }

    return bestState;
}

/* ========================== STRICT PF CONTROL ========================== */

static void CorrectPowerFactor(void)
{
    /*
     * STRICT LOGIC
     *
     * PF < 0.85
     *     Capacitor correction is allowed.
     *
     * 0.85 <= PF < 0.95
     *     HOLD the existing capacitor state.
     *
     * PF >= 0.95
     *     Turn all capacitors OFF.
     */

    /* Invalid/no-load condition */

    if (Voltage < 100.0f ||
        Current < 0.05f ||
        ApparentPower < 10.0f)
    {
        if (capacitorState != 0)
        {
            ApplyCapacitorState(0);
        }

        return;
    }

    /* PF >= 0.95 -> all capacitors OFF */

    if (PowerFactor >= PF_HIGH_LIMIT)
    {
        if (capacitorState != 0)
        {
            ApplyCapacitorState(0);
        }

        return;
    }

    /*
     * 0.85 <= PF < 0.95
     *
     * DO NOTHING.
     *
     * Existing relay state remains unchanged.
     */

    if (PowerFactor >= PF_LOW_LIMIT)
    {
        return;
    }

    /*
     * PF < 0.85
     *
     * Only here are we allowed to change
     * the capacitor combination.
     */

    float required_uF =
        CalculateRequiredCapacitance();

    if (required_uF <= 0.0f)
    {
        return;
    }

    uint8_t newState =
        FindBestCapacitorCombination(required_uF);

    /*
     * Change relay state only if necessary.
     */

    if (newState != capacitorState)
    {
        ApplyCapacitorState(newState);
    }
}

/* ========================== MAIN ========================== */

int main(void)
{
    HAL_Init();

    SystemClock_Config();

    MX_GPIO_Init();

    MX_ADC1_Init();

    MX_I2C1_Init();

    /* ADC calibration */

    if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }

    /* LCD */

    lcd_init();

    lcd_clear();

    lcd_goto_XY(0, 0);
    lcd_send_string("APFC SYSTEM");

    lcd_goto_XY(1, 0);
    lcd_send_string("STARTING...");

    HAL_Delay(1500);

    /* All relays OFF */

    ApplyCapacitorState(0);

    /* Offset calibration message */

    lcd_clear();

    lcd_goto_XY(0, 0);
    lcd_send_string("CURRENT OFFSET");

    lcd_goto_XY(1, 0);
    lcd_send_string("NO LOAD...");

    HAL_Delay(1500);

    CalibrateOffsets();

    lcd_clear();

    lcd_goto_XY(0, 0);
    lcd_send_string("OFFSET READY");

    HAL_Delay(1000);

    uint32_t lastCorrectionTime = 0;
    uint32_t lastDisplayTime = 0;

    uint8_t screen = 0;

    while (1)
    {
        /* Read measurements */

        ReadPowerParameters();

        /*
         * Capacitor correction every 5 seconds.
         *
         * This prevents rapid relay switching.
         */

        if ((HAL_GetTick() - lastCorrectionTime) >= 5000)
        {
            lastCorrectionTime = HAL_GetTick();

            CorrectPowerFactor();
        }

        /* LCD update every 2 seconds */

        if ((HAL_GetTick() - lastDisplayTime) >= 2000)
        {
            lastDisplayTime = HAL_GetTick();

            lcd_clear();

            if (screen == 0)
            {
                sprintf(
                    lcd,
                    "V:%6.1f V",
                    Voltage
                );

                lcd_goto_XY(0, 0);
                lcd_send_string(lcd);

                sprintf(
                    lcd,
                    "I:%6.2f A",
                    Current
                );

                lcd_goto_XY(1, 0);
                lcd_send_string(lcd);
            }
            else
            {
                sprintf(
                    lcd,
                    "P:%6.1f W",
                    RealPower
                );

                lcd_goto_XY(0, 0);
                lcd_send_string(lcd);

                sprintf(
                    lcd,
                    "PF:%5.2f",
                    PowerFactor
                );

                lcd_goto_XY(1, 0);
                lcd_send_string(lcd);
            }

            screen = !screen;
        }

        HAL_Delay(10);
    }
}

/* ========================== SYSTEM CLOCK ========================== */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

    RCC_OscInitStruct.OscillatorType =
        RCC_OSCILLATORTYPE_HSE;

    RCC_OscInitStruct.HSEState =
        RCC_HSE_ON;

    RCC_OscInitStruct.HSEPredivValue =
        RCC_HSE_PREDIV_DIV1;

    RCC_OscInitStruct.HSIState =
        RCC_HSI_ON;

    RCC_OscInitStruct.PLL.PLLState =
        RCC_PLL_ON;

    RCC_OscInitStruct.PLL.PLLSource =
        RCC_PLLSOURCE_HSE;

    RCC_OscInitStruct.PLL.PLLMUL =
        RCC_PLL_MUL9;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_SYSCLK |
        RCC_CLOCKTYPE_PCLK1 |
        RCC_CLOCKTYPE_PCLK2;

    RCC_ClkInitStruct.SYSCLKSource =
        RCC_SYSCLKSOURCE_PLLCLK;

    RCC_ClkInitStruct.AHBCLKDivider =
        RCC_SYSCLK_DIV1;

    RCC_ClkInitStruct.APB1CLKDivider =
        RCC_HCLK_DIV2;

    RCC_ClkInitStruct.APB2CLKDivider =
        RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(
            &RCC_ClkInitStruct,
            FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }

    PeriphClkInit.PeriphClockSelection =
        RCC_PERIPHCLK_ADC;

    PeriphClkInit.AdcClockSelection =
        RCC_ADCPCLK2_DIV6;

    if (HAL_RCCEx_PeriphCLKConfig(
            &PeriphClkInit) != HAL_OK)
    {
        Error_Handler();
    }
}

/* ========================== ADC1 ========================== */

static void MX_ADC1_Init(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    hadc1.Instance = ADC1;

    hadc1.Init.ScanConvMode =
        ADC_SCAN_ENABLE;

    hadc1.Init.ContinuousConvMode =
        ENABLE;

    hadc1.Init.DiscontinuousConvMode =
        DISABLE;

    hadc1.Init.ExternalTrigConv =
        ADC_SOFTWARE_START;

    hadc1.Init.DataAlign =
        ADC_DATAALIGN_RIGHT;

    hadc1.Init.NbrOfConversion = 2;

    if (HAL_ADC_Init(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }

    /* PA0 - Voltage */

    sConfig.Channel =
        ADC_CHANNEL_0;

    sConfig.Rank =
        ADC_REGULAR_RANK_1;

    sConfig.SamplingTime =
        ADC_SAMPLETIME_55CYCLES_5;

    if (HAL_ADC_ConfigChannel(
            &hadc1,
            &sConfig) != HAL_OK)
    {
        Error_Handler();
    }

    /* PA1 - ACS712 */

    sConfig.Channel =
        ADC_CHANNEL_1;

    sConfig.Rank =
        ADC_REGULAR_RANK_2;

    sConfig.SamplingTime =
        ADC_SAMPLETIME_55CYCLES_5;

    if (HAL_ADC_ConfigChannel(
            &hadc1,
            &sConfig) != HAL_OK)
    {
        Error_Handler();
    }
}

/* ========================== I2C1 ========================== */

static void MX_I2C1_Init(void)
{
    hi2c1.Instance = I2C1;

    hi2c1.Init.ClockSpeed = 100000;

    hi2c1.Init.DutyCycle =
        I2C_DUTYCYCLE_2;

    hi2c1.Init.OwnAddress1 = 0;

    hi2c1.Init.AddressingMode =
        I2C_ADDRESSINGMODE_7BIT;

    hi2c1.Init.DualAddressMode =
        I2C_DUALADDRESS_DISABLE;

    hi2c1.Init.OwnAddress2 = 0;

    hi2c1.Init.GeneralCallMode =
        I2C_GENERALCALL_DISABLE;

    hi2c1.Init.NoStretchMode =
        I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&hi2c1) != HAL_OK)
    {
        Error_Handler();
    }
}

/* ========================== GPIO ========================== */

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* PA0 and PA1 - ADC */

    GPIO_InitStruct.Pin =
        GPIO_PIN_0 |
        GPIO_PIN_1;

    GPIO_InitStruct.Mode =
        GPIO_MODE_ANALOG;

    HAL_GPIO_Init(
        GPIOA,
        &GPIO_InitStruct
    );

    /* PA3 to PA6 - Relay outputs */

    GPIO_InitStruct.Pin =
        GPIO_PIN_3 |
        GPIO_PIN_4 |
        GPIO_PIN_5 |
        GPIO_PIN_6;

    GPIO_InitStruct.Mode =
        GPIO_MODE_OUTPUT_PP;

    GPIO_InitStruct.Pull =
        GPIO_NOPULL;

    GPIO_InitStruct.Speed =
        GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(
        GPIOA,
        &GPIO_InitStruct
    );

    /* All relays OFF */

    HAL_GPIO_WritePin(
        GPIOA,
        GPIO_PIN_3 |
        GPIO_PIN_4 |
        GPIO_PIN_5 |
        GPIO_PIN_6,
        RELAY_OFF
    );
}

/* ========================== ERROR HANDLER ========================== */

void Error_Handler(void)
{
    __disable_irq();

    while (1)
    {
    }
}

#ifdef USE_FULL_ASSERT

void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}

#endif
