/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2024, Confluent Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "test.h"

#include "../src/rdkafka_proto.h"

#include <stdarg.h>


/**
 * @name Mock tests specific of the KIP-848 group consumer protocol
 */


static int allowed_error;
static int rebalance_cnt;
static rd_kafka_resp_err_t rebalance_exp_event;
static rd_bool_t rebalance_exp_lost = rd_false;

/**
 * @brief Decide what error_cb's will cause the test to fail.
 */
static int
error_is_fatal_cb(rd_kafka_t *rk, rd_kafka_resp_err_t err, const char *reason) {
        if (err == allowed_error ||
            /* If transport errors are allowed then it is likely
             * that we'll also see ALL_BROKERS_DOWN. */
            (allowed_error == RD_KAFKA_RESP_ERR__TRANSPORT &&
             err == RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN)) {
                TEST_SAY("Ignoring allowed error: %s: %s\n",
                         rd_kafka_err2name(err), reason);
                return 0;
        }
        return 1;
}

/**
 * @brief Rebalance callback saving number of calls and verifying expected
 *        event.
 */
static void rebalance_cb(rd_kafka_t *rk,
                         rd_kafka_resp_err_t err,
                         rd_kafka_topic_partition_list_t *parts,
                         void *opaque) {

        rebalance_cnt++;
        TEST_SAY("Rebalance #%d: %s: %d partition(s)\n", rebalance_cnt,
                 rd_kafka_err2name(err), parts->cnt);

        TEST_ASSERT(
            err == rebalance_exp_event, "Expected rebalance event %s, not %s",
            rd_kafka_err2name(rebalance_exp_event), rd_kafka_err2name(err));

        if (rebalance_exp_lost) {
                TEST_ASSERT(rd_kafka_assignment_lost(rk),
                            "Expected partitions lost");
                TEST_SAY("Partitions were lost\n");
        }

        if (err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS) {
                test_consumer_assign("assign", rk, parts);
        } else {
                test_consumer_unassign("unassign", rk);
        }

        /* Make sure only one rebalance callback is served per poll()
         * so that expect_rebalance() returns to the test logic on each
         * rebalance. */
        rd_kafka_yield(rk);
}

/**
 * @brief Wait \p tmout_ms has passed and at least \p num heartbeats
 *        have been received by the mock cluster.
 *
 * @return Number of heartbeats received.
 */
static int wait_all_heartbeats_done(rd_kafka_mock_cluster_t *mcluster,
                                    int num,
                                    int tmout_ms) {
        size_t i;
        rd_kafka_mock_request_t **requests;
        size_t request_cnt;
        int current_heartbeats = 0;

        rd_usleep(tmout_ms * 1000, 0);

        while (current_heartbeats < num) {
                requests = rd_kafka_mock_get_requests(mcluster, &request_cnt);
                current_heartbeats = 0;
                for (i = 0; i < request_cnt; i++) {
                        if (rd_kafka_mock_request_api_key(requests[i]) ==
                            RD_KAFKAP_ConsumerGroupHeartbeat)
                                current_heartbeats++;
                }
                rd_kafka_mock_request_destroy_array(requests, request_cnt);
                rd_usleep(100 * 1000, 0);
        }
        return current_heartbeats;
}

static rd_kafka_t *create_consumer(const char *bootstraps, const char *topic) {
        rd_kafka_conf_t *conf;
        test_conf_init(&conf, NULL, 0);
        test_conf_set(conf, "bootstrap.servers", bootstraps);
        test_conf_set(conf, "group.protocol", "consumer");
        test_conf_set(conf, "auto.offset.reset", "earliest");
        test_conf_set(conf, "debug", "all");
        return test_create_consumer(topic, rebalance_cb, conf, NULL);
}

/**
 * @brief Test heartbeat behavior with fatal errors,
 *        ensuring:
 *        - a fatal error is received on poll and consumer close
 *        - no rebalance cb is called
 *        - no final leave group heartbeat is sent
 *
 * @param err The error code to test.
 * @param variation See test main.
 */
static void
do_test_consumer_group_heartbeat_fatal_error(rd_kafka_resp_err_t err,
                                             int variation) {
        rd_kafka_mock_cluster_t *mcluster;
        const char *bootstraps;
        rd_kafka_topic_partition_list_t *subscription;
        rd_kafka_t *c;
        rd_kafka_message_t *rkmessage;
        int expected_heartbeats, found_heartbeats, expected_rebalance_cnt,
            test_total_time_ms = 0;
        test_timing_t timing;
        rebalance_cnt       = 0;
        rebalance_exp_lost  = rd_false;
        rebalance_exp_event = RD_KAFKA_RESP_ERR_NO_ERROR;
        const char *topic   = test_mk_topic_name(__FUNCTION__, 0);

        SUB_TEST_QUICK("%s, variation %d", rd_kafka_err2name(err), variation);

        mcluster = test_mock_cluster_new(1, &bootstraps);
        rd_kafka_mock_set_default_heartbeat_interval(mcluster, 500);
        rd_kafka_mock_topic_create(mcluster, topic, 1, 1);

        TIMING_START(&timing, "consumer_group_heartbeat_fatal_error");

        if (variation == 1) {
                /* First HB returns assignment */
                rd_kafka_mock_broker_push_request_error_rtts(
                    mcluster, 1, RD_KAFKAP_ConsumerGroupHeartbeat, 1,
                    RD_KAFKA_RESP_ERR_NO_ERROR, 0);
        }

        rd_kafka_mock_broker_push_request_error_rtts(
            mcluster, 1, RD_KAFKAP_ConsumerGroupHeartbeat, 1, err, 0);

        c = create_consumer(bootstraps, topic);

        /* Subscribe to the input topic */
        subscription = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(subscription, topic,
                                          /* The partition is ignored in
                                           * rd_kafka_subscribe() */
                                          RD_KAFKA_PARTITION_UA);

        rd_kafka_mock_start_request_tracking(mcluster);
        TEST_CALL_ERR__(rd_kafka_subscribe(c, subscription));
        rd_kafka_topic_partition_list_destroy(subscription);

        expected_heartbeats = 1;
        if (variation == 1)
                expected_heartbeats++;
        test_total_time_ms += 750;
        TEST_ASSERT((found_heartbeats =
                         wait_all_heartbeats_done(mcluster, expected_heartbeats,
                                                  750)) == expected_heartbeats,
                    "Expected %d heartbeats, got %d", expected_heartbeats,
                    found_heartbeats);

        rd_kafka_mock_clear_requests(mcluster);

        expected_rebalance_cnt = 0;
        if (variation == 1) {
                expected_rebalance_cnt++;
                rebalance_exp_event = RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS;

                /* Trigger rebalance cb */
                rkmessage = rd_kafka_consumer_poll(c, 500);
                TEST_ASSERT(!rkmessage, "No message should be returned");
        }

        /* Consume from c, a fatal error is returned */
        rkmessage = rd_kafka_consumer_poll(c, 500);
        TEST_ASSERT(rkmessage != NULL, "An error message should be returned");
        TEST_ASSERT(rkmessage->err == RD_KAFKA_RESP_ERR__FATAL,
                    "Expected a _FATAL error, got %s",
                    rd_kafka_err2name(rkmessage->err));
        rd_kafka_message_destroy(rkmessage);

        TEST_ASSERT(rebalance_cnt == expected_rebalance_cnt,
                    "Expected %d rebalance events, got %d",
                    expected_rebalance_cnt, rebalance_cnt);

        expected_rebalance_cnt = 0;
        if (variation == 1) {
                expected_rebalance_cnt++;
                rebalance_exp_event = RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS;
                rebalance_exp_lost  = rd_true;
        }

        /* Close c, a fatal error is returned */
        TEST_ASSERT(rd_kafka_consumer_close(c) == RD_KAFKA_RESP_ERR__FATAL,
                    "Expected a _FATAL error, got %s", rd_kafka_err2name(err));

        TEST_ASSERT(rebalance_cnt == expected_rebalance_cnt,
                    "Expected %d rebalance events, got %d",
                    expected_rebalance_cnt, rebalance_cnt);

        /* After closing the consumer, no heartbeat should have been sent */
        test_total_time_ms += 250;
        TEST_ASSERT((found_heartbeats =
                         wait_all_heartbeats_done(mcluster, 0, 250)) == 0,
                    "Expected no leave group heartbeat, got %d",
                    found_heartbeats);

        rd_kafka_mock_stop_request_tracking(mcluster);
        rd_kafka_destroy(c);
        test_mock_cluster_destroy(mcluster);

        /* test_total_time_ms - 500 ms < timing < test_total_time_ms + 500 ms */
        TIMING_ASSERT(&timing, test_total_time_ms - 500,
                      test_total_time_ms + 500);
        SUB_TEST_PASS();
}

/**
 * @brief Test all kind of fatal errors in a ConsumerGroupHeartbeat call.
 */
static void do_test_consumer_group_heartbeat_fatal_errors(void) {
        rd_kafka_resp_err_t fatal_errors[] = {
            RD_KAFKA_RESP_ERR_INVALID_REQUEST,
            RD_KAFKA_RESP_ERR_GROUP_MAX_SIZE_REACHED,
            RD_KAFKA_RESP_ERR_UNSUPPORTED_ASSIGNOR,
            RD_KAFKA_RESP_ERR_UNSUPPORTED_VERSION,
            RD_KAFKA_RESP_ERR_UNRELEASED_INSTANCE_ID,
            RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED};
        size_t i;
        for (i = 0; i < RD_ARRAY_SIZE(fatal_errors); i++) {
                do_test_consumer_group_heartbeat_fatal_error(fatal_errors[i],
                                                             0);
                do_test_consumer_group_heartbeat_fatal_error(fatal_errors[i],
                                                             1);
        }
}

/**
 * @brief Test heartbeat behavior with retriable errors,
 *        ensuring:
 *        - no error is received on poll and consumer close
 *        - rebalance cb is called to assign and revoke
 *        - final leave group heartbeat is sent
 *
 * @param err The error code to test.
 * @param variation See test main.
 */
static void
do_test_consumer_group_heartbeat_retriable_error(rd_kafka_resp_err_t err,
                                                 int variation) {
        rd_kafka_mock_cluster_t *mcluster;
        const char *bootstraps;
        rd_kafka_topic_partition_list_t *subscription;
        rd_kafka_t *c;
        int expected_heartbeats, found_heartbeats, observation_window_ms,
            test_total_time_ms = 0;
        test_timing_t timing;
        const char *topic      = test_mk_topic_name(__FUNCTION__, 0);
        test_curr->is_fatal_cb = error_is_fatal_cb;
        rebalance_cnt          = 0;
        rebalance_exp_lost     = rd_false;
        allowed_error          = RD_KAFKA_RESP_ERR__TRANSPORT;

        SUB_TEST_QUICK("%s, variation %d", rd_kafka_err2name(err), variation);


        mcluster = test_mock_cluster_new(1, &bootstraps);
        rd_kafka_mock_set_default_heartbeat_interval(mcluster, 500);
        rd_kafka_mock_topic_create(mcluster, topic, 1, 1);

        c = create_consumer(bootstraps, topic);

        TIMING_START(&timing, "consumer_group_heartbeat_retriable_error");

        if (variation == 1) {
                /* First HB returns assignment */
                rd_kafka_mock_broker_push_request_error_rtts(
                    mcluster, 1, RD_KAFKAP_ConsumerGroupHeartbeat, 1,
                    RD_KAFKA_RESP_ERR_NO_ERROR, 0);
        }

        rd_kafka_mock_broker_push_request_error_rtts(
            mcluster, 1, RD_KAFKAP_ConsumerGroupHeartbeat, 1, err, 0);

        /* Subscribe to the input topic */
        subscription = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(subscription, topic,
                                          /* The partition is ignored in
                                           * rd_kafka_subscribe() */
                                          RD_KAFKA_PARTITION_UA);

        rd_kafka_mock_start_request_tracking(mcluster);
        TEST_CALL_ERR__(rd_kafka_subscribe(c, subscription));
        rd_kafka_topic_partition_list_destroy(subscription);

        expected_heartbeats = 2;
        /* Time for first HB and retry */
        observation_window_ms = 250;
        if (variation == 1) {
                /* wait 1 HB interval more */
                observation_window_ms += 750;
                expected_heartbeats++;
        }
        test_total_time_ms += observation_window_ms;
        TEST_ASSERT((found_heartbeats = wait_all_heartbeats_done(
                         mcluster, expected_heartbeats,
                         observation_window_ms)) == expected_heartbeats,
                    "Expected %d heartbeats, got %d", expected_heartbeats,
                    found_heartbeats);

        rebalance_exp_event = RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS;

        /* Consume from c, no message is returned */
        test_total_time_ms += 250;
        test_consumer_poll_no_msgs("after heartbeat", c, 0, 250);

        TEST_ASSERT(rebalance_cnt > 0, "Expected > 0 rebalance events, got %d",
                    rebalance_cnt);

        rebalance_exp_event = RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS;

        rd_kafka_mock_clear_requests(mcluster);
        rebalance_cnt = 0;
        /* Close c without errors */
        TEST_ASSERT(rd_kafka_consumer_close(c) == RD_KAFKA_RESP_ERR_NO_ERROR,
                    "Expected NO_ERROR, got %s", rd_kafka_err2name(err));
        TEST_ASSERT(rebalance_cnt > 0, "Expected > 0 rebalance events, got %d",
                    rebalance_cnt);
        rebalance_exp_event = RD_KAFKA_RESP_ERR_NO_ERROR;

        test_total_time_ms += 250;
        /* After closing the consumer, leave group heartbeat should have been
         * sent */
        TEST_ASSERT((found_heartbeats =
                         wait_all_heartbeats_done(mcluster, 1, 250)) == 1,
                    "Expected 1 leave group heartbeat, got %d",
                    found_heartbeats);

        rd_kafka_mock_stop_request_tracking(mcluster);
        rd_kafka_destroy(c);
        test_mock_cluster_destroy(mcluster);

        /* test_total_time_ms - 500  ms < timing < test_total_time_ms + 500 ms
         */
        TIMING_ASSERT(&timing, test_total_time_ms - 500,
                      test_total_time_ms + 500);

        test_curr->is_fatal_cb = NULL;
        allowed_error          = RD_KAFKA_RESP_ERR_NO_ERROR;

        SUB_TEST_PASS();
}

/**
 * @brief Test all kind of retriable errors in a ConsumerGroupHeartbeat call.
 */
static void do_test_consumer_group_heartbeat_retriable_errors(void) {
        rd_kafka_resp_err_t retriable_errors[] = {
            RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS,
            RD_KAFKA_RESP_ERR__SSL, RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE};
        size_t i;
        for (i = 0; i < RD_ARRAY_SIZE(retriable_errors); i++) {
                do_test_consumer_group_heartbeat_retriable_error(
                    retriable_errors[i], 0);
                do_test_consumer_group_heartbeat_retriable_error(
                    retriable_errors[i], 1);
        }
}

/**
 * @brief Test heartbeat behavior with consumer fenced errors,
 *        ensuring:
 *        - no error is received on poll and consumer close
 *        - rebalance callbacks are called, with partitions lost when
 *          necessary
 *        - a final leave group heartbeat is sent
 *
 * @param err The error code to test.
 * @param variation See test main.
 */
static void
do_test_consumer_group_heartbeat_fenced_error(rd_kafka_resp_err_t err,
                                              int variation) {
        rd_kafka_mock_cluster_t *mcluster;
        const char *bootstraps;
        rd_kafka_topic_partition_list_t *subscription;
        rd_kafka_t *c;
        rd_kafka_message_t *rkmessage;
        int expected_heartbeats, found_heartbeats, expected_rebalance_cnt,
            test_total_time_ms = 0;
        test_timing_t timing;
        rebalance_cnt       = 0;
        rebalance_exp_lost  = rd_false;
        rebalance_exp_event = RD_KAFKA_RESP_ERR_NO_ERROR;
        const char *topic   = test_mk_topic_name(__FUNCTION__, 0);

        SUB_TEST_QUICK("%s, variation %d", rd_kafka_err2name(err), variation);

        mcluster = test_mock_cluster_new(1, &bootstraps);
        rd_kafka_mock_set_default_heartbeat_interval(mcluster, 500);
        rd_kafka_mock_topic_create(mcluster, topic, 1, 1);

        TIMING_START(&timing, "consumer_group_heartbeat_fenced_error");

        if (variation == 1) {
                /* First HB returns assignment */
                rd_kafka_mock_broker_push_request_error_rtts(
                    mcluster, 1, RD_KAFKAP_ConsumerGroupHeartbeat, 1,
                    RD_KAFKA_RESP_ERR_NO_ERROR, 0);
        }

        rd_kafka_mock_broker_push_request_error_rtts(
            mcluster, 1, RD_KAFKAP_ConsumerGroupHeartbeat, 1, err, 0);

        c = create_consumer(bootstraps, topic);

        /* Subscribe to the input topic */
        subscription = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(subscription, topic,
                                          /* The partition is ignored in
                                           * rd_kafka_subscribe() */
                                          RD_KAFKA_PARTITION_UA);

        rd_kafka_mock_start_request_tracking(mcluster);
        TEST_CALL_ERR__(rd_kafka_subscribe(c, subscription));
        rd_kafka_topic_partition_list_destroy(subscription);

        /*First HB is fenced and second receives assignment*/
        expected_heartbeats = 2;
        if (variation == 1)
                /*First HB receives assignment*/
                expected_heartbeats = 1;

        test_total_time_ms += 250;
        TEST_ASSERT((found_heartbeats =
                         wait_all_heartbeats_done(mcluster, expected_heartbeats,
                                                  250)) == expected_heartbeats,
                    "Expected %d heartbeats, got %d", expected_heartbeats,
                    found_heartbeats);

        expected_rebalance_cnt = 0;
        /* variation 0: Second HB assigned */
        if (variation == 1) {
                expected_rebalance_cnt++;
                rebalance_exp_event = RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS;

                /* First HB assigned */
                rkmessage = rd_kafka_consumer_poll(c, 500);
                TEST_ASSERT(!rkmessage, "No message should be returned");

                expected_rebalance_cnt++;
                rebalance_exp_event = RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS;
                rebalance_exp_lost  = rd_true;

                /* Second HB loses partitions */
                rkmessage = rd_kafka_consumer_poll(c, 500);
                TEST_ASSERT(!rkmessage, "No message should be returned");

                /* Third HB assigns again */
        }

        expected_rebalance_cnt++;
        rebalance_exp_event = RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS;
        rebalance_exp_lost  = rd_false;

        /* Consume from c, partitions are lost if assigned */
        rkmessage = rd_kafka_consumer_poll(c, 500);
        TEST_ASSERT(!rkmessage, "No message should be returned");

        TEST_ASSERT(rebalance_cnt == expected_rebalance_cnt,
                    "Expected %d rebalance events, got %d",
                    expected_rebalance_cnt, rebalance_cnt);


        test_total_time_ms += 250;
        if (variation == 0) {
                /*Ack for assignment HB*/
                expected_heartbeats++;
        } else if (variation == 1) {
                /* First HB is fenced
                 * Second receives assignment
                 * Third acks assignment */
                expected_heartbeats += 3;
        }
        TEST_ASSERT((found_heartbeats =
                         wait_all_heartbeats_done(mcluster, expected_heartbeats,
                                                  250)) == expected_heartbeats,
                    "Expected %d heartbeats, got %d", expected_heartbeats,
                    found_heartbeats);

        expected_rebalance_cnt++;
        rebalance_exp_event = RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS;

        rd_kafka_mock_clear_requests(mcluster);
        /* Close c, no error is returned */
        TEST_CALL_ERR__(rd_kafka_consumer_close(c));

        TEST_ASSERT(rebalance_cnt == expected_rebalance_cnt,
                    "Expected %d rebalance events, got %d",
                    expected_rebalance_cnt, rebalance_cnt);

        /* After closing the consumer, 1 heartbeat should been sent */
        test_total_time_ms += 250;
        TEST_ASSERT((found_heartbeats =
                         wait_all_heartbeats_done(mcluster, 1, 250)) == 1,
                    "Expected 1 leave group heartbeat, got %d",
                    found_heartbeats);

        rd_kafka_mock_stop_request_tracking(mcluster);
        rd_kafka_destroy(c);
        test_mock_cluster_destroy(mcluster);

        /* test_total_time_ms - 500 ms < timing < test_total_time_ms + 500 ms */
        TIMING_ASSERT(&timing, test_total_time_ms - 500,
                      test_total_time_ms + 500);
        SUB_TEST_PASS();
}

/**
 * @brief Test all kind of consumer fenced errors in a ConsumerGroupHeartbeat
 * call.
 */
static void do_test_consumer_group_heartbeat_fenced_errors(void) {
        rd_kafka_resp_err_t fenced_errors[] = {
            RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID,
            RD_KAFKA_RESP_ERR_FENCED_MEMBER_EPOCH};
        size_t i;
        for (i = 0; i < RD_ARRAY_SIZE(fenced_errors); i++) {
                do_test_consumer_group_heartbeat_fenced_error(fenced_errors[i],
                                                              0);
                do_test_consumer_group_heartbeat_fenced_error(fenced_errors[i],
                                                              1);
        }
}

int main_0146_consumer_group_consumer_mock(int argc, char **argv) {
        TEST_SKIP_MOCK_CLUSTER(0);

        /* variation 0: errors on first HB
         * variation 1: errors on second HB */

        do_test_consumer_group_heartbeat_fatal_errors();

        do_test_consumer_group_heartbeat_retriable_errors();

        do_test_consumer_group_heartbeat_fenced_errors();

        return 0;
}
