/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2014-2017 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2014-2016 (c) Sten Grüner
 *    Copyright 2014-2015, 2017 (c) Florian Palm
 *    Copyright 2015-2016 (c) Chris Iatrou
 *    Copyright 2015-2016 (c) Oleksiy Vasylyev
 *    Copyright 2016 (c) Joakim L. Gilje
 *    Copyright 2016-2017 (c) Stefan Profanter, fortiss GmbH
 *    Copyright 2016 (c) TorbenD
 *    Copyright 2017 (c) frax2222
 *    Copyright 2017 (c) Mark Giraud, Fraunhofer IOSB
 *    Copyright 2019 (c) Kalycito Infotech Private Limited
 */

#include <open62541/transport_generated.h>
#include <open62541/transport_generated_encoding_binary.h>
#include <open62541/transport_generated_handling.h>
#include <open62541/types_generated_encoding_binary.h>
#include <open62541/types_generated_handling.h>
#include "open62541/plugin/network.h"

#include "ua_securechannel_manager.h"
#include "ua_server_internal.h"
#include "ua_services.h"

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
// store the authentication token and session ID so we can help fuzzing by setting
// these values in the next request automatically
UA_NodeId unsafe_fuzz_authenticationToken = {0, UA_NODEIDTYPE_NUMERIC, {0}};
#endif

#ifdef UA_DEBUG_DUMP_PKGS_FILE
void UA_debug_dumpCompleteChunk(UA_Server *const server, UA_Connection *const connection,
                                UA_ByteString *messageBuffer);
#endif

/********************/
/* Helper Functions */
/********************/

static UA_StatusCode
sendServiceFaultWithRequest(UA_SecureChannel *channel,
                            const UA_RequestHeader *requestHeader,
                            const UA_DataType *responseType,
                            UA_UInt32 requestId, UA_StatusCode error) {
    UA_STACKARRAY(UA_Byte, response, responseType->memSize);
    UA_init(response, responseType);
    UA_ResponseHeader *responseHeader = (UA_ResponseHeader*)response;
    responseHeader->requestHandle = requestHeader->requestHandle;
    responseHeader->timestamp = UA_DateTime_now();
    responseHeader->serviceResult = error;

    /* Send error message. Message type is MSG and not ERR, since we are on a
     * SecureChannel! */
    UA_StatusCode retval =
        UA_SecureChannel_sendSymmetricMessage(channel, requestId, UA_MESSAGETYPE_MSG,
                                              response, responseType);

    UA_LOG_DEBUG(channel->securityPolicy->logger, UA_LOGCATEGORY_SERVER,
                 "Sent ServiceFault with error code %s", UA_StatusCode_name(error));
    return retval;
}

 /* This is not an ERR message, the connection is not closed afterwards */
static UA_StatusCode
sendServiceFault(UA_SecureChannel *channel, const UA_ByteString *msg,
                 size_t offset, const UA_DataType *responseType,
                 UA_UInt32 requestId, UA_StatusCode error) {
    UA_RequestHeader requestHeader;
    UA_StatusCode retval = UA_RequestHeader_decodeBinary(msg, &offset, &requestHeader);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
    retval = sendServiceFaultWithRequest(channel, &requestHeader, responseType,
                                         requestId, error);
    UA_RequestHeader_clear(&requestHeader);
    return retval;
}

static void
getServicePointers(UA_UInt32 requestTypeId, const UA_DataType **requestType,
                   const UA_DataType **responseType, UA_Service *service,
                   UA_Boolean *requiresSession) {
    switch(requestTypeId) {
    case UA_NS0ID_GETENDPOINTSREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_GetEndpoints;
        *requestType = &UA_TYPES[UA_TYPES_GETENDPOINTSREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_GETENDPOINTSRESPONSE];
        *requiresSession = false;
        break;
    case UA_NS0ID_FINDSERVERSREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_FindServers;
        *requestType = &UA_TYPES[UA_TYPES_FINDSERVERSREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_FINDSERVERSRESPONSE];
        *requiresSession = false;
        break;
#ifdef UA_ENABLE_DISCOVERY
# ifdef UA_ENABLE_DISCOVERY_MULTICAST
    case UA_NS0ID_FINDSERVERSONNETWORKREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_FindServersOnNetwork;
        *requestType = &UA_TYPES[UA_TYPES_FINDSERVERSONNETWORKREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_FINDSERVERSONNETWORKRESPONSE];
        *requiresSession = false;
        break;
# endif
    case UA_NS0ID_REGISTERSERVERREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_RegisterServer;
        *requestType = &UA_TYPES[UA_TYPES_REGISTERSERVERREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_REGISTERSERVERRESPONSE];
        *requiresSession = false;
        break;
    case UA_NS0ID_REGISTERSERVER2REQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_RegisterServer2;
        *requestType = &UA_TYPES[UA_TYPES_REGISTERSERVER2REQUEST];
        *responseType = &UA_TYPES[UA_TYPES_REGISTERSERVER2RESPONSE];
        *requiresSession = false;
        break;
#endif
    case UA_NS0ID_CREATESESSIONREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)(uintptr_t)Service_CreateSession;
        *requestType = &UA_TYPES[UA_TYPES_CREATESESSIONREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_CREATESESSIONRESPONSE];
        *requiresSession = false;
        break;
    case UA_NS0ID_ACTIVATESESSIONREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)(uintptr_t)Service_ActivateSession;
        *requestType = &UA_TYPES[UA_TYPES_ACTIVATESESSIONREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_ACTIVATESESSIONRESPONSE];
        break;
    case UA_NS0ID_CLOSESESSIONREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)(uintptr_t)Service_CloseSession;
        *requestType = &UA_TYPES[UA_TYPES_CLOSESESSIONREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_CLOSESESSIONRESPONSE];
        break;
    case UA_NS0ID_READREQUEST_ENCODING_DEFAULTBINARY:
        *service = NULL;
        *service = (UA_Service)Service_Read;
        *requestType = &UA_TYPES[UA_TYPES_READREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_READRESPONSE];
        break;
    case UA_NS0ID_WRITEREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_Write;
        *requestType = &UA_TYPES[UA_TYPES_WRITEREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_WRITERESPONSE];
        break;
    case UA_NS0ID_BROWSEREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_Browse;
        *requestType = &UA_TYPES[UA_TYPES_BROWSEREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_BROWSERESPONSE];
        break;
    case UA_NS0ID_BROWSENEXTREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_BrowseNext;
        *requestType = &UA_TYPES[UA_TYPES_BROWSENEXTREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_BROWSENEXTRESPONSE];
        break;
    case UA_NS0ID_REGISTERNODESREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_RegisterNodes;
        *requestType = &UA_TYPES[UA_TYPES_REGISTERNODESREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_REGISTERNODESRESPONSE];
        break;
    case UA_NS0ID_UNREGISTERNODESREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_UnregisterNodes;
        *requestType = &UA_TYPES[UA_TYPES_UNREGISTERNODESREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_UNREGISTERNODESRESPONSE];
        break;
    case UA_NS0ID_TRANSLATEBROWSEPATHSTONODEIDSREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_TranslateBrowsePathsToNodeIds;
        *requestType = &UA_TYPES[UA_TYPES_TRANSLATEBROWSEPATHSTONODEIDSREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_TRANSLATEBROWSEPATHSTONODEIDSRESPONSE];
        break;

#ifdef UA_ENABLE_SUBSCRIPTIONS
    case UA_NS0ID_CREATESUBSCRIPTIONREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_CreateSubscription;
        *requestType = &UA_TYPES[UA_TYPES_CREATESUBSCRIPTIONREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_CREATESUBSCRIPTIONRESPONSE];
        break;
    case UA_NS0ID_PUBLISHREQUEST_ENCODING_DEFAULTBINARY:
        *requestType = &UA_TYPES[UA_TYPES_PUBLISHREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_PUBLISHRESPONSE];
        break;
    case UA_NS0ID_REPUBLISHREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_Republish;
        *requestType = &UA_TYPES[UA_TYPES_REPUBLISHREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_REPUBLISHRESPONSE];
        break;
    case UA_NS0ID_MODIFYSUBSCRIPTIONREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_ModifySubscription;
        *requestType = &UA_TYPES[UA_TYPES_MODIFYSUBSCRIPTIONREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_MODIFYSUBSCRIPTIONRESPONSE];
        break;
    case UA_NS0ID_SETPUBLISHINGMODEREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_SetPublishingMode;
        *requestType = &UA_TYPES[UA_TYPES_SETPUBLISHINGMODEREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_SETPUBLISHINGMODERESPONSE];
        break;
    case UA_NS0ID_DELETESUBSCRIPTIONSREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_DeleteSubscriptions;
        *requestType = &UA_TYPES[UA_TYPES_DELETESUBSCRIPTIONSREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_DELETESUBSCRIPTIONSRESPONSE];
        break;
    case UA_NS0ID_CREATEMONITOREDITEMSREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_CreateMonitoredItems;
        *requestType = &UA_TYPES[UA_TYPES_CREATEMONITOREDITEMSREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_CREATEMONITOREDITEMSRESPONSE];
        break;
    case UA_NS0ID_DELETEMONITOREDITEMSREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_DeleteMonitoredItems;
        *requestType = &UA_TYPES[UA_TYPES_DELETEMONITOREDITEMSREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_DELETEMONITOREDITEMSRESPONSE];
        break;
    case UA_NS0ID_MODIFYMONITOREDITEMSREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_ModifyMonitoredItems;
        *requestType = &UA_TYPES[UA_TYPES_MODIFYMONITOREDITEMSREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_MODIFYMONITOREDITEMSRESPONSE];
        break;
    case UA_NS0ID_SETMONITORINGMODEREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_SetMonitoringMode;
        *requestType = &UA_TYPES[UA_TYPES_SETMONITORINGMODEREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_SETMONITORINGMODERESPONSE];
        break;
#endif
#ifdef UA_ENABLE_HISTORIZING
        /* For History read */
    case UA_NS0ID_HISTORYREADREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_HistoryRead;
        *requestType = &UA_TYPES[UA_TYPES_HISTORYREADREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_HISTORYREADRESPONSE];
        break;
        /* For History update */
    case UA_NS0ID_HISTORYUPDATEREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_HistoryUpdate;
        *requestType = &UA_TYPES[UA_TYPES_HISTORYUPDATEREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_HISTORYUPDATERESPONSE];
        break;
#endif

#ifdef UA_ENABLE_METHODCALLS
    case UA_NS0ID_CALLREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_Call;
        *requestType = &UA_TYPES[UA_TYPES_CALLREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_CALLRESPONSE];
        break;
#endif

#ifdef UA_ENABLE_NODEMANAGEMENT
    case UA_NS0ID_ADDNODESREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_AddNodes;
        *requestType = &UA_TYPES[UA_TYPES_ADDNODESREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_ADDNODESRESPONSE];
        break;
    case UA_NS0ID_ADDREFERENCESREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_AddReferences;
        *requestType = &UA_TYPES[UA_TYPES_ADDREFERENCESREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_ADDREFERENCESRESPONSE];
        break;
    case UA_NS0ID_DELETENODESREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_DeleteNodes;
        *requestType = &UA_TYPES[UA_TYPES_DELETENODESREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_DELETENODESRESPONSE];
        break;
    case UA_NS0ID_DELETEREFERENCESREQUEST_ENCODING_DEFAULTBINARY:
        *service = (UA_Service)Service_DeleteReferences;
        *requestType = &UA_TYPES[UA_TYPES_DELETEREFERENCESREQUEST];
        *responseType = &UA_TYPES[UA_TYPES_DELETEREFERENCESRESPONSE];
        break;
#endif

    default:
        break;
    }
}

/*************************/
/* Process Message Types */
/*************************/

/* HEL -> Open up the connection */
static UA_StatusCode
processHEL(UA_Server *server, UA_SecureChannel *channel, const UA_ByteString *msg) {
    size_t offset = 8; /* Go to the beginning of the TcpHelloMessage */
    UA_TcpHelloMessage helloMessage;
    UA_StatusCode retval = UA_TcpHelloMessage_decodeBinary(msg, &offset, &helloMessage);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Currently not checked */
    UA_String_clear(&helloMessage.endpointUrl);

    /* Parameterize the connection. The TcpHelloMessage casts to a
     * TcpAcknowledgeMessage. */
    retval = UA_SecureChannel_processHELACK(channel,
                                            (UA_TcpAcknowledgeMessage*)&helloMessage);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_INFO(&server->config.logger, UA_LOGCATEGORY_NETWORK,
                    "Connection %i | Error during the HEL/ACK handshake",
                    (int)(channel->connection->sockfd));
        return retval;
    }

    /* Get the send buffer from the network layer */
    UA_Connection *connection = channel->connection;
    UA_ByteString ack_msg;
    UA_ByteString_init(&ack_msg);
    retval = connection->getSendBuffer(connection, channel->config.sendBufferSize, &ack_msg);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Build acknowledge response */
    UA_TcpAcknowledgeMessage ackMessage;
    ackMessage.protocolVersion = 0;
    ackMessage.receiveBufferSize = channel->config.recvBufferSize;
    ackMessage.sendBufferSize = channel->config.sendBufferSize;
    ackMessage.maxMessageSize = channel->config.localMaxMessageSize;
    ackMessage.maxChunkCount = channel->config.localMaxChunkCount;
    
    UA_TcpMessageHeader ackHeader;
    ackHeader.messageTypeAndChunkType = UA_MESSAGETYPE_ACK + UA_CHUNKTYPE_FINAL;
    ackHeader.messageSize = 8 + 20; /* ackHeader + ackMessage */

    /* Encode and send the response */
    UA_Byte *bufPos = ack_msg.data;
    const UA_Byte *bufEnd = &ack_msg.data[ack_msg.length];
    retval |= UA_TcpMessageHeader_encodeBinary(&ackHeader, &bufPos, bufEnd);
    retval |= UA_TcpAcknowledgeMessage_encodeBinary(&ackMessage, &bufPos, bufEnd);
    if(retval != UA_STATUSCODE_GOOD) {
        connection->releaseSendBuffer(connection, &ack_msg);
        return retval;
    }

    ack_msg.length = ackHeader.messageSize;
    return connection->send(connection, &ack_msg);
}

/* OPN -> Open up/renew the securechannel */
static UA_StatusCode
processOPN(UA_Server *server, UA_SecureChannel *channel,
           const UA_UInt32 requestId, const UA_ByteString *msg) {
    /* Decode the request */
    size_t offset = 0;
    UA_NodeId requestType;
    UA_OpenSecureChannelRequest openSecureChannelRequest;
    UA_StatusCode retval = UA_NodeId_decodeBinary(msg, &offset, &requestType);

    if(retval != UA_STATUSCODE_GOOD) {
        UA_NodeId_clear(&requestType);
        UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                               "Could not decode the NodeId. Closing the connection");
        UA_SecureChannelManager_close(&server->secureChannelManager, channel->securityToken.channelId);
        return retval;
    }
    retval = UA_OpenSecureChannelRequest_decodeBinary(msg, &offset, &openSecureChannelRequest);

    /* Error occurred */
    if(retval != UA_STATUSCODE_GOOD ||
       requestType.identifier.numeric != UA_TYPES[UA_TYPES_OPENSECURECHANNELREQUEST].binaryEncodingId) {
        UA_NodeId_clear(&requestType);
        UA_OpenSecureChannelRequest_clear(&openSecureChannelRequest);
        UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                               "Could not decode the OPN message. Closing the connection.");
        UA_SecureChannelManager_close(&server->secureChannelManager, channel->securityToken.channelId);
        return retval;
    }
    UA_NodeId_clear(&requestType);

    /* Call the service */
    UA_OpenSecureChannelResponse openScResponse;
    UA_OpenSecureChannelResponse_init(&openScResponse);
    Service_OpenSecureChannel(server, channel, &openSecureChannelRequest, &openScResponse);
    UA_OpenSecureChannelRequest_clear(&openSecureChannelRequest);
    if(openScResponse.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING_CHANNEL(&server->config.logger, channel, "Could not open a SecureChannel. "
                               "Closing the connection.");
        UA_SecureChannelManager_close(&server->secureChannelManager,
                                      channel->securityToken.channelId);
        return openScResponse.responseHeader.serviceResult;
    }

    /* Send the response */
    retval = UA_SecureChannel_sendAsymmetricOPNMessage(channel, requestId, &openScResponse,
                                                       &UA_TYPES[UA_TYPES_OPENSECURECHANNELRESPONSE]);
    UA_OpenSecureChannelResponse_clear(&openScResponse);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                               "Could not send the OPN answer with error code %s",
                               UA_StatusCode_name(retval));
        UA_SecureChannelManager_close(&server->secureChannelManager,
                                      channel->securityToken.channelId);
    }

    return retval;
}

static UA_StatusCode
decryptProcessOPN(UA_Server *server, UA_SecureChannel *channel,
                  UA_ByteString *msg) {
    UA_LOG_DEBUG_CHANNEL(&server->config.logger, channel, "Decrypt an OPN message");

    /* Skip the first header. We know length and message type. */
    size_t offset = UA_SECURE_CONVERSATION_MESSAGE_HEADER_LENGTH;

    /* Decode the asymmetric algorithm security header and call the callback
     * to perform checks. */
    UA_AsymmetricAlgorithmSecurityHeader asymHeader;
    UA_AsymmetricAlgorithmSecurityHeader_init(&asymHeader);
    UA_StatusCode retval =
        UA_AsymmetricAlgorithmSecurityHeader_decodeBinary(msg, &offset, &asymHeader);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                               "Could not decode the OPN header");
        return retval;
    }

    /* Verify the certificate before creating the SecureChannel with it */
    if(asymHeader.senderCertificate.length > 0) {
        retval = server->config.certificateVerification.
            verifyCertificate(server->config.certificateVerification.context,
                              &asymHeader.senderCertificate);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                                   "Could not verify the client's certificate");
            return retval;
        }
    }

    if(channel->state < UA_SECURECHANNELSTATE_OPEN) {
        retval = UA_SecureChannelManager_config(&server->secureChannelManager,
                                                channel, &asymHeader);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_AsymmetricAlgorithmSecurityHeader_clear(&asymHeader);
            return retval;
        }
    }

    retval = checkAsymHeader(channel, &asymHeader);
    UA_AsymmetricAlgorithmSecurityHeader_clear(&asymHeader);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                               "Could not verify OPN header");
        return retval;
    }

    /* After decryption, msg contains only the payload after the SequenceHeader */
    UA_UInt32 requestId = 0;
    UA_UInt32 sequenceNumber = 0;
    retval = decryptAndVerifyChunk(channel, &channel->securityPolicy->asymmetricModule.cryptoModule,
                                   UA_MESSAGETYPE_OPN, msg, offset, &requestId, &sequenceNumber);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                               "Could not decrypt and verify the OPN payload");
        return retval;
    }

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    retval = processSequenceNumberAsym(channel, sequenceNumber);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                               "Could not process the OPN message's sequence number");
        return retval;
    }
#endif

    return processOPN(server, channel, requestId, msg);
}

UA_StatusCode
sendResponse(UA_SecureChannel *channel, UA_UInt32 requestId, UA_UInt32 requestHandle,
             UA_ResponseHeader *responseHeader, const UA_DataType *responseType) {
    /* Prepare the ResponseHeader */
    responseHeader->requestHandle = requestHandle;
    responseHeader->timestamp = UA_DateTime_now();

    /* Start the message context */
    UA_MessageContext mc;
    UA_StatusCode retval = UA_MessageContext_begin(&mc, channel, requestId, UA_MESSAGETYPE_MSG);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Assert's required for clang-analyzer */
    UA_assert(mc.buf_pos == &mc.messageBuffer.data[UA_SECURE_MESSAGE_HEADER_LENGTH]);
    UA_assert(mc.buf_end <= &mc.messageBuffer.data[mc.messageBuffer.length]);

    /* Encode the response type */
    UA_NodeId typeId = UA_NODEID_NUMERIC(0, responseType->binaryEncodingId);
    retval = UA_MessageContext_encode(&mc, &typeId, &UA_TYPES[UA_TYPES_NODEID]);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Encode the response */
    retval = UA_MessageContext_encode(&mc, responseHeader, responseType);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Finish / send out */
    return UA_MessageContext_finish(&mc);
}

/* A Session is bound to at most one SecureChannel. After creation, the Session
 * is already bound to the SecureChannel on which the CreateSession request was
 * received. Even if the Session is not yet activated.
 *
 * The only way to rebind a Session to another SecureChannel is via the
 * ActivateSession request.
 *
 * A Session can only be closed from the SecureChannel to which it is bound.
 * (Also prior to ActivateSession.) */
static UA_StatusCode
processMSGDecoded(UA_Server *server, UA_SecureChannel *channel, UA_UInt32 requestId,
                  UA_Service service, const UA_RequestHeader *requestHeader,
                  const UA_DataType *requestType, UA_ResponseHeader *responseHeader,
                  const UA_DataType *responseType, UA_Boolean sessionRequired) {
    /* Does the Session bound to the SecureChannel match the
     * AuthenticationToken? */
    UA_Session *session = (UA_Session*)channel->session;
    if(session && !UA_NodeId_equal(&session->header.authenticationToken,
                                   &requestHeader->authenticationToken))
        return sendServiceFaultWithRequest(channel, requestHeader, responseType,
                                           requestId, UA_STATUSCODE_BADSESSIONIDINVALID);

    /* Has the session timed out? */
    if(session && session->validTill < UA_DateTime_nowMonotonic())
        return sendServiceFaultWithRequest(channel, requestHeader, responseType,
                                           requestId, UA_STATUSCODE_BADSESSIONIDINVALID);

    /* Session lifecycle service. The session pointer can still be NULL. */
    if(requestType == &UA_TYPES[UA_TYPES_CREATESESSIONREQUEST] ||
       requestType == &UA_TYPES[UA_TYPES_ACTIVATESESSIONREQUEST] ||
       requestType == &UA_TYPES[UA_TYPES_CLOSESESSIONREQUEST]) {
        UA_LOCK(server->serviceMutex);
        ((UA_SessionService)(uintptr_t)service)(server, channel, session,
                                                requestHeader, responseHeader);
        UA_UNLOCK(server->serviceMutex);
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        /* Store the authentication token so we can help fuzzing by setting
         * these values in the next request automatically */
        if(requestType == &UA_TYPES[UA_TYPES_CREATESESSIONREQUEST]) {
            UA_CreateSessionResponse *res = (UA_CreateSessionResponse *)responseHeader;
            UA_NodeId_copy(&res->authenticationToken, &unsafe_fuzz_authenticationToken);
        }
#endif
        return sendResponse(channel, requestId, requestHeader->requestHandle,
                            responseHeader, responseType);
    }

    /* Set an anonymous, inactive session for services that need no session */
    UA_Session anonymousSession;
    if(!session) {
        if(sessionRequired) {
#ifdef UA_ENABLE_TYPEDESCRIPTION
            UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                                   "%s refused without a valid session",
                                   requestType->typeName);
#else
            UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                                   "Service %i refused without a valid session",
                                   requestType->binaryEncodingId);
#endif
            return sendServiceFaultWithRequest(channel, requestHeader, responseType,
                                               requestId, UA_STATUSCODE_BADSESSIONIDINVALID);
        }

        UA_Session_init(&anonymousSession);
        anonymousSession.sessionId = UA_NODEID_GUID(0, UA_GUID_NULL);
        anonymousSession.header.channel = channel;
        session = &anonymousSession;
    }

    UA_assert(session != NULL);

    /* Trying to use a non-activated session? */
    if(sessionRequired && !session->activated) {
#ifdef UA_ENABLE_TYPEDESCRIPTION
        UA_LOG_WARNING_SESSION(&server->config.logger, session,
                               "%s refused on a non-activated session",
                               requestType->typeName);
#else
        UA_LOG_WARNING_SESSION(&server->config.logger, session,
                               "Service %i refused on a non-activated session",
                               requestType->binaryEncodingId);
#endif
        UA_LOCK(server->serviceMutex);
        UA_Server_removeSessionByToken(server, &session->header.authenticationToken);
        UA_UNLOCK(server->serviceMutex);
        return sendServiceFaultWithRequest(channel, requestHeader, responseType,
                                           requestId, UA_STATUSCODE_BADSESSIONNOTACTIVATED);
    }

    /* Update the session lifetime */
    UA_Session_updateLifetime(session);

#ifdef UA_ENABLE_SUBSCRIPTIONS
    /* The publish request is not answered immediately */
    if(requestType == &UA_TYPES[UA_TYPES_PUBLISHREQUEST]) {
        UA_LOCK(server->serviceMutex);
        Service_Publish(server, session, (const UA_PublishRequest*)requestHeader, requestId);
        UA_UNLOCK(server->serviceMutex);
        return UA_STATUSCODE_GOOD;
    }
#endif

#if UA_MULTITHREADING >= 100
    /* The call request might not be answered immediately */
    if(requestType == &UA_TYPES[UA_TYPES_CALLREQUEST]) {
        UA_Boolean finished = true;
        UA_LOCK(server->serviceMutex);
        Service_CallAsync(server, session, requestId, (const UA_CallRequest*)requestHeader,
                          (UA_CallResponse*)responseHeader, &finished);
        UA_UNLOCK(server->serviceMutex);

        /* Async method calls remain. Don't send a response now */
        if(!finished)
            return UA_STATUSCODE_GOOD;

        /* We are done here */
        return sendResponse(channel, requestId, requestHeader->requestHandle,
                            responseHeader, responseType);
    }
#endif

    /* Dispatch the synchronous service call and send the response */
    UA_LOCK(server->serviceMutex);
    service(server, session, requestHeader, responseHeader);
    UA_UNLOCK(server->serviceMutex);
    return sendResponse(channel, requestId, requestHeader->requestHandle,
                        responseHeader, responseType);
}

static UA_StatusCode
processMSG(UA_Server *server, UA_SecureChannel *channel,
           UA_UInt32 requestId, const UA_ByteString *msg) {
    /* Decode the nodeid */
    size_t offset = 0;
    UA_NodeId requestTypeId;
    UA_StatusCode retval = UA_NodeId_decodeBinary(msg, &offset, &requestTypeId);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
    if(requestTypeId.namespaceIndex != 0 ||
       requestTypeId.identifierType != UA_NODEIDTYPE_NUMERIC)
        UA_NodeId_clear(&requestTypeId); /* leads to badserviceunsupported */

    size_t requestPos = offset; /* Store the offset (for sendServiceFault) */

    /* Get the service pointers */
    UA_Service service = NULL;
    UA_Boolean sessionRequired = true;
    const UA_DataType *requestType = NULL;
    const UA_DataType *responseType = NULL;
    getServicePointers(requestTypeId.identifier.numeric, &requestType,
                       &responseType, &service, &sessionRequired);
    if(!requestType) {
        if(requestTypeId.identifier.numeric == 787) {
            UA_LOG_INFO_CHANNEL(&server->config.logger, channel,
                                "Client requested a subscription, " \
                                "but those are not enabled in the build");
        } else {
            UA_LOG_INFO_CHANNEL(&server->config.logger, channel,
                                "Unknown request with type identifier %i",
                                requestTypeId.identifier.numeric);
        }
        return sendServiceFault(channel, msg, requestPos, &UA_TYPES[UA_TYPES_SERVICEFAULT],
                                requestId, UA_STATUSCODE_BADSERVICEUNSUPPORTED);
    }
    UA_assert(responseType);

    /* Decode the request */
    UA_STACKARRAY(UA_Byte, request, requestType->memSize);
    retval = UA_decodeBinary(msg, &offset, request, requestType, server->config.customDataTypes);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_DEBUG_CHANNEL(&server->config.logger, channel,
                             "Could not decode the request with StatusCode %s",
                             UA_StatusCode_name(retval));
        return sendServiceFault(channel, msg, requestPos, responseType, requestId, retval);
    }

    /* Check timestamp in the request header */
    UA_RequestHeader *requestHeader = (UA_RequestHeader*)request;
    if(requestHeader->timestamp == 0) {
        if(server->config.verifyRequestTimestamp <= UA_RULEHANDLING_WARN) {
            UA_LOG_WARNING_CHANNEL(&server->config.logger, channel,
                                   "The server sends no timestamp in the request header. "
                                   "See the 'verifyRequestTimestamp' setting.");
            if(server->config.verifyRequestTimestamp <= UA_RULEHANDLING_ABORT) {
                retval = sendServiceFaultWithRequest(channel, requestHeader, responseType,
                                                     requestId, UA_STATUSCODE_BADINVALIDTIMESTAMP);
                UA_clear(request, requestType);
                return retval;
            }
        }
    }

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    /* Set the authenticationToken from the create session request to help
     * fuzzing cover more lines */
    UA_NodeId_clear(&requestHeader->authenticationToken);
    if(!UA_NodeId_isNull(&unsafe_fuzz_authenticationToken))
        UA_NodeId_copy(&unsafe_fuzz_authenticationToken, &requestHeader->authenticationToken);
#endif

    /* Prepare the respone */
    UA_STACKARRAY(UA_Byte, response, responseType->memSize);
    UA_ResponseHeader *responseHeader = (UA_ResponseHeader*)response;
    UA_init(response, responseType);

    /* Continue with the decoded Request */
    retval = processMSGDecoded(server, channel, requestId, service, requestHeader, requestType,
                               responseHeader, responseType, sessionRequired);

    /* Clean up */
    UA_clear(request, requestType);
    UA_clear(responseHeader, responseType);
    return retval;
}

/* Takes decoded messages starting at the nodeid of the content type. */
static void
processSecureChannelMessage(void *application, UA_SecureChannel *channel,
                            UA_MessageType messagetype, UA_UInt32 requestId,
                            UA_ByteString *message) {
    UA_Server *server = (UA_Server*)application;

    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    switch(messagetype) {
    case UA_MESSAGETYPE_HEL:
        UA_LOG_TRACE_CHANNEL(&server->config.logger, channel, "Process a HEL message");
        retval = processHEL(server, channel, message);
        break;
    case UA_MESSAGETYPE_OPN:
        UA_LOG_TRACE_CHANNEL(&server->config.logger, channel, "Process an OPN message");
        retval = decryptProcessOPN(server, channel, message);
        break;
    case UA_MESSAGETYPE_MSG:
        UA_LOG_TRACE_CHANNEL(&server->config.logger, channel, "Process a MSG");
        retval = processMSG(server, channel, requestId, message);
        break;
    case UA_MESSAGETYPE_CLO:
        UA_LOG_TRACE_CHANNEL(&server->config.logger, channel, "Process a CLO");
        Service_CloseSecureChannel(server, channel);
        break;
    default:
        UA_LOG_TRACE_CHANNEL(&server->config.logger, channel, "Invalid message type");
        retval = UA_STATUSCODE_BADTCPMESSAGETYPEINVALID;
        break;
    }
    if(retval != UA_STATUSCODE_GOOD) {
        if(channel->connection) {
            UA_LOG_INFO_CHANNEL(&server->config.logger, channel,
                                "Processing the message failed with StatusCode %s. "
                                "Closing the channel.", UA_StatusCode_name(retval));
            UA_TcpErrorMessage errMsg;
            UA_TcpErrorMessage_init(&errMsg);
            errMsg.error = retval;
            UA_Connection_sendError(channel->connection, &errMsg);
            Service_CloseSecureChannel(server, channel);
        } else {
            UA_LOG_INFO_CHANNEL(&server->config.logger, channel,
                                "Processing the message failed. Channel already closed "
                                "with StatusCode %s. ", UA_StatusCode_name(retval));
        }
    }
}

void
UA_Server_processBinaryMessage(UA_Server *server, UA_Connection *connection,
                               UA_ByteString *message) {
    UA_LOG_TRACE(&server->config.logger, UA_LOGCATEGORY_NETWORK,
                 "Connection %i | Received a packet.", (int)(connection->sockfd));

    UA_TcpErrorMessage error;
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    UA_SecureChannel *channel = connection->channel;

    /* Add a SecureChannel to a new connection */
    if(!channel) {
        retval = UA_SecureChannelManager_create(&server->secureChannelManager, connection);
        if(retval != UA_STATUSCODE_GOOD)
            goto error;
        channel = connection->channel;
        UA_assert(channel);
    }

#ifdef UA_DEBUG_DUMP_PKGS
    UA_dump_hex_pkg(message->data, message->length);
#endif
#ifdef UA_DEBUG_DUMP_PKGS_FILE
    UA_debug_dumpCompleteChunk(server, channel->connection, message);
#endif

    retval = UA_SecureChannel_processPacket(channel, server, processSecureChannelMessage, message);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_INFO(&server->config.logger, UA_LOGCATEGORY_NETWORK,
                    "Connection %i | Processing the message failed with error %s",
                    (int)(connection->sockfd), UA_StatusCode_name(retval));
        goto error;
    }

    return;

 error:
    /* Send an ERR message and close the connection */
    error.error = retval;
    error.reason = UA_STRING_NULL;
    UA_Connection_sendError(connection, &error);
    connection->close(connection);
}

#if UA_MULTITHREADING >= 200
static void
deleteConnection(UA_Server *server, UA_Connection *connection) {
    connection->free(connection);
}
#endif

void
UA_Server_removeConnection(UA_Server *server, UA_Connection *connection) {
    UA_Connection_detachSecureChannel(connection);
#if UA_MULTITHREADING >= 200
    UA_DelayedCallback *dc = (UA_DelayedCallback*)UA_malloc(sizeof(UA_DelayedCallback));
    if(!dc)
        return; /* Malloc cannot fail on OS's that support multithreading. They
                 * rather kill the process. */
    dc->callback = (UA_ApplicationCallback)deleteConnection;
    dc->application = server;
    dc->data = connection;
    UA_WorkQueue_enqueueDelayed(&server->workQueue, dc);
#else
    connection->free(connection);
#endif
}
