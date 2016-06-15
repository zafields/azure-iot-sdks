// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdio.h>
#include <stdlib.h>

#include "iothub_client_ll.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "iothubtransportmqtt.h"
#include "azure_c_shared_utility/platform.h"

//#define USE_CERT    1

#ifdef USE_CERT
#include "certs.h"
#endif

static const char* connectionString = "[device connection string]";

static int g_callbackCounter = 0;
static bool g_reconnect = true;
static bool g_continueRunning = true;
#define SEND_DATA_SIZE      1024*5
#define MESSAGES_TIL_SEND   20000

DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_CONFIRMATION_RESULT, IOTHUB_CLIENT_CONFIRMATION_RESULT_VALUES);

static IOTHUBMESSAGE_DISPOSITION_RESULT ReceiveMessageCallback(IOTHUB_MESSAGE_HANDLE message, void* userContextCallback)
{
    int* counter = (int*)userContextCallback;
    const char* buffer;
    size_t size;

    if (IoTHubMessage_GetByteArray(message, (const unsigned char**)&buffer, &size) != IOTHUB_MESSAGE_OK)
    {
        (void)printf("unable to retrieve the message data\r\n");
    }
    else
    {
        (void)printf("Received Message [%d] with Data: <<<%.*s>>> & Size=%d\r\n", *counter, (int)size, buffer, (int)size);
        // If we receive the work 'quit' then we stop running
        if (memcmp(buffer, "quit", size) == 0)
        {
            g_continueRunning = false;
        }
    }

    /* Some device specific action code goes here... */
    (*counter)++;
    return IOTHUBMESSAGE_ACCEPTED;
}

static void SendConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    int msgNum = (int)userContextCallback;
    (void)printf("Confirmation[%d] received for message.  Confirm Result %s\r\n", msgNum, ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result));
    if (result == IOTHUB_CLIENT_CONFIRMATION_MESSAGE_TIMEOUT)
    {
        g_reconnect = true;
    }
}

static IOTHUB_MESSAGE_HANDLE create_message_data()
{
    IOTHUB_MESSAGE_HANDLE messageHandle = NULL;
    char msgText[128];
    sprintf_s(msgText, sizeof(msgText), "{\"deviceId\":\"device\",\"msgCount\":%d}", g_callbackCounter);
    messageHandle = IoTHubMessage_CreateFromByteArray(msgText, strlen(msgText) );
    return messageHandle;
}

IOTHUB_CLIENT_LL_HANDLE connect_to_iothub(void* user_ctx)
{
    IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle = NULL;
    if ((iotHubClientHandle = IoTHubClient_LL_CreateFromConnectionString(connectionString, MQTT_Protocol)) != NULL)
    {
        bool traceOn = true;
        IoTHubClient_LL_SetOption(iotHubClientHandle, "logtrace", &traceOn);

        if (IoTHubClient_LL_SetMessageCallback(iotHubClientHandle, ReceiveMessageCallback, user_ctx) != IOTHUB_CLIENT_OK)
        {
            (void)printf("ERROR: IoTHubClient_LL_SetMessageCallback..........FAILED!\r\n");
        }
        else
        {
            // For mbed add the certificate information
#ifdef USE_CERT
            if (IoTHubClient_LL_SetOption(iotHubClientHandle, "TrustedCerts", certificates) != IOTHUB_CLIENT_OK)
            {
                printf("failure to set option \"TrustedCerts\"\r\n");
            }
#endif
            (void)printf("IoTHubClient_LL_SetMessageCallback...successful.\r\n");
        }

    }
    return iotHubClientHandle;
}

void iothub_client_sample_mqtt_run(void)
{
    g_continueRunning = true;
    
    int receiveContext = 0;

    if (platform_init() != 0)
    {
        (void)printf("Failed to initialize the platform.\r\n");
    }
    else
    {
        IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle = NULL;
        /* Now that we are ready to receive commands, let's send some messages */
        size_t iterator = MESSAGES_TIL_SEND+1;
        do
        {
            if (g_reconnect)
            {
                iterator = 0;
                IoTHubClient_LL_Destroy(iotHubClientHandle);
                iotHubClientHandle = connect_to_iothub(&receiveContext);
                if (iotHubClientHandle == NULL)
                {
                    break;
                }
                else
                {
                    g_reconnect = false;
                }
            }
            else
            {
                if (iterator > MESSAGES_TIL_SEND)
                {
                    int eventNum = g_callbackCounter;
                    IOTHUB_MESSAGE_HANDLE msgHandle = create_message_data();
                    if (msgHandle == NULL)
                    {
                        (void)printf("ERROR: iotHubMessageHandle is NULL... Ending\r\n");
                        break;
                    }
                    else
                    {
                        if (IoTHubClient_LL_SendEventAsync(iotHubClientHandle, msgHandle, SendConfirmationCallback, (void*)eventNum) != IOTHUB_CLIENT_OK)
                        {
                            (void)printf("ERROR: IoTHubClient_LL_SendEventAsync..........FAILED!\r\n");
                        }
                        else
                        {
                            g_callbackCounter++;
                            (void)printf("IoTHubClient_LL_SendEventAsync accepted message [%d] for transmission to IoT Hub.\r\n", eventNum);
                        }
                    }
                    iterator = 0;
                }
                iterator++;
            }
            IoTHubClient_LL_DoWork(iotHubClientHandle);
            ThreadAPI_Sleep(1);
        } while (g_continueRunning);

        IoTHubClient_LL_Destroy(iotHubClientHandle);
        platform_deinit();
    }
}

int main(void)
{
    iothub_client_sample_mqtt_run();
    return 0;
}
