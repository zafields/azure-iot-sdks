// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#define _POSIX_C_SOURCE 201112L

#include <stdio.h>
#include <stdlib.h>

#ifdef _CRT_DBG_MAP_ALLOC
#include <crtdbg.h>
#endif // _CRT_DBG_MAP_ALLOC

#include "iothub_client_ll.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "iothubtransportmqtt.h"
#include "azure_c_shared_utility/platform.h"

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#endif

//#define USE_CERT    1

#ifdef USE_CERT
#include "../../../certs/certs.h"
#endif

static const char* connectionString = "[device connection string]";
static const char* CONNECT_TEST_URI = "www.bing.com";
static const char* CONNECT_TEST_PORT = "80";

static int g_callbackCounter = 0;
static bool g_reconnect = true;
static bool g_continueRunning = true;
#define SEND_DATA_SIZE          1024*5
#define MESSAGES_TIL_SEND       10000
#define CONNECTION_RETRY        30
#define MAX_OUTSTANDING_MSGS    3

typedef struct MESSAGE_QUEUE_TAG
{
    size_t count;
    DLIST_ENTRY to_be_sent;
    DLIST_ENTRY wait_reply;
} MESSAGE_QUEUE;

typedef struct SEND_DATA_INFO_TAG
{
    int msg_count;
    IOTHUB_MESSAGE_HANDLE msg_handle;
    MESSAGE_QUEUE* queue;
    DLIST_ENTRY entry;
} SEND_DATA_INFO;

DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_CONFIRMATION_RESULT, IOTHUB_CLIENT_CONFIRMATION_RESULT_VALUES);


static void SendConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    SEND_DATA_INFO* dataInfo = (SEND_DATA_INFO*)userContextCallback;

    (void)printf("Confirmation[%d] received for message.  Confirm Result %s\r\n", dataInfo->msg_count, ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result));
    if (result == IOTHUB_CLIENT_CONFIRMATION_OK)
    {
        dataInfo->queue->count--;
        (void)DList_RemoveEntryList(&dataInfo->entry);
        IoTHubMessage_Destroy(dataInfo->msg_handle);
        free(dataInfo);
    }
    else
    {
        if (result == IOTHUB_CLIENT_CONFIRMATION_MESSAGE_TIMEOUT)
        {
            g_reconnect = true;
        }
        (void)DList_RemoveEntryList(&dataInfo->entry);
        // Move this waiting item to sent item
        DList_InsertTailList(&dataInfo->queue->to_be_sent, &dataInfo->entry);
    }
}

static bool send_message_data(IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle, MESSAGE_QUEUE* msgQueue)
{
    bool result;
    if (!DList_IsListEmpty(&msgQueue->to_be_sent) )
    {
        PDLIST_ENTRY currentListEntry = msgQueue->to_be_sent.Flink;
        SEND_DATA_INFO* dataInfo = containingRecord(currentListEntry, SEND_DATA_INFO, entry);

        if (IoTHubClient_LL_SendEventAsync(iotHubClientHandle, dataInfo->msg_handle, SendConfirmationCallback, (void*)dataInfo) != IOTHUB_CLIENT_OK)
        {
            (void)printf("ERROR: IoTHubClient_LL_SendEventAsync..........FAILED!\r\n");
            result = false;
        }
        else
        {
            // Remove the current entry from the to be sent list
            (void)DList_RemoveEntryList(currentListEntry); 

            // Add to the wait for reply list
            DList_InsertTailList(&msgQueue->wait_reply, &dataInfo->entry);

            (void)printf("IoTHubClient_LL_SendEventAsync accepted message for transmission to IoT Hub.\r\n");
            result = true;
        }
    }
    else
    {
        result = true;
    }
    return result;
}

static bool create_message_data(MESSAGE_QUEUE* msgQueue)
{
    bool result;
    // Construct the message as needed
    char msgText[128];
    sprintf_s(msgText, sizeof(msgText), "{\"deviceId\":\"device\",\"msgCount\":%d}", g_callbackCounter++);

    SEND_DATA_INFO* msgNode = malloc(sizeof(SEND_DATA_INFO));
    if (msgNode == NULL)
    {
        result = false;
    }
    else
    {
        msgNode->msg_count = g_callbackCounter;
        msgNode->queue = msgQueue;
        msgNode->msg_handle = IoTHubMessage_CreateFromByteArray(msgText, strlen(msgText));
        if (msgNode->msg_handle == NULL)
        {
            result = false;
        }
        else
        {
            DList_InsertTailList(&msgQueue->to_be_sent, &msgNode->entry);
            msgQueue->count++;
            result = true;
        }
    }
    return result;
}

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

bool is_device_connected()
{
    bool result;
    struct addrinfo addrHint = { 0 };
    struct addrinfo* addrInfo;
    addrHint.ai_family = AF_INET;
    addrHint.ai_socktype = SOCK_STREAM;
    addrHint.ai_protocol = 0;

    int err = getaddrinfo(CONNECT_TEST_URI, CONNECT_TEST_PORT, &addrHint, &addrInfo);
    if (err != 0)
    {
        result = false;
        printf("device is not connected...\r\n");
        // Wait for 30 seconds and try again
        ThreadAPI_Sleep(CONNECTION_RETRY*1000);
    }
    else
    {
        freeaddrinfo(addrInfo);
        result = true;
    }
    return result;
}

void iothub_client_sample_mqtt_run(void)
{
    MESSAGE_QUEUE msgQueue = {0};
    g_continueRunning = true;
    int send_time = MESSAGES_TIL_SEND;
    
    int receiveContext = 0;

    if (platform_init() != 0)
    {
        (void)printf("Failed to initialize the platform.\r\n");
    }
    else
    {
        DList_InitializeListHead(&msgQueue.to_be_sent);
        DList_InitializeListHead(&msgQueue.wait_reply);

        IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle = NULL;
        do
        {
            if (g_reconnect)
            {
                IoTHubClient_LL_Destroy(iotHubClientHandle);
                iotHubClientHandle = NULL;
                if (is_device_connected() )
                {
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
            }
            else
            {
                // Now that we are ready to receive commands, let's send some messages 
                if (send_time > MESSAGES_TIL_SEND)
                {
                    if (msgQueue.count < MAX_OUTSTANDING_MSGS)
                    {
                        if (!create_message_data(&msgQueue) )
                        {
                            (void)printf("ERROR: iotHubMessageHandle is NULL... Ending\r\n");
                            break;
                        }
                    }
                    send_time = 0;
                }
                send_time++;

                if (!send_message_data(iotHubClientHandle, &msgQueue))
                {
                    (void)printf("ERROR: sending message data\r\n");
                }
            }
            IoTHubClient_LL_DoWork(iotHubClientHandle);
            ThreadAPI_Sleep(1);
        } while (g_continueRunning);

        while (!DList_IsListEmpty(&msgQueue.to_be_sent))
        {
            PDLIST_ENTRY currentEntry = DList_RemoveHeadList(&msgQueue.to_be_sent);
            SEND_DATA_INFO* dataInfo = containingRecord(currentEntry, SEND_DATA_INFO, entry);
            IoTHubMessage_Destroy(dataInfo->msg_handle);
            free(dataInfo);
        }
        while (!DList_IsListEmpty(&msgQueue.wait_reply))
        {
            PDLIST_ENTRY currentEntry = DList_RemoveHeadList(&msgQueue.wait_reply);
            SEND_DATA_INFO* dataInfo = containingRecord(currentEntry, SEND_DATA_INFO, entry);
            IoTHubMessage_Destroy(dataInfo->msg_handle);
            free(dataInfo);
        }

        IoTHubClient_LL_Destroy(iotHubClientHandle);
        platform_deinit();
    }
}

int main(void)
{
    iothub_client_sample_mqtt_run();

#ifdef _CRT_DBG_MAP_ALLOC
    _CrtDumpMemoryLeaks();
#endif
    return 0;
}
