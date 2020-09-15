//receipt_handler.c

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/ioevent_loop.h"
#include "../../sf_util.h"
#include "../../sf_func.h"
#include "../../sf_nio.h"
#include "../../sf_global.h"
#include "../../sf_service.h"
#include "../../sf_proto.h"
#include "client_channel.h"
#include "receipt_handler.h"

static IdempotencyReceiptThreadContext *receipt_thread_contexts = NULL;

static int receipt_init_task(struct fast_task_info *task)
{
    task->connect_timeout = SF_G_CONNECT_TIMEOUT; //for client side
    task->network_timeout = SF_G_NETWORK_TIMEOUT;
    return 0;
}

static int receipt_recv_timeout_callback(struct fast_task_info *task)
{
    IdempotencyClientChannel *channel;

    if (SF_NIO_TASK_STAGE_FETCH(task) == SF_NIO_STAGE_CONNECT) {
        logError("file: "__FILE__", line: %d, "
                "connect to server %s:%d timeout",
                __LINE__, task->server_ip, task->port);
        return ETIMEDOUT;
    }

    channel = (IdempotencyClientChannel *)task->arg;
    if (channel->waiting_resp_qinfo.head != NULL) {
        logError("file: "__FILE__", line: %d, "
                "waiting receipt response from server %s:%d timeout",
                __LINE__, task->server_ip, task->port);
        return ETIMEDOUT;
    }

    return 0;
}

static void receipt_task_finish_cleanup(struct fast_task_info *task)
{
    IdempotencyClientChannel *channel;

    if (task->event.fd >= 0) {
        sf_task_detach_thread(task);
        close(task->event.fd);
        task->event.fd = -1;
    }

    channel = (IdempotencyClientChannel *)task->arg;

    fc_list_del_init(&channel->dlink);
    __sync_bool_compare_and_swap(&channel->established, 1, 0);
    __sync_bool_compare_and_swap(&channel->in_ioevent, 1, 0);

    logDebug("file: "__FILE__", line: %d, "
            "receipt task for server %s:%d exit",
            __LINE__, task->server_ip, task->port);
}

static int setup_channel_request(struct fast_task_info *task)
{
    IdempotencyClientChannel *channel;
    SFCommonProtoHeader *header;
    FSProtoSetupChannelReq *req;

    channel = (IdempotencyClientChannel *)task->arg;
    header = (SFCommonProtoHeader *)task->data;
    req = (FSProtoSetupChannelReq *)(header + 1);
    int2buff(__sync_add_and_fetch(&channel->id, 0), req->channel_id);
    int2buff(__sync_add_and_fetch(&channel->key, 0), req->key);

    FS_PROTO_SET_HEADER(header, FS_SERVICE_PROTO_SETUP_CHANNEL_REQ,
            sizeof(FSProtoSetupChannelReq));
    task->length = sizeof(SFCommonProtoHeader) + sizeof(FSProtoSetupChannelReq);
    return sf_send_add_event(task);
}

static int check_report_req_receipt(struct fast_task_info *task,
        int *count)
{
    IdempotencyClientChannel *channel;
    SFCommonProtoHeader *header;
    FSProtoReportReqReceiptHeader *rheader;
    FSProtoReportReqReceiptBody *rbody;
    FSProtoReportReqReceiptBody *rstart;
    IdempotencyClientReceipt *last;
    IdempotencyClientReceipt *receipt;
    char *buff_end;

    if (task->length > 0) {
        *count = 0;
        logWarning("file: "__FILE__", line: %d, "
                "server %s:%d, task length: %d != 0, skip check "
                "and report receipt request!", __LINE__,
                task->server_ip, task->port, task->length);
        return 0;
    }

    channel = (IdempotencyClientChannel *)task->arg;
    if (channel->waiting_resp_qinfo.head != NULL) {
        *count = 0;
        return 0;
    }

    fc_queue_pop_to_queue(&channel->queue, &channel->waiting_resp_qinfo);
    if (channel->waiting_resp_qinfo.head == NULL) {
        *count = 0;
        return 0;
    }

    header = (SFCommonProtoHeader *)task->data;
    rheader = (FSProtoReportReqReceiptHeader *)(header + 1);
    rbody = rstart = (FSProtoReportReqReceiptBody *)(rheader + 1);
    buff_end = task->data + task->size;
    last = NULL;
    receipt = channel->waiting_resp_qinfo.head;
    do {
        //check buffer remain space
        if (buff_end - (char *)rbody < sizeof(FSProtoReportReqReceiptBody)) {
            break;
        }

        long2buff(receipt->req_id, rbody->req_id);
        rbody++;

        last = receipt;
        receipt = receipt->next;
    } while (receipt != NULL);

    if (receipt != NULL) {  //repush to queue
        struct fc_queue_info qinfo;
        bool notify;

        qinfo.head = receipt;
        qinfo.tail = channel->waiting_resp_qinfo.tail;
        fc_queue_push_queue_to_head_ex(&channel->queue, &qinfo, &notify);

        last->next = NULL;
        channel->waiting_resp_qinfo.tail = last;
    }

    *count = rbody - rstart;
    int2buff(*count, rheader->count);
    task->length = (char *)rbody - task->data;
    int2buff(task->length - sizeof(SFCommonProtoHeader), header->body_len);
    header->cmd = FS_SERVICE_PROTO_REPORT_REQ_RECEIPT_REQ;
    return sf_send_add_event(task);
}

static inline void update_lru_chain(struct fast_task_info *task)
{
    IdempotencyReceiptThreadContext *thread_ctx;
    IdempotencyClientChannel *channel;

    thread_ctx = (IdempotencyReceiptThreadContext *)task->thread_data->arg;
    channel = (IdempotencyClientChannel *)task->arg;
    channel->last_pkg_time = g_current_time;
    fc_list_move_tail(&channel->dlink, &thread_ctx->head);
}

static int report_req_receipt_request(struct fast_task_info *task,
        const bool update_lru)
{
    int result;
    int count;

    if ((result=check_report_req_receipt(task, &count)) != 0) {
        return result;
    }

    if (count == 0) {
        result = sf_set_read_event(task);
    } else if (update_lru) {
        update_lru_chain(task);
    }

    return 0;
}

static inline int receipt_expect_body_length(struct fast_task_info *task,
        const int expect_body_len)
{
    if ((int)(task->length - sizeof(SFCommonProtoHeader)) != expect_body_len) {
        logError("file: "__FILE__", line: %d, "
                "server %s:%d, response body length: %d != %d",
                __LINE__, task->server_ip, task->port, (int)(task->length -
                    sizeof(SFCommonProtoHeader)), expect_body_len);
        return EINVAL;
    }

    return 0;
}

static int deal_setup_channel_response(struct fast_task_info *task)
{
    int result;
    IdempotencyReceiptThreadContext *thread_ctx;
    FSProtoSetupChannelResp *resp;
    IdempotencyClientChannel *channel;
    int channel_id;
    int channel_key;

    if ((result=receipt_expect_body_length(task,
                    sizeof(FSProtoSetupChannelResp))) != 0)
    {
        return result;
    }

    channel = (IdempotencyClientChannel *)task->arg;
    if (__sync_add_and_fetch(&channel->established, 0)) {
        logWarning("file: "__FILE__", line: %d, "
                "response from server %s:%d, unexpected cmd: "
                "SETUP_CHANNEL_RESP, ignore it!",
                __LINE__, task->server_ip, task->port);
        return 0;
    }

    resp = (FSProtoSetupChannelResp *)(task->data + sizeof(SFCommonProtoHeader));
    channel_id = buff2int(resp->channel_id);
    channel_key = buff2int(resp->key);
    idempotency_client_channel_set_id_key(channel, channel_id, channel_key);
    if (__sync_bool_compare_and_swap(&channel->established, 0, 1)) {
        thread_ctx = (IdempotencyReceiptThreadContext *)task->thread_data->arg;
        fc_list_add_tail(&channel->dlink, &thread_ctx->head);
    }

    PTHREAD_MUTEX_LOCK(&channel->lc_pair.lock);
    pthread_cond_broadcast(&channel->lc_pair.cond);
    PTHREAD_MUTEX_UNLOCK(&channel->lc_pair.lock);

    if (channel->waiting_resp_qinfo.head != NULL) {
        bool notify;
        fc_queue_push_queue_to_head_ex(&channel->queue,
                &channel->waiting_resp_qinfo, &notify);
        channel->waiting_resp_qinfo.head = NULL;
        channel->waiting_resp_qinfo.tail = NULL;
    }

    return 0;
}

static inline int deal_report_req_receipt_response(struct fast_task_info *task)
{
    int result;
    IdempotencyClientChannel *channel;
    IdempotencyClientReceipt *current;
    IdempotencyClientReceipt *deleted;

    if ((result=receipt_expect_body_length(task, 0)) != 0) {
        return result;
    }

    channel = (IdempotencyClientChannel *)task->arg;
    if (channel->waiting_resp_qinfo.head == NULL) {
        logWarning("file: "__FILE__", line: %d, "
                "response from server %s:%d, unexpect cmd: "
                "REPORT_REQ_RECEIPT_RESP", __LINE__,
                task->server_ip, task->port);
        return 0;
    }

    current = channel->waiting_resp_qinfo.head;
    do {
        deleted = current;
        current = current->next;

        fast_mblock_free_object(&channel->receipt_allocator, deleted);
    } while (current != NULL);

    channel->waiting_resp_qinfo.head = NULL;
    channel->waiting_resp_qinfo.tail = NULL;
    return 0;
}

static int receipt_deal_task(struct fast_task_info *task)
{
    int result;
    int stage;

    do {
        stage = SF_NIO_TASK_STAGE_FETCH(task);
        if (stage == SF_NIO_STAGE_HANDSHAKE) {
            result = setup_channel_request(task);
            break;
        } else if (stage == SF_NIO_STAGE_CONTINUE) {
            if (((IdempotencyClientChannel *)task->arg)->established) {
                result = report_req_receipt_request(task, true);
            } else {
                result = 0;  //just ignore
            }
            break;
        }

        result = buff2short(((SFCommonProtoHeader *)task->data)->status);
        if (result != 0) {
            int msg_len;
            char *message;

            msg_len = task->length - sizeof(SFCommonProtoHeader);
            message = task->data + sizeof(SFCommonProtoHeader);
            logError("file: "__FILE__", line: %d, "
                    "response from server %s:%d, cmd: %d (%s), "
                    "status: %d, error info: %.*s",
                    __LINE__, task->server_ip, task->port,
                    ((SFCommonProtoHeader *)task->data)->cmd,
                    sf_get_cmd_caption(((SFCommonProtoHeader *)task->data)->cmd),
                    result, msg_len, message);
            break;
        }

        switch (((SFCommonProtoHeader *)task->data)->cmd) {
            case FS_SERVICE_PROTO_SETUP_CHANNEL_RESP:
                result = deal_setup_channel_response(task);
                break;
            case FS_SERVICE_PROTO_REPORT_REQ_RECEIPT_RESP:
                result = deal_report_req_receipt_response(task);
                break;
            default:
                logError("file: "__FILE__", line: %d, "
                        "response from server %s:%d, unexpect cmd: %d (%s)",
                        __LINE__, task->server_ip, task->port,
                        ((SFCommonProtoHeader *)task->data)->cmd,
                        sf_get_cmd_caption(((SFCommonProtoHeader *)task->data)->cmd));
                result = EINVAL;
                break;
        }

        if (result == 0) {
            update_lru_chain(task);
            task->offset = task->length = 0;
            result = report_req_receipt_request(task, false);
        }
    } while (0);

    return result > 0 ? -1 * result : result;
}

static int receipt_thread_loop_callback(struct nio_thread_data *thread_data)
{
    IdempotencyClientChannel *channel;
    IdempotencyClientChannel *tmp;
    IdempotencyReceiptThreadContext *thread_ctx;

    thread_ctx = (IdempotencyReceiptThreadContext *)thread_data->arg;
    fc_list_for_each_entry_safe(channel, tmp, &thread_ctx->head, dlink) {
        //check heartbeat
        //channel->task
    }

    return 0;
}

static void *receipt_alloc_thread_extra_data(const int thread_index)
{
    IdempotencyReceiptThreadContext *ctx;

    ctx = receipt_thread_contexts + thread_index;
    FC_INIT_LIST_HEAD(&ctx->head);
    return ctx;
}

int receipt_handler_init()
{
    receipt_thread_contexts = (IdempotencyReceiptThreadContext *)fc_malloc(
            sizeof(IdempotencyReceiptThreadContext) * SF_G_WORK_THREADS);
    if (receipt_thread_contexts == NULL) {
        return ENOMEM;
    }

    return sf_service_init_ex2(&g_sf_context,
            receipt_alloc_thread_extra_data, receipt_thread_loop_callback,
            NULL, sf_proto_set_body_length, receipt_deal_task,
            receipt_task_finish_cleanup, receipt_recv_timeout_callback,
            1000, sizeof(SFCommonProtoHeader), 0, receipt_init_task);
}

int receipt_handler_destroy()
{
    return 0;
}
