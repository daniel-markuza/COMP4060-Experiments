/*******************************************************************************
  USB UART click routine example source file

  Company
    Microchip Technology Inc.

  File Name
    usb_uart_example.c

  Summary
    USB UART click routine example implementation file.

  Description
    This file defines the usage of the USB UART click routine APIs.

  Remarks:
    None.

 *******************************************************************************/

// DOM-IGNORE-BEGIN
/*
    (c) 2021 Microchip Technology Inc. and its subsidiaries. You may use this
    software and any derivatives exclusively with Microchip products.

    THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
    EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
    WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
    PARTICULAR PURPOSE, OR ITS INTERACTION WITH MICROCHIP PRODUCTS, COMBINATION
    WITH ANY OTHER PRODUCTS, OR USE IN ANY APPLICATION.

    IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
    INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
    WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
    BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
    FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
    ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
    THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.

    MICROCHIP PROVIDES THIS SOFTWARE CONDITIONALLY UPON YOUR ACCEPTANCE OF THESE
    TERMS.
*/
// DOM-IGNORE-END

/**
  Section: Included Files
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "definitions.h" // SYS function prototypes
#include "usb_uart.h"
#include <xc.h>
#include "utils.h"
// setup our heartbeat to be 1ms: we overflow at 1ms intervals with a 48MHz clock
// uses the SysTicks unit so that we get reliable debugging (timer stops on breakpoints)
// this is a countdown timer that sends an interrupt when 0
#define MS_TICKS 48000UL

// number of millisecond between LED flashes
#define LED_FLASH_MS 1000UL
#define PATTERN_LENGTH 14 // Length of "RAW:-xxx,hi\r\n"

//// NOTE: this overflows every ~50 days, so I'm not going to care here...
// volatile uint32_t msCount = 0;

/**
  Section: Example Code
 */
// AT+SENDMCAST:01,FFFF,01,01,C091,0002,Test
// AT+RDATAB:XX then data of size XX

//
// uint8_t restart[] = "ATZ\r";
//
// uint8_t join_own_network[] = "AT+EN\r";
// uint8_t change_channel[] = "AT+CCHANGE:10\r"; // that's 10 hex! channels go from 11-26, aka 0B to 1A

char out_string[200];

//// Fires every 1ms
// void SysTick_Handler()
//{
//   msCount++;
// }

void usb_uart_example(void)
{

  systemInitialize();

  uint8_t read_chars[100];
  uint8_t buffer[10];

  usb_uart_USART_Write(restart, 4);
  while (usb_uart_USART_WriteIsBusy())
    ;
  // need to wait for an "OK" to come through
  usb_uart_USART_Read((uint8_t *)&read_chars, 2);
  while (usb_uart_USART_ReadIsBusy())
    ; // do nothing

  usb_uart_USART_Write(join_own_network, 6);
  while (usb_uart_USART_WriteIsBusy())
    ;
  // need to wait for an "OK" to come through
  usb_uart_USART_Read((uint8_t *)&read_chars, 2);
  while (usb_uart_USART_ReadIsBusy())
    ; // do nothing

  usb_uart_USART_Write(change_channel, 14);
  while (usb_uart_USART_WriteIsBusy())
    ;
  // need to wait for an "OK" to come through
  usb_uart_USART_Read((uint8_t *)&read_chars, 2);
  while (usb_uart_USART_ReadIsBusy())
    ; // do nothing

  // preload loop
  // start reading
  usb_uart_USART_Read((uint8_t *)&read_chars, 1);
  printf("Let's go\r\n");
  while (1)
  {
    // wait for an interrupt
    __WFI();

    if (!usb_uart_USART_ReadIsBusy())
    {
      sprintf(out_string, "%c", read_chars[0]);
      // usb_uart_USART_Write((uint8_t *)out_string,sizeof(out_string));
      printf(out_string);

      // read the next one
      usb_uart_USART_Read((uint8_t *)&read_chars, 1);
    }
    SERCOM5_USART_Read(&buffer, 10, true);
    if (buffer[0] != '\0')
    {
      // printf((char*)&buffer); // comment this out if your serial program does a local echo.
      //  send the string to the zigbee one char at a time
      for (int i = 0; i < 10 && buffer[i] != '\0'; i++)
      {
        usb_uart_USART_Write((uint8_t *)&buffer[i], 1);
      }
    }
    //     handle blinking the light
    handleLEDBlink();
  }
}
