#include "debug.h"
#include <stdint.h>

/*
 * CH32V003J4M6 VOLTMETER
 *
 * Physical pins:
 *
 * Pin 3 = PA2 = ADC channel 0
 * Pin 5 = PC1 = I2C SDA
 * Pin 6 = PC2 = I2C SCL
 *
 * Voltage divider:
 *
 * VIN ---- 68k ---- PA2 ---- 12k ---- GND
 *
 * Maximum VIN = approximately 22 V
 *
 * OLED:
 *
 * SSD1306
 * 64 x 32 pixels
 * I2C address = 0x3C
 *
 * Display refresh = 10 Hz
 */


/* ============================================================
   VOLTAGE DIVIDER
   ============================================================ */

#define ADC_REFERENCE_MV       3300UL

#define R_TOP_OHM              68000UL
#define R_BOTTOM_OHM           12000UL

/*
 * ADC is 10-bit:
 *
 * 0 ... 1023
 *
 * Instead of using floating point, all calculations are
 * performed using integer arithmetic.
 */


/* ============================================================
   OLED
   ============================================================ */

#define OLED_ADDRESS           0x3C

#define OLED_WIDTH             64
#define OLED_HEIGHT            32

#define OLED_PAGES             4
#define OLED_COLUMN_OFFSET     32


/* ============================================================
   I2C
   ============================================================ */

#define I2C_SPEED              100000UL


/* ============================================================
   TIMING
   ============================================================ */

#define UPDATE_INTERVAL_MS     100

/* ============================================================
   10 ms SYSTEM TICK (SysTick)
   ============================================================ */

#define TICK_PERIOD_MS         10UL
#define UPDATE_TICKS           (UPDATE_INTERVAL_MS / TICK_PERIOD_MS)
#define OLED_POWERUP_TICKS     (500UL / TICK_PERIOD_MS)

/*
 * Incremented by the SysTick interrupt every 10 ms.
 * volatile: it is modified inside an interrupt handler.
 */
static volatile uint32_t systemTicks = 0;

/*
 * WCH "fast interrupt" attribute for the RISC-V core.
 * https://www.wch-ic.com/downloads/CH32V003RM_PDF.html (PFIC chapter)
 */
void SysTick_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void SysTick_Handler(void)
{
    /* Clear the compare-match flag, otherwise the IRQ fires again. */
    SysTick->SR = 0;

    systemTicks++;
}

static void tick_init(void)
{
    SysTick->CTLR = 0;
    SysTick->SR   = 0;
    SysTick->CNT  = 0;

    /*
     * Counter runs from HCLK (48 MHz), so 10 ms = HCLK / 100 counts.
     * CH32V003 Reference Manual, SysTick chapter (CTLR/CMP registers):
     * https://www.wch-ic.com/downloads/CH32V003RM_PDF.html
     */
    SysTick->CMP = (SystemCoreClock / (1000UL / TICK_PERIOD_MS)) - 1UL;

    /* Note: WCH spells this IRQ name "SysTicK_IRQn" (capital K). */
    NVIC_EnableIRQ(SysTicK_IRQn);

    /*
     * CTLR = 0x0F:
     *   bit 0 STE   = enable counter
     *   bit 1 STIE  = enable interrupt
     *   bit 2 STCLK = clock from HCLK (not HCLK/8)
     *   bit 3 STRE  = auto-reload (restart at 0 on compare match)
     * CH32V003 Reference Manual, SysTick control register (CTLR).
     */
    SysTick->CTLR = 0x0F;
}
/* ============================================================
   OLED FRAMEBUFFER
   ============================================================ */

static uint8_t oledBuffer[OLED_WIDTH * OLED_PAGES];


/* ============================================================
   SIMPLE 5x7 FONT
   ============================================================

   Characters used:

   0 1 2 3 4 5 6 7 8 9
   .
   V

   Each character is 5 pixels wide.
   ============================================================ */

static const uint8_t font5x7[][5] =
{
    /* 0 */
    {0x3E, 0x51, 0x49, 0x45, 0x3E},

    /* 1 */
    {0x00, 0x42, 0x7F, 0x40, 0x00},

    /* 2 */
    {0x42, 0x61, 0x51, 0x49, 0x46},

    /* 3 */
    {0x21, 0x41, 0x45, 0x4B, 0x31},

    /* 4 */
    {0x18, 0x14, 0x12, 0x7F, 0x10},

    /* 5 */
    {0x27, 0x45, 0x45, 0x45, 0x39},

    /* 6 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30},

    /* 7 */
    {0x01, 0x71, 0x09, 0x05, 0x03},

    /* 8 */
    {0x36, 0x49, 0x49, 0x49, 0x36},

    /* 9 */
    {0x06, 0x49, 0x49, 0x29, 0x1E},

    /* . */
    {0x00, 0x60, 0x60, 0x00, 0x00},

    /* V */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}
};


/* ============================================================
   I2C INITIALIZATION
   ============================================================ */

static void i2c_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    I2C_InitTypeDef I2C_InitStructure = {0};


    /*
     * Enable GPIOC and AFIO clocks.
     */

    RCC_APB2PeriphClockCmd(
        RCC_APB2Periph_GPIOC |
        RCC_APB2Periph_AFIO,
        ENABLE
    );


    /*
     * Enable I2C1 peripheral clock.
     */

    RCC_APB1PeriphClockCmd(
        RCC_APB1Periph_I2C1,
        ENABLE
    );


    /*
     * PC1 = SDA
     * PC2 = SCL
     *
     * I2C requires alternate-function
     * open-drain operation.
     */

    GPIO_InitStructure.GPIO_Pin =
        GPIO_Pin_1 |
        GPIO_Pin_2;

    GPIO_InitStructure.GPIO_Mode =
        GPIO_Mode_AF_OD;

    GPIO_InitStructure.GPIO_Speed =
        GPIO_Speed_50MHz;

    GPIO_Init(
        GPIOC,
        &GPIO_InitStructure
    );


    /*
     * Configure I2C1.
     */

    I2C_InitStructure.I2C_ClockSpeed =
        I2C_SPEED;

    I2C_InitStructure.I2C_Mode =
        I2C_Mode_I2C;

    I2C_InitStructure.I2C_DutyCycle =
        I2C_DutyCycle_2;

    I2C_InitStructure.I2C_OwnAddress1 =
        0x00;

    I2C_InitStructure.I2C_Ack =
        I2C_Ack_Enable;

    I2C_InitStructure.I2C_AcknowledgedAddress =
        I2C_AcknowledgedAddress_7bit;


    I2C_Init(
        I2C1,
        &I2C_InitStructure
    );


    I2C_Cmd(
        I2C1,
        ENABLE
    );


    I2C_AcknowledgeConfig(
        I2C1,
        ENABLE
    );
}


/* ============================================================
   I2C WRITE
   ============================================================ */

static void i2c_write(
    uint8_t address,
    const uint8_t *data,
    uint16_t length
)
{
    uint16_t timeout;


    /*
     * Wait until the previous bus activity is finished.
     */

    timeout = 10000;

    while (
        I2C_GetFlagStatus(
            I2C1,
            I2C_FLAG_BUSY
        )
    )
    {
        if (--timeout == 0)
            return;
    }


    /*
     * START
     */

    I2C_GenerateSTART(
        I2C1,
        ENABLE
    );


    timeout = 10000;

    while (
        !I2C_CheckEvent(
            I2C1,
            I2C_EVENT_MASTER_MODE_SELECT
        )
    )
    {
        if (--timeout == 0)
            return;
    }


    /*
     * Send slave address.
     */

    I2C_Send7bitAddress(
        I2C1,
        address << 1,
        I2C_Direction_Transmitter
    );


    timeout = 10000;

    while (
        !I2C_CheckEvent(
            I2C1,
            I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED
        )
    )
    {
        if (--timeout == 0)
        {
            I2C_GenerateSTOP(I2C1, ENABLE);
            return;
        }
    }


    /*
     * Send data.
     */

    for (
        uint16_t i = 0;
        i < length;
        i++
    )
    {
        I2C_SendData(
            I2C1,
            data[i]
        );


        timeout = 10000;

        while (
            !I2C_CheckEvent(
                I2C1,
                I2C_EVENT_MASTER_BYTE_TRANSMITTED
            )
        )
        {
            if (--timeout == 0)
            {
                I2C_GenerateSTOP(
                    I2C1,
                    ENABLE
                );

                return;
            }
        }
    }


    /*
     * STOP
     */

    I2C_GenerateSTOP(
        I2C1,
        ENABLE
    );
}


/* ============================================================
   OLED COMMAND
   ============================================================ */

static void oled_command(
    uint8_t command
)
{
    uint8_t data[2];

    data[0] = 0x00;
    data[1] = command;

    i2c_write(
        OLED_ADDRESS,
        data,
        2
    );
}


/* ============================================================
   OLED INITIALIZATION
   ============================================================ */

static void oled_init(void)
{
    oled_command(0xAE); // Display OFF

    /*
     * Clock
     */
    oled_command(0xD5);
    oled_command(0x80);

    /*
     * Multiplex ratio: 32 rows
     */
    oled_command(0xA8);
    oled_command(0x1F);

    /*
     * Display offset
     */
    oled_command(0xD3);
    oled_command(0x00);

    /*
     * Start line = 0
     */
    oled_command(0x40);

    /*
     * Charge pump
     */
    oled_command(0x8D);
    oled_command(0x14);

    /*
     * Page addressing mode
     */
    oled_command(0x20);
    oled_command(0x02);

    /*
     * Segment remap
     */
    oled_command(0xA1);

    /*
     * COM scan direction
     */
    oled_command(0xC8);

    /*
     * COM pins
     */
    oled_command(0xDA);
    oled_command(0x12);

    /*
     * Contrast
     */
    oled_command(0x81);
    oled_command(0x8F);

    /*
     * Pre-charge
     */
    oled_command(0xD9);
    oled_command(0xF1);

    /*
     * VCOMH
     */
    oled_command(0xDB);
    oled_command(0x40);

    /*
     * Entire display follows RAM
     */
    oled_command(0xA4);

    /*
     * Normal display
     */
    oled_command(0xA6);

    /*
     * Display ON
     */
    oled_command(0xAF);

    /*
     * Clear framebuffer.
     */
    //oled_clear();
}

/* ============================================================
   OLED CLEAR
   ============================================================ */

static void oled_clear(void)
{
    for (
        uint16_t i = 0;
        i < sizeof(oledBuffer);
        i++
    )
    {
        oledBuffer[i] = 0;
    }
}


/* ============================================================
   DRAW CHARACTER
   ============================================================ */

static void oled_draw_char(
    uint8_t x,
    uint8_t y,
    uint8_t character
)
{
    uint8_t fontIndex;


    /*
     * Map character to font table.
     */

    if (
        character >= '0' &&
        character <= '9'
    )
    {
        fontIndex = character - '0';
    }
    else if (character == '.')
    {
        fontIndex = 10;
    }
    else if (character == 'V')
    {
        fontIndex = 11;
    }
    else
    {
        return;
    }


    /*
     * This font is 7 pixels high,
     * therefore y must be within page 0..3.
     */

    if (y >= OLED_HEIGHT)
        return;


    /*
     * Draw five columns.
     */

    for (
        uint8_t column = 0;
        column < 5;
        column++
    )
    {
        uint8_t xx = x + column;

        if (xx >= OLED_WIDTH)
            continue;


        uint8_t bits =
            font5x7[fontIndex][column];


        for (
            uint8_t row = 0;
            row < 7;
            row++
        )
        {
            uint8_t yy = y + row;

            if (yy >= OLED_HEIGHT)
                continue;


            if (bits & (1 << row))
            {
                uint16_t index =
                    (yy / 8) * OLED_WIDTH +
                    xx;

                oledBuffer[index] |=
                    (1 << (yy % 8));
            }
        }
    }
}


/* ============================================================
   DRAW VOLTAGE
   ============================================================ */

static void oled_draw_voltage(
    uint32_t voltage_centi
)
{
    char text[8];

    /*
     * voltage_centi is voltage multiplied by 100.
     *
     * Example:
     *
     * 12.34 V
     *
     * becomes:
     *
     * 1234
     */


    uint32_t integerPart =
        voltage_centi / 100;

    uint32_t decimalPart =
        voltage_centi % 100;


    /*
     * We only expect 0.00 ... 22.00 V.
     */

    text[0] =
        '0' + (integerPart / 10);

    text[1] =
        '0' + (integerPart % 10);

    text[2] = '.';

    text[3] =
        '0' + (decimalPart / 10);

    text[4] =
        '0' + (decimalPart % 10);

    text[5] = 'V';

    text[6] = '\0';


    /*
     * Six characters.
     *
     * Character width = 6 pixels
     *
     * Total = 36 pixels.
     *
     * Center on a 64-pixel display.
     */

    uint8_t x =
        (OLED_WIDTH - 36) / 2;

    uint8_t y = 12;


    for (
        uint8_t i = 0;
        i < 6;
        i++
    )
    {
        oled_draw_char(
            x,
            y,
            text[i]
        );

        x += 6;
    }
}
/* ============================================================
   DRAW RAW NUMBER (DEBUG)
   ============================================================ */

static void oled_draw_raw(
    uint16_t value
)
{
    /*
     * 12-bit ADC result: 0..4095, always fits in 4 digits.
     * CH32V003 Reference Manual, ADC chapter (12-bit data register):
     * https://www.wch-ic.com/downloads/CH32V003RM_PDF.html
     */
    if (value > 9999)
        value = 9999;

    /*
     * 4 characters x 6 pixels = 24 pixels, centered on 64 pixels.
     */
    uint8_t x = (OLED_WIDTH - 24) / 2;
    uint8_t y = 12;

    /*
     * Extract digits from most to least significant,
     * reusing oled_draw_char() from the existing code.
     */
    uint16_t divisor = 1000;

    for (uint8_t i = 0; i < 4; i++)
    {
        oled_draw_char(
            x,
            y,
            '0' + ((value / divisor) % 10)
        );

        divisor /= 10;
        x += 6;
    }
}

/* ============================================================
   OLED UPDATE
   ============================================================ */

static void oled_update(void)
{
    uint8_t packet[65];

    packet[0] = 0x40;

    for (uint8_t page = 0;
         page < OLED_PAGES;
         page++)
    {
        /*
         * Select page.
         */
        oled_command(0xB0 | page);

        /*
         * Start at column 0.
         */
        oled_command(0x00 | (OLED_COLUMN_OFFSET & 0x0F));
        oled_command(0x10 | (OLED_COLUMN_OFFSET >> 4));

        /*
         * Copy framebuffer page.
         */
        for (uint8_t column = 0;
             column < OLED_WIDTH;
             column++)
        {
            packet[column + 1] =
                oledBuffer[
                    page * OLED_WIDTH + column
                ];
        }

        i2c_write(
            OLED_ADDRESS,
            packet,
            65
        );
    }
}
/* ============================================================
   ADC INITIALIZATION
   ============================================================ */

static void adc_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    ADC_InitTypeDef ADC_InitStructure = {0};


    /*
     * PA2 = ADC channel 0
     */

    RCC_APB2PeriphClockCmd(
        RCC_APB2Periph_GPIOA |
        RCC_APB2Periph_ADC1,
        ENABLE
    );


    /*
     * Configure PA2 as analog input.
     */

    GPIO_InitStructure.GPIO_Pin =
        GPIO_Pin_2;

    GPIO_InitStructure.GPIO_Mode =
        GPIO_Mode_AIN;

    GPIO_Init(
        GPIOA,
        &GPIO_InitStructure
    );


    /*
     * Configure ADC1.
     */

    ADC_DeInit(ADC1);


    ADC_InitStructure.ADC_Mode =
        ADC_Mode_Independent;

    ADC_InitStructure.ADC_ScanConvMode =
        DISABLE;

    ADC_InitStructure.ADC_ContinuousConvMode =
        DISABLE;

    ADC_InitStructure.ADC_ExternalTrigConv =
        ADC_ExternalTrigConv_None;

    ADC_InitStructure.ADC_DataAlign =
        ADC_DataAlign_Right;

    ADC_InitStructure.ADC_NbrOfChannel =
        1;


    ADC_Init(
        ADC1,
        &ADC_InitStructure
    );


    /*
     * Enable ADC.
     */

    ADC_Cmd(
        ADC1,
        ENABLE
    );


    /*
     * ADC calibration.
     */

    ADC_ResetCalibration(ADC1);

    while (
        ADC_GetResetCalibrationStatus(ADC1)
    );


    ADC_StartCalibration(ADC1);

    while (
        ADC_GetCalibrationStatus(ADC1)
    );
}


/* ============================================================
   ADC READ
   ============================================================ */

static uint16_t adc_read(void)
{
    uint16_t value;


    /*
     * PA2 = ADC channel 0.
     *
     * 241 ADC cycles gives the ADC plenty
     * of acquisition time for the relatively
     * high impedance 68k / 12k divider.
     */

    ADC_RegularChannelConfig(
        ADC1,
        ADC_Channel_0,
        1,
        ADC_SampleTime_241Cycles
    );


    ADC_SoftwareStartConvCmd(
        ADC1,
        ENABLE
    );


    while (
        !ADC_GetFlagStatus(
            ADC1,
            ADC_FLAG_EOC
        )
    );


    value =
        ADC_GetConversionValue(ADC1);


    return value;
}


/* ============================================================
   ADC AVERAGE
   ============================================================ */

static uint16_t adc_read_average(void)
{
    uint32_t sum = 0;


    /*
     * Eight samples are averaged.
     *
     * This happens very quickly compared
     * with the 100 ms display period.
     */

    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        sum += adc_read();
    }


    return (uint16_t)(sum / 8);
}


/* ============================================================
   CONVERT ADC TO INPUT VOLTAGE
   ============================================================ */

static uint32_t adc_to_voltage_centi(
    uint16_t adc
)
{
    /*
     * We want the result in centivolts.
     *
     * Example:
     *
     * 12.34 V
     *
     * result = 1234
     *
     *
     * ADC voltage:
     *
     * Vadc = ADC * 3300 / 4095
     *
     *
     * Input voltage:
     *
     * Vin = Vadc * 80000 / 12000
     *
     *
     * Therefore:
     *
     * Vin_centi =
     *
     * ADC * 3300 * 80000 * 100
     * -------------------------
     * 4095 * 12000
     *
     * This is calculated using 64-bit
     * arithmetic to avoid overflow.
     */


    /*
     * Units: ADC * mV * ohm / ohm = mV at the input.
     * Converting mV to centivolts means dividing by 10
     * (1 cV = 10 mV), so the 10 goes in the denominator.
     *
     * Quick check: ADC = 614 (3.3 V on VIN)
     *   614 * 3300 * 80000 / (4095 * 12000 * 10) = 330  ->  3.30 V
     */
    uint64_t numerator =
        (uint64_t)adc *
        ADC_REFERENCE_MV *
        (R_TOP_OHM + R_BOTTOM_OHM);


    uint64_t denominator =
        1023ULL *
        R_BOTTOM_OHM *
        10ULL;


    return (uint32_t)(
        (numerator + denominator / 2) /
        denominator
    );
}


/* ============================================================
   MAIN
   ============================================================ */

int main(void)
{
    SystemCoreClockUpdate();

    /* Delay_Init() is gone: it also touches SysTick. */
    tick_init();
    i2c_init();
    adc_init();

    uint8_t  oledReady  = 0;
    uint32_t nextUpdate = 0;

    while (1)
    {
        uint32_t now = systemTicks;

        /*
         * Non-blocking OLED power-up: initialize it once
         * 500 ms have passed since boot.
         */
        if (!oledReady)
        {
            if (now >= OLED_POWERUP_TICKS)
            {
                oled_init();
                oledReady  = 1;
                nextUpdate = now;
            }

            continue;
        }

        /*
         * Signed subtraction keeps the comparison correct even when
         * the 32-bit tick counter wraps around (after ~1.3 years).
         */
        if ((int32_t)(now - nextUpdate) >= 0)
        {
            nextUpdate += UPDATE_TICKS;

            uint16_t adc = adc_read_average();

            uint32_t voltage = adc_to_voltage_centi(adc);

            if (voltage > 2200)
                voltage = 2200;

            oled_clear();
            oled_draw_voltage(voltage);
            oled_update();
        }
    }
}