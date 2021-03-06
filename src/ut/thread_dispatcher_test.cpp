/**
 * @file thread_dispatcher_test.cpp UT for classes defined in
 *       thread_dispatcher.cpp
 *
 * Copyright (C) Metaswitch Networks 2017
 * If license terms are provided to you in a COPYING file in the root directory
 * of the source code repository by which you are accessing this code, then
 * the license outlined in that COPYING file applies to your use.
 * Otherwise no rights are granted except for those provided to you by
 * Metaswitch Networks in a separate written agreement.
 */

#include "gtest/gtest.h"
#include "test_interposer.hpp"
#include "testingcommon.h"
#include "mockloadmonitor.hpp"
#include "mock_pjsip_module.h"
#include "siptest.hpp"
#include "stack.h"

#include "thread_dispatcher.h"

using ::testing::Return;
using ::testing::StrictMock;
using ::testing::_;
using ::testing::ResultOf;
using ::testing::Expectation;
using ::testing::InvokeWithoutArgs;

// Should be at least 5 to avoid causing problems with some of the UTs
static const int REQUEST_ON_QUEUE_TIMEOUT_MS = 10;

class MockCallback : public PJUtils::Callback
{
  MOCK_METHOD0(run, void());
  MOCK_METHOD0(destruct, void());
  virtual ~MockCallback() { destruct(); }
};

class ThreadDispatcherTest : public SipTest
{
public:

  ThreadDispatcherTest()
  {
    mod_mock = new StrictMock<MockPJSipModule>(stack_data.endpt,
                                               "test-module",
                                               PJSIP_MOD_PRIORITY_TRANSPORT_LAYER);

    init_thread_dispatcher(1,
                           NULL,
                           NULL,
                           NULL,
                           &load_monitor,
                           NULL,
                           REQUEST_ON_QUEUE_TIMEOUT_MS);
    mod_thread_dispatcher = get_mod_thread_dispatcher();

    cwtest_completely_control_time();
  }

  static void SetUpTestCase()
  {
    SipTest::SetUpTestCase();

  }

  virtual void inject_msg_thread(std::string msg)
  {
    TRC_DEBUG("Injecting message:\n%s", msg.c_str());
    inject_msg_direct(msg, mod_thread_dispatcher);
  }

  // Returns a function which takes an rx_data and returns true if its call ID
  // matches the string id_str, and false otherwise.
  static std::function<bool(pjsip_rx_data*)> rx_call_id_matches(std::string id_str)
  {
    return [id_str](pjsip_rx_data* rdata) -> bool
      {
        return rx_call_id_equals_value(rdata, id_str);
      };
  }

  // Returns true if the call ID of the given rdata matches the string id_str,
  // and false otherwise.
  static bool rx_call_id_equals_value(pjsip_rx_data* rdata, std::string id_str)
  {
    std::string rdata_id_str(rdata->msg_info.cid->id.ptr,
                             rdata->msg_info.cid->id.slen);
    return (rdata_id_str == id_str);
  }

  // Get the status code of a tx_data.
  static int get_tx_status_code(pjsip_tx_data* tdata)
  {
    return tdata->msg->line.status.code;
  }

  static void TearDownTestCase()
  {
    SipTest::TearDownTestCase();
  }

  virtual ~ThreadDispatcherTest()
  {
    cwtest_reset_time();

    delete mod_mock;
    unregister_thread_dispatcher();
  }

  StrictMock<MockPJSipModule>* mod_mock;
  ::testing::StrictMock<MockLoadMonitor> load_monitor;
  pjsip_module* mod_thread_dispatcher;
  pjsip_process_rdata_param rp;
};

TEST_F(ThreadDispatcherTest, StandardInviteTest)
{
  TestingCommon::Message msg;
  msg._method = "INVITE";

  EXPECT_CALL(load_monitor, admit_request(_)).WillOnce(Return(true));
  EXPECT_CALL(*mod_mock, on_rx_request(_)).WillOnce(Return(PJ_TRUE));
  EXPECT_CALL(load_monitor, get_target_latency_us()).WillOnce(Return(100000));
  EXPECT_CALL(load_monitor, request_complete(_, _));

  inject_msg_thread(msg.get_request());
  process_queue_element();
}

TEST_F(ThreadDispatcherTest, SlowInviteTest)
{
  TestingCommon::Message msg;
  msg._method = "INVITE";

  EXPECT_CALL(load_monitor, admit_request(_)).WillOnce(Return(true));

  // Slow responses get logged out by a different path, where slow means
  // that > 50 * target latency
  EXPECT_CALL(*mod_mock, on_rx_request(_)).WillOnce(DoAll(
    InvokeWithoutArgs([](){ cwtest_advance_time_ms(6000); }),
    Return(PJ_TRUE)));

  EXPECT_CALL(load_monitor, get_target_latency_us()).WillOnce(Return(10));

  EXPECT_CALL(load_monitor, request_complete(_, _));

  inject_msg_thread(msg.get_request());
  process_queue_element();
}

// Invites should be rejected with a 503 if the load monitor returns false.
TEST_F(ThreadDispatcherTest, OverloadedInviteTest)
{
  TestingCommon::Message msg;
  msg._method = "INVITE";

  EXPECT_CALL(load_monitor, admit_request(_)).WillOnce(Return(false));
  EXPECT_CALL(load_monitor, get_target_latency_us()).WillOnce(Return(100000));
  EXPECT_CALL(*mod_mock, on_tx_response(ResultOf(get_tx_status_code, 503)));

  inject_msg_thread(msg.get_request());
}

// Invites older than the specified request_on_queue_timeout parameter should
// be rejected with a 503.
TEST_F(ThreadDispatcherTest, RejectOldInviteTest)
{
  TestingCommon::Message msg;
  msg._method = "INVITE";

  EXPECT_CALL(load_monitor, admit_request(_)).WillOnce(Return(true));
  EXPECT_CALL(load_monitor, get_target_latency_us()).WillOnce(Return(100000));
  EXPECT_CALL(*mod_mock, on_tx_response(ResultOf(get_tx_status_code, 503)));

  inject_msg_thread(msg.get_request());
  cwtest_advance_time_ms(REQUEST_ON_QUEUE_TIMEOUT_MS + 5);
  process_queue_element();
}

// On recieving an OPTIONS message, the thread dispatcher should not call into
// the load monitor - it should process the request regardless of load.
TEST_F(ThreadDispatcherTest, NeverRejectOptionsTest)
{
  TestingCommon::Message msg;
  msg._method = "OPTIONS";

  EXPECT_CALL(*mod_mock, on_rx_request(_)).WillOnce(Return(PJ_TRUE));
  EXPECT_CALL(load_monitor, get_target_latency_us()).WillOnce(Return(100000));
  EXPECT_CALL(load_monitor, request_complete(_, _));

  inject_msg_thread(msg.get_request());
  process_queue_element();
}

// On recieving a SUBSCRIBE message, the thread dispatcher should not call into
// the load monitor - it should process the request regardless of load.
TEST_F(ThreadDispatcherTest, NeverRejectSubscribeTest)
{
  TestingCommon::Message msg;
  msg._method = "SUBSCRIBE";

  EXPECT_CALL(*mod_mock, on_rx_request(_)).WillOnce(Return(PJ_TRUE));
  EXPECT_CALL(load_monitor, get_target_latency_us()).WillOnce(Return(100000));
  EXPECT_CALL(load_monitor, request_complete(_, _));

  inject_msg_thread(msg.get_request());
  process_queue_element();
}

// On recieving a SIP response, the thread dispatcher should not call into the
// load monitor - it should process the request regardless of load.
TEST_F(ThreadDispatcherTest, NeverRejectResponseTest)
{
  TestingCommon::Message msg;
  msg._method = "INVITE";
  msg._status = "200 OK";

  EXPECT_CALL(*mod_mock, on_rx_response(_)).WillOnce(Return(PJ_TRUE));
  EXPECT_CALL(load_monitor, get_target_latency_us()).WillOnce(Return(100000));
  EXPECT_CALL(load_monitor, request_complete(_, _));

  inject_msg_thread(msg.get_response());
  process_queue_element();
}

// Queued callbacks should be run then destroyed.
TEST_F(ThreadDispatcherTest, CallbackTest)
{
  EXPECT_CALL(load_monitor, get_target_latency_us()).WillOnce(Return(100000));

  StrictMock<MockCallback>* cb = new StrictMock<MockCallback>();
  add_callback_to_queue(cb);

  EXPECT_CALL(*cb, run());
  EXPECT_CALL(*cb, destruct());

  process_queue_element();
}

// OPTIONS messages should be prioritised over other message types.
TEST_F(ThreadDispatcherTest, PrioritiseOptionsTest)
{
  TestingCommon::Message invite_msg;
  invite_msg._method = "INVITE";

  TestingCommon::Message options_msg;
  options_msg._method = "OPTIONS";

  // Only the INVITE should check the load monitor
  EXPECT_CALL(load_monitor, admit_request(_)).WillOnce(Return(true));

  // The OPTIONS poll should be processed first
  Expectation options_exp = EXPECT_CALL(*mod_mock,
    on_rx_request(ResultOf(rx_call_id_matches(options_msg.get_call_id()), true)))
    .WillOnce(Return(PJ_TRUE));

  // The INVITE poll should be processed second
  EXPECT_CALL(*mod_mock,
    on_rx_request(ResultOf(rx_call_id_matches(invite_msg.get_call_id()), true)))
    .After(options_exp)
    .WillOnce(Return(PJ_TRUE));

  EXPECT_CALL(load_monitor, get_target_latency_us()).WillRepeatedly(Return(100000));
  EXPECT_CALL(load_monitor, request_complete(_, _)).Times(2);

  inject_msg_thread(invite_msg.get_request());
  inject_msg_thread(options_msg.get_request());

  process_queue_element();
  process_queue_element();
}

// Older messages should be prioritised over newer ones.
TEST_F(ThreadDispatcherTest, PrioritiseOlderTest)
{
  TestingCommon::Message older_msg;
  older_msg._method = "INVITE";

  TestingCommon::Message newer_msg;
  newer_msg._method = "INVITE";

  EXPECT_CALL(load_monitor, admit_request(_)).Times(2).WillRepeatedly(Return(true));

  // The older message should be processed first
  Expectation older_exp = EXPECT_CALL(*mod_mock,
    on_rx_request(ResultOf(rx_call_id_matches(older_msg.get_call_id()), true)))
    .WillOnce(Return(PJ_TRUE));

  // The newer message should be processed second
  EXPECT_CALL(*mod_mock,
    on_rx_request(ResultOf(rx_call_id_matches(newer_msg.get_call_id()), true)))
    .After(older_exp)
    .WillOnce(Return(PJ_TRUE));

  EXPECT_CALL(load_monitor, get_target_latency_us()).WillRepeatedly(Return(100000));
  EXPECT_CALL(load_monitor, request_complete(_, _)).Times(2);

  inject_msg_thread(older_msg.get_request());
  cwtest_advance_time_ms(1);
  inject_msg_thread(newer_msg.get_request());

  process_queue_element();
  process_queue_element();
}

// OPTIONS messages should be processed before INVITE messages, even if the
// INVITEs are older.
TEST_F(ThreadDispatcherTest, PrioritiseOptionsOverOlderTest)
{
  TestingCommon::Message invite_msg;
  invite_msg._method = "INVITE";

  TestingCommon::Message options_msg;
  options_msg._method = "OPTIONS";

  // Only the INVITE should check the load monitor
  EXPECT_CALL(load_monitor, admit_request(_)).WillOnce(Return(true));

  // The OPTIONS poll should be processed first
  Expectation options_exp = EXPECT_CALL(*mod_mock,
    on_rx_request(ResultOf(rx_call_id_matches(options_msg.get_call_id()), true)))
    .WillOnce(Return(PJ_TRUE));

  // The INVITE poll should be processed second
  EXPECT_CALL(*mod_mock,
    on_rx_request(ResultOf(rx_call_id_matches(invite_msg.get_call_id()), true)))
    .After(options_exp)
    .WillOnce(Return(PJ_TRUE));

  EXPECT_CALL(load_monitor, get_target_latency_us()).WillRepeatedly(Return(100000));
  EXPECT_CALL(load_monitor, request_complete(_, _)).Times(2);

  inject_msg_thread(invite_msg.get_request());
  cwtest_advance_time_ms(1);
  inject_msg_thread(options_msg.get_request());

  process_queue_element();
  process_queue_element();
}

class SipEventQueueTest : public ::testing::Test
{
public:
  SipEventQueueTest()
  {
    // We can distinguish e1 and e2 by the value of en.event_data.rdata
    SipEventData event_data;
    event_data.rdata = &rdata_1;
    e1.type = MESSAGE;
    e1.event_data = event_data;

    event_data.rdata = &rdata_2;
    e2.type = MESSAGE;
    e2.event_data = event_data;

    PriorityEventQueueBackend* q_backend = new PriorityEventQueueBackend();
    q = new eventq<struct SipEvent>(0, true, q_backend);

    cwtest_completely_control_time();
  }

  virtual ~SipEventQueueTest()
  {
    cwtest_reset_time();

    delete q;
    q = nullptr;
  }

  SipEvent e1;
  SipEvent e2;

  pjsip_rx_data rdata_1;
  pjsip_rx_data rdata_2;

  eventq<struct SipEvent>* q;
};

// Test that higher priority SipEvents are 'larger' than lower priority ones.
// 'Larger' SipEvents are returned sooner by the priority queue.
TEST_F(SipEventQueueTest, PriorityOrdering)
{
  // Lower the priority of e2
  e2.priority = 1;

  // e1 should be 'larger' than e2
  EXPECT_TRUE(SipEvent::compare(e2, e1));
  EXPECT_TRUE(SipEvent::compare(e2, e1));
}

// Test that older SipEvents are 'larger' than newer ones at the same priority
// level.
TEST_F(SipEventQueueTest, TimeOrdering)
{
  // Set e1 to be older than e2
  e1.stop_watch.start();
  cwtest_advance_time_ms(1);
  e2.stop_watch.start();

  // e1 should be 'larger' than e2
  EXPECT_TRUE(SipEvent::compare(e2, e1));
  EXPECT_TRUE(SipEvent::compare(e2, e1));
}

// Test that SipEvents are ordered by priority before time.
TEST_F(SipEventQueueTest, PriorityAndTimeOrdering)
{
  // Lower the priority of e2
  e2.priority = 1;

  // Set e2 to be older than e1
  e1.stop_watch.start();
  cwtest_advance_time_ms(1);
  e2.stop_watch.start();

  // e1 should be 'larger' than e2
  EXPECT_TRUE(SipEvent::compare(e2, e1));
  EXPECT_TRUE(SipEvent::compare(e2, e1));
}

// Test that higher priority SipEvents are returned before lower priority ones.
TEST_F(SipEventQueueTest, QueuePriorityOrdering)
{
  // Lower the priority of e2
  e2.priority = 1;

  q->push(e2);
  q->push(e1);

  SipEvent e;

  // e1 is higher priority, so should be returned first
  q->pop(e);
  EXPECT_EQ(e1.event_data.rdata, e.event_data.rdata);

  q->pop(e);
  EXPECT_EQ(e2.priority, e.priority);
}

// Test that older SipEvents are returned before newer ones at the same priority
// level.
TEST_F(SipEventQueueTest, QueueTimeOrdering)
{
  // Set e1 to be older than e2
  e1.stop_watch.start();
  cwtest_advance_time_ms(1);
  e2.stop_watch.start();

  q->push(e2);
  q->push(e1);

  SipEvent e;

  // e1 is older, so should be returned first
  q->pop(e);
  EXPECT_EQ(e1.event_data.rdata, e.event_data.rdata);

  q->pop(e);
  EXPECT_EQ(e2.event_data.rdata, e.event_data.rdata);
}

// Test that SipEvents are returned from the queue in priority, then time, order.
TEST_F(SipEventQueueTest, QueuePriorityAndTimeOrdering)
{
  // Lower the priority of e2
  e2.priority = 1;

  // Set e2 to be older than e1
  e1.stop_watch.start();
  cwtest_advance_time_ms(1);
  e2.stop_watch.start();

  q->push(e2);
  q->push(e1);

  SipEvent e;

  // e1 is higher priority, so should be returned first despite e2 being older
  q->pop(e);
  EXPECT_EQ(e1.event_data.rdata, e.event_data.rdata);

  q->pop(e);
  EXPECT_EQ(e2.event_data.rdata, e.event_data.rdata);
}
