// #include <stdio.h>
// #include <stdint.h>
// #include <string.h>
// #include <stdlib.h>
// #include "definitions.h"
// #include "click_routines/usb_uart/usb_uart.h"
// #include "utils.h"

// // Configuration - Set these differently for each node
// #define NODE_DELAY_MS 2000 // How long this node waits before sending data (make unique per node)
// #define ROUND_DURATION_MS 17000UL
// #define CH_PROBABILITY 0.25 // 25% chance to become CH
// #define CH_AD_INTERVAL_MS 3000

// // Message format
// #define MSG_START '>'
// #define MSG_END '<'
// #define CH_AD_HEADER "CH_AD"
// #define JOIN_HEADER "JOIN"
// #define DATA_HEADER "DATA"

// typedef struct
// {
//     char nodeID[16];
//     int sensorValue;
// } MemberData;

// static MemberData receivedData[5]; // Store up to 5 members' data
// static uint8_t dataCount = 0;
// static uint8_t isClusterHead = 0;
// static uint8_t hasJoined = 0;
// static uint32_t roundStartTime = 0;
// static char clusterHeadID[16] = {0};

// void broadcast(const char *type, const char *content)
// {
//     char msg[50];
//     snprintf(msg, sizeof(msg), "%c%s,%s%c", MSG_START, type, content, MSG_END);

//     // Send via UART
//     char cmd[30];
//     sprintf(cmd, "AT+RDATAB:%02X\r", (uint8_t)strlen(msg));
//     usb_uart_USART_Write((uint8_t *)cmd, strlen(cmd));
//     delayMs(20);
//     usb_uart_USART_Write((uint8_t *)msg, strlen(msg));
// }

// void handleMessage(const char *msg)
// {
//     char type[16], content[32];
//     if (sscanf(msg, "%*c%[^,],%[^<]", type, content) != 2)
//         return;

//     if (strcmp(type, CH_AD_HEADER) == 0 && !isClusterHead && !hasJoined)
//     {
//         strncpy(clusterHeadID, content, sizeof(clusterHeadID) - 1);
//         broadcast(JOIN_HEADER, "NODE_ID"); // Replace with your node ID
//         hasJoined = 1;
//     }
//     else if (strcmp(type, DATA_HEADER) == 0 && isClusterHead)
//     {
//         // Store member data
//         if (dataCount < 5)
//         {
//             sscanf(content, "%[^,],%d",
//                    receivedData[dataCount].nodeID,
//                    &receivedData[dataCount].sensorValue);
//             dataCount++;
//         }
//     }
// }

// void processInput()
// {
//     static char buffer[50];
//     static uint8_t idx = 0;

//     while (!usb_uart_USART_ReadIsBusy())
//     {
//         uint8_t byte;
//         if (usb_uart_USART_Read(&byte, 1) == 1)
//         {
//             if (byte == MSG_START)
//             {
//                 idx = 0;
//                 buffer[idx++] = byte;
//             }
//             else if (byte == MSG_END && idx > 0)
//             {
//                 buffer[idx++] = byte;
//                 buffer[idx] = '\0';
//                 handleMessage(buffer);
//                 idx = 0;
//             }
//             else if (idx < sizeof(buffer) - 1)
//             {
//                 buffer[idx++] = byte;
//             }
//         }
//     }
// }

// void runAsClusterHead()
// {
//     uint32_t now = getMsCount();

//     // Periodic CH advertisements
//     if (now - roundStartTime > CH_AD_INTERVAL_MS)
//     {
//         broadcast(CH_AD_HEADER, "CH_ID"); // Replace with your CH ID
//         roundStartTime = now;
//     }

//     // End of round processing
//     if (now - roundStartTime > ROUND_DURATION_MS - 2000)
//     {
//         printf("\n--- Aggregated Data ---\n");
//         for (int i = 0; i < dataCount; i++)
//         {
//             printf("Node %s: Value %d\n",
//                    receivedData[i].nodeID,
//                    receivedData[i].sensorValue);
//         }
//         printf("----------------------\n");
//         dataCount = 0;
//     }
// }

// void runAsMember()
// {
//     uint32_t now = getMsCount();
//     static uint8_t hasSent = 0;

//     if (!hasSent && now - roundStartTime > NODE_DELAY_MS)
//     {
//         int sensorValue = readSensor(); // Implement your sensor reading
//         char data[32];
//         snprintf(data, sizeof(data), "NODE_ID,%d", sensorValue); // Replace NODE_ID
//         broadcast(DATA_HEADER, data);
//         hasSent = 1;

//         // Visual feedback
//         PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14; // LED ON
//         delayMs(200);
//         PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14; // LED OFF
//     }
// }

// void leach_main()
// {
//     systemInitialize();

//     // Elect cluster head
//     isClusterHead = (rand() % 100) < (CH_PROBABILITY * 100);
//     roundStartTime = getMsCount();

//     if (isClusterHead)
//     {
//         broadcast(CH_AD_HEADER, "CH_ID"); // Replace with your CH ID
//         printf("I am the Cluster Head this round!\n");
//     }
//     else
//     {
//         printf("I am a member node this round\n");
//     }

//     while (1)
//     {
//         processInput();

//         if (isClusterHead)
//         {
//             runAsClusterHead();
//         }
//         else if (hasJoined)
//         {
//             runAsMember();
//         }

//         // Round completion
//         if (getMsCount() - roundStartTime > ROUND_DURATION_MS)
//         {
//             isClusterHead = 0;
//             hasJoined = 0;
//             roundStartTime = getMsCount();
//             // Re-elect roles
//             isClusterHead = (rand() % 100) < (CH_PROBABILITY * 100);
//             if (isClusterHead)
//                 broadcast(CH_AD_HEADER, "CH_ID");
//         }

//         delayMs(10);
//     }
// }