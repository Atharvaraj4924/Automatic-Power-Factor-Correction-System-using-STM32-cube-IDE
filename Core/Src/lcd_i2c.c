#include "lcd_i2c.h"

extern I2C_HandleTypeDef hi2c1;

#define LCD_BACKLIGHT 0x08
#define ENABLE        0x04
#define RS            0x01

static void lcd_write4bits(uint8_t data)
{
    uint8_t buf;

    buf = data | LCD_BACKLIGHT;
    HAL_I2C_Master_Transmit(&hi2c1, LCD_ADDR, &buf, 1, HAL_MAX_DELAY);

    buf |= ENABLE;
    HAL_I2C_Master_Transmit(&hi2c1, LCD_ADDR, &buf, 1, HAL_MAX_DELAY);

    HAL_Delay(1);

    buf &= ~ENABLE;
    HAL_I2C_Master_Transmit(&hi2c1, LCD_ADDR, &buf, 1, HAL_MAX_DELAY);

    HAL_Delay(1);
}

void lcd_send_cmd(char cmd)
{
    lcd_write4bits(cmd & 0xF0);
    lcd_write4bits((cmd << 4) & 0xF0);
}

void lcd_send_data(char data)
{
    lcd_write4bits((data & 0xF0) | RS);
    lcd_write4bits(((data << 4) & 0xF0) | RS);
}

void lcd_clear(void)
{
    lcd_send_cmd(0x01);
    HAL_Delay(2);
}

void lcd_goto_XY(uint8_t row, uint8_t col)
{
    uint8_t addr;

    if(row==0)
        addr=0x80+col;
    else
        addr=0xC0+col;

    lcd_send_cmd(addr);
}

void lcd_send_string(char *str)
{
    while(*str)
    {
        lcd_send_data(*str++);
    }
}

void lcd_init(void)
{
    HAL_Delay(50);

    lcd_write4bits(0x30);
    HAL_Delay(5);

    lcd_write4bits(0x30);
    HAL_Delay(5);

    lcd_write4bits(0x30);
    HAL_Delay(5);

    lcd_write4bits(0x20);
    HAL_Delay(5);

    lcd_send_cmd(0x28);
    lcd_send_cmd(0x0C);
    lcd_send_cmd(0x06);
    lcd_send_cmd(0x01);

    HAL_Delay(5);
}
