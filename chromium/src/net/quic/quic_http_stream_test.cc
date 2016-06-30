// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/quic_http_stream.h"

#include <stdint.h>

#include <memory>
#include <vector>

#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/thread_task_runner_handle.h"
#include "net/base/chunked_upload_data_stream.h"
#include "net/base/elements_upload_data_stream.h"
#include "net/base/net_errors.h"
#include "net/base/socket_performance_watcher.h"
#include "net/base/test_completion_callback.h"
#include "net/base/test_data_directory.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/http/http_response_headers.h"
#include "net/http/transport_security_state.h"
#include "net/log/test_net_log.h"
#include "net/log/test_net_log_util.h"
#include "net/quic/congestion_control/send_algorithm_interface.h"
#include "net/quic/crypto/crypto_protocol.h"
#include "net/quic/crypto/proof_verifier_chromium.h"
#include "net/quic/crypto/quic_decrypter.h"
#include "net/quic/crypto/quic_encrypter.h"
#include "net/quic/crypto/quic_server_info.h"
#include "net/quic/quic_chromium_alarm_factory.h"
#include "net/quic/quic_chromium_client_session.h"
#include "net/quic/quic_chromium_client_stream.h"
#include "net/quic/quic_chromium_connection_helper.h"
#include "net/quic/quic_chromium_packet_reader.h"
#include "net/quic/quic_chromium_packet_writer.h"
#include "net/quic/quic_connection.h"
#include "net/quic/quic_http_utils.h"
#include "net/quic/quic_write_blocked_list.h"
#include "net/quic/spdy_utils.h"
#include "net/quic/test_tools/crypto_test_utils.h"
#include "net/quic/test_tools/mock_clock.h"
#include "net/quic/test_tools/mock_crypto_client_stream_factory.h"
#include "net/quic/test_tools/mock_random.h"
#include "net/quic/test_tools/quic_connection_peer.h"
#include "net/quic/test_tools/quic_test_packet_maker.h"
#include "net/quic/test_tools/quic_test_utils.h"
#include "net/quic/test_tools/test_task_runner.h"
#include "net/socket/socket_test_util.h"
#include "net/spdy/spdy_frame_builder.h"
#include "net/spdy/spdy_framer.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/spdy/spdy_protocol.h"
#include "net/test/cert_test_util.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using std::string;
using testing::_;
using testing::AnyNumber;
using testing::Return;

namespace net {
namespace test {
namespace {

const char kUploadData[] = "Really nifty data!";
const char kDefaultServerHostName[] = "www.example.org";
const uint16_t kDefaultServerPort = 80;

class TestQuicConnection : public QuicConnection {
 public:
  TestQuicConnection(const QuicVersionVector& versions,
                     QuicConnectionId connection_id,
                     IPEndPoint address,
                     QuicChromiumConnectionHelper* helper,
                     QuicChromiumAlarmFactory* alarm_factory,
                     QuicPacketWriter* writer)
      : QuicConnection(connection_id,
                       address,
                       helper,
                       alarm_factory,
                       writer,
                       true /* owns_writer */,
                       Perspective::IS_CLIENT,
                       versions) {}

  void SetSendAlgorithm(SendAlgorithmInterface* send_algorithm) {
    QuicConnectionPeer::SetSendAlgorithm(this, send_algorithm);
  }
};

// Subclass of QuicHttpStream that closes itself when the first piece of data
// is received.
class AutoClosingStream : public QuicHttpStream {
 public:
  explicit AutoClosingStream(
      const base::WeakPtr<QuicChromiumClientSession>& session)
      : QuicHttpStream(session) {}

  void OnHeadersAvailable(const SpdyHeaderBlock& headers,
                          size_t frame_len) override {
    Close(false);
  }

  void OnDataAvailable() override { Close(false); }
};

}  // namespace

class QuicHttpStreamPeer {
 public:
  static QuicChromiumClientStream* GetQuicChromiumClientStream(
      QuicHttpStream* stream) {
    return stream->stream_;
  }

  static bool WasHandshakeConfirmed(QuicHttpStream* stream) {
    return stream->was_handshake_confirmed_;
  }

  static void SetHandshakeConfirmed(QuicHttpStream* stream, bool confirmed) {
    stream->was_handshake_confirmed_ = confirmed;
  }
};

class QuicHttpStreamTest : public ::testing::TestWithParam<QuicVersion> {
 protected:
  static const bool kFin = true;
  static const bool kIncludeVersion = true;
  static const bool kIncludeCongestionFeedback = true;

  // Holds a packet to be written to the wire, and the IO mode that should
  // be used by the mock socket when performing the write.
  struct PacketToWrite {
    PacketToWrite(IoMode mode, QuicReceivedPacket* packet)
        : mode(mode), packet(packet) {}
    PacketToWrite(IoMode mode, int rv) : mode(mode), packet(nullptr), rv(rv) {}
    IoMode mode;
    QuicReceivedPacket* packet;
    int rv;
  };

  QuicHttpStreamTest()
      : use_closing_stream_(false),
        crypto_config_(CryptoTestUtils::ProofVerifierForTesting()),
        read_buffer_(new IOBufferWithSize(4096)),
        promise_id_(kServerDataStreamId1),
        stream_id_(kClientDataStreamId1),
        connection_id_(2),
        maker_(GetParam(), connection_id_, &clock_, kDefaultServerHostName),
        random_generator_(0),
        response_offset_(0) {
    IPAddress ip(192, 0, 2, 33);
    peer_addr_ = IPEndPoint(ip, 443);
    self_addr_ = IPEndPoint(ip, 8435);
    clock_.AdvanceTime(QuicTime::Delta::FromMilliseconds(20));
  }

  ~QuicHttpStreamTest() {
    session_->CloseSessionOnError(ERR_ABORTED, QUIC_INTERNAL_ERROR);
    for (size_t i = 0; i < writes_.size(); i++) {
      delete writes_[i].packet;
    }
  }

  // Adds a packet to the list of expected writes.
  void AddWrite(std::unique_ptr<QuicReceivedPacket> packet) {
    writes_.push_back(PacketToWrite(SYNCHRONOUS, packet.release()));
  }

  void AddWrite(IoMode mode, int rv) {
    writes_.push_back(PacketToWrite(mode, rv));
  }

  // Returns the packet to be written at position |pos|.
  QuicReceivedPacket* GetWrite(size_t pos) { return writes_[pos].packet; }

  bool AtEof() {
    return socket_data_->AllReadDataConsumed() &&
           socket_data_->AllWriteDataConsumed();
  }

  void ProcessPacket(std::unique_ptr<QuicReceivedPacket> packet) {
    connection_->ProcessUdpPacket(self_addr_, peer_addr_, *packet);
  }

  // Configures the test fixture to use the list of expected writes.
  void Initialize() {
    mock_writes_.reset(new MockWrite[writes_.size()]);
    for (size_t i = 0; i < writes_.size(); i++) {
      if (writes_[i].packet == nullptr) {
        mock_writes_[i] = MockWrite(writes_[i].mode, writes_[i].rv, i);
      } else {
        mock_writes_[i] = MockWrite(writes_[i].mode, writes_[i].packet->data(),
                                    writes_[i].packet->length());
      }
    }

    socket_data_.reset(new StaticSocketDataProvider(
        nullptr, 0, mock_writes_.get(), writes_.size()));

    std::unique_ptr<MockUDPClientSocket> socket(new MockUDPClientSocket(
        socket_data_.get(), net_log_.bound().net_log()));
    socket->Connect(peer_addr_);
    runner_ = new TestTaskRunner(&clock_);
    send_algorithm_ = new MockSendAlgorithm();
    EXPECT_CALL(*send_algorithm_, InRecovery()).WillRepeatedly(Return(false));
    EXPECT_CALL(*send_algorithm_, InSlowStart()).WillRepeatedly(Return(false));
    EXPECT_CALL(*send_algorithm_, OnPacketSent(_, _, _, _, _))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*send_algorithm_, RetransmissionDelay())
        .WillRepeatedly(Return(QuicTime::Delta::Zero()));
    EXPECT_CALL(*send_algorithm_, GetCongestionWindow())
        .WillRepeatedly(Return(kMaxPacketSize));
    EXPECT_CALL(*send_algorithm_, PacingRate())
        .WillRepeatedly(Return(QuicBandwidth::Zero()));
    EXPECT_CALL(*send_algorithm_, TimeUntilSend(_, _))
        .WillRepeatedly(Return(QuicTime::Delta::Zero()));
    EXPECT_CALL(*send_algorithm_, BandwidthEstimate())
        .WillRepeatedly(Return(QuicBandwidth::Zero()));
    EXPECT_CALL(*send_algorithm_, SetFromConfig(_, _)).Times(AnyNumber());
    helper_.reset(
        new QuicChromiumConnectionHelper(&clock_, &random_generator_));
    alarm_factory_.reset(new QuicChromiumAlarmFactory(runner_.get(), &clock_));

    connection_ =
        new TestQuicConnection(SupportedVersions(GetParam()), connection_id_,
                               peer_addr_, helper_.get(), alarm_factory_.get(),
                               new QuicChromiumPacketWriter(socket.get()));
    connection_->set_visitor(&visitor_);
    connection_->SetSendAlgorithm(send_algorithm_);

    // Load a certificate that is valid for *.example.org
    scoped_refptr<X509Certificate> test_cert(
        ImportCertFromFile(GetTestCertsDirectory(), "wildcard.pem"));
    EXPECT_TRUE(test_cert.get());

    verify_details_.cert_verify_result.verified_cert = test_cert;
    verify_details_.cert_verify_result.is_issued_by_known_root = true;
    crypto_client_stream_factory_.AddProofVerifyDetails(&verify_details_);

    session_.reset(new QuicChromiumClientSession(
        connection_, std::move(socket),
        /*stream_factory=*/nullptr, &crypto_client_stream_factory_, &clock_,
        &transport_security_state_,
        base::WrapUnique(static_cast<QuicServerInfo*>(nullptr)),
        QuicServerId(kDefaultServerHostName, kDefaultServerPort,
                     PRIVACY_MODE_DISABLED),
        kQuicYieldAfterPacketsRead,
        QuicTime::Delta::FromMilliseconds(kQuicYieldAfterDurationMilliseconds),
        /*cert_verify_flags=*/0, DefaultQuicConfig(), &crypto_config_,
        "CONNECTION_UNKNOWN", base::TimeTicks::Now(), &push_promise_index_,
        base::ThreadTaskRunnerHandle::Get().get(),
        /*socket_performance_watcher=*/nullptr, net_log_.bound().net_log()));
    session_->Initialize();
    session_->GetCryptoStream()->CryptoConnect();
    EXPECT_TRUE(session_->IsCryptoHandshakeConfirmed());
    stream_.reset(use_closing_stream_
                      ? new AutoClosingStream(session_->GetWeakPtr())
                      : new QuicHttpStream(session_->GetWeakPtr()));

    promised_stream_.reset(use_closing_stream_
                               ? new AutoClosingStream(session_->GetWeakPtr())
                               : new QuicHttpStream(session_->GetWeakPtr()));

    push_promise_[":path"] = "/bar";
    push_promise_[":authority"] = "www.example.org";
    push_promise_[":version"] = "HTTP/1.1";
    push_promise_[":method"] = "GET";
    push_promise_[":scheme"] = "https";

    promised_response_[":status"] = "200 OK";
    promised_response_[":version"] = "HTTP/1.1";
    promised_response_["content-type"] = "text/plain";

    promise_url_ = SpdyUtils::GetUrlFromHeaderBlock(push_promise_);

    serialized_push_promise_ =
        SpdyUtils::SerializeUncompressedHeaders(push_promise_);
  }

  void SetRequest(const string& method,
                  const string& path,
                  RequestPriority priority) {
    request_headers_ = maker_.GetRequestHeaders(method, "http", path);
  }

  void SetResponse(const string& status, const string& body) {
    response_headers_ = maker_.GetResponseHeaders(status);
    response_data_ = body;
  }

  std::unique_ptr<QuicReceivedPacket> InnerConstructDataPacket(
      QuicPacketNumber packet_number,
      QuicStreamId stream_id,
      bool should_include_version,
      bool fin,
      QuicStreamOffset offset,
      base::StringPiece data) {
    return maker_.MakeDataPacket(packet_number, stream_id,
                                 should_include_version, fin, offset, data);
  }

  std::unique_ptr<QuicReceivedPacket> ConstructDataPacket(
      QuicPacketNumber packet_number,
      bool should_include_version,
      bool fin,
      QuicStreamOffset offset,
      base::StringPiece data) {
    return InnerConstructDataPacket(packet_number, stream_id_,
                                    should_include_version, fin, offset, data);
  }

  std::unique_ptr<QuicReceivedPacket> InnerConstructRequestHeadersPacket(
      QuicPacketNumber packet_number,
      QuicStreamId stream_id,
      bool should_include_version,
      bool fin,
      RequestPriority request_priority,
      size_t* spdy_headers_frame_length) {
    SpdyPriority priority =
        ConvertRequestPriorityToQuicPriority(request_priority);
    return maker_.MakeRequestHeadersPacket(
        packet_number, stream_id, should_include_version, fin, priority,
        request_headers_, spdy_headers_frame_length);
  }

  std::unique_ptr<QuicReceivedPacket> ConstructRequestHeadersPacket(
      QuicPacketNumber packet_number,
      bool fin,
      RequestPriority request_priority,
      size_t* spdy_headers_frame_length) {
    return InnerConstructRequestHeadersPacket(
        packet_number, stream_id_, kIncludeVersion, fin, request_priority,
        spdy_headers_frame_length);
  }

  std::unique_ptr<QuicReceivedPacket> InnerConstructResponseHeadersPacket(
      QuicPacketNumber packet_number,
      QuicStreamId stream_id,
      bool fin,
      size_t* spdy_headers_frame_length) {
    return maker_.MakeResponseHeadersPacket(
        packet_number, stream_id, !kIncludeVersion, fin, response_headers_,
        spdy_headers_frame_length, &response_offset_);
  }

  std::unique_ptr<QuicReceivedPacket> ConstructResponseHeadersPacket(
      QuicPacketNumber packet_number,
      bool fin,
      size_t* spdy_headers_frame_length) {
    return InnerConstructResponseHeadersPacket(packet_number, stream_id_, fin,
                                               spdy_headers_frame_length);
  }

  std::unique_ptr<QuicReceivedPacket> ConstructResponseHeadersPacketWithOffset(
      QuicPacketNumber packet_number,
      bool fin,
      size_t* spdy_headers_frame_length,
      QuicStreamOffset* offset) {
    return maker_.MakeResponseHeadersPacket(
        packet_number, stream_id_, !kIncludeVersion, fin, response_headers_,
        spdy_headers_frame_length, offset);
  }

  std::unique_ptr<QuicReceivedPacket> ConstructResponseTrailersPacket(
      QuicPacketNumber packet_number,
      bool fin,
      const SpdyHeaderBlock& trailers,
      size_t* spdy_headers_frame_length,
      QuicStreamOffset* offset) {
    return maker_.MakeResponseHeadersPacket(packet_number, stream_id_,
                                            !kIncludeVersion, fin, trailers,
                                            spdy_headers_frame_length, offset);
  }

  std::unique_ptr<QuicReceivedPacket> ConstructRstStreamPacket(
      QuicPacketNumber packet_number) {
    return maker_.MakeRstPacket(
        packet_number, true, stream_id_,
        AdjustErrorForVersion(QUIC_RST_ACKNOWLEDGEMENT, GetParam()));
  }

  std::unique_ptr<QuicReceivedPacket> ConstructRstStreamCancelledPacket(
      QuicPacketNumber packet_number) {
    return maker_.MakeRstPacket(packet_number, !kIncludeVersion, stream_id_,
                                QUIC_STREAM_CANCELLED);
  }

  std::unique_ptr<QuicReceivedPacket> ConstructRstStreamVaryMismatchPacket(
      QuicPacketNumber packet_number) {
    return maker_.MakeRstPacket(packet_number, !kIncludeVersion, promise_id_,
                                QUIC_PROMISE_VARY_MISMATCH);
  }

  std::unique_ptr<QuicReceivedPacket> ConstructAckAndRstStreamPacket(
      QuicPacketNumber packet_number,
      QuicPacketNumber largest_received,
      QuicPacketNumber ack_least_unacked,
      QuicPacketNumber stop_least_unacked) {
    return maker_.MakeAckAndRstPacket(
        packet_number, !kIncludeVersion, stream_id_, QUIC_STREAM_CANCELLED,
        largest_received, ack_least_unacked, stop_least_unacked,
        !kIncludeCongestionFeedback);
  }

  std::unique_ptr<QuicReceivedPacket> ConstructAckAndRstStreamPacket(
      QuicPacketNumber packet_number) {
    return ConstructAckAndRstStreamPacket(packet_number, 2, 1, 1);
  }

  std::unique_ptr<QuicReceivedPacket> ConstructAckPacket(
      QuicPacketNumber packet_number,
      QuicPacketNumber largest_received,
      QuicPacketNumber least_unacked) {
    return maker_.MakeAckPacket(packet_number, largest_received, least_unacked,
                                !kIncludeCongestionFeedback);
  }

  void ReceivePromise(QuicStreamId id) {
    QuicChromiumClientStream* stream =
        QuicHttpStreamPeer::GetQuicChromiumClientStream(stream_.get());
    stream->OnStreamHeaders(serialized_push_promise_);

    stream->OnPromiseHeadersComplete(id, serialized_push_promise_.size());
  }

  BoundTestNetLog net_log_;
  bool use_closing_stream_;
  MockSendAlgorithm* send_algorithm_;
  scoped_refptr<TestTaskRunner> runner_;
  std::unique_ptr<MockWrite[]> mock_writes_;
  MockClock clock_;
  TestQuicConnection* connection_;
  std::unique_ptr<QuicChromiumConnectionHelper> helper_;
  std::unique_ptr<QuicChromiumAlarmFactory> alarm_factory_;
  testing::StrictMock<MockConnectionVisitor> visitor_;
  std::unique_ptr<QuicHttpStream> stream_;
  TransportSecurityState transport_security_state_;
  std::unique_ptr<QuicChromiumClientSession> session_;
  QuicCryptoClientConfig crypto_config_;
  TestCompletionCallback callback_;
  HttpRequestInfo request_;
  HttpRequestHeaders headers_;
  HttpResponseInfo response_;
  scoped_refptr<IOBufferWithSize> read_buffer_;
  SpdyHeaderBlock request_headers_;
  SpdyHeaderBlock response_headers_;
  string request_data_;
  string response_data_;
  QuicClientPushPromiseIndex push_promise_index_;

  // For server push testing
  std::unique_ptr<QuicHttpStream> promised_stream_;
  SpdyHeaderBlock push_promise_;
  SpdyHeaderBlock promised_response_;
  const QuicStreamId promise_id_;
  string promise_url_;
  string serialized_push_promise_;
  const QuicStreamId stream_id_;

 private:
  const QuicConnectionId connection_id_;
  QuicTestPacketMaker maker_;
  IPEndPoint self_addr_;
  IPEndPoint peer_addr_;
  MockRandom random_generator_;
  ProofVerifyDetailsChromium verify_details_;
  MockCryptoClientStreamFactory crypto_client_stream_factory_;
  std::unique_ptr<StaticSocketDataProvider> socket_data_;
  std::vector<PacketToWrite> writes_;
  QuicStreamOffset response_offset_;
};

INSTANTIATE_TEST_CASE_P(Version,
                        QuicHttpStreamTest,
                        ::testing::ValuesIn(QuicSupportedVersions()));

TEST_P(QuicHttpStreamTest, RenewStreamForAuth) {
  Initialize();
  EXPECT_EQ(nullptr, stream_->RenewStreamForAuth());
}

TEST_P(QuicHttpStreamTest, CanReuseConnection) {
  Initialize();
  EXPECT_FALSE(stream_->CanReuseConnection());
}

TEST_P(QuicHttpStreamTest, DisableConnectionMigrationForStream) {
  request_.load_flags |= LOAD_DISABLE_CONNECTION_MIGRATION;
  Initialize();
  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  QuicChromiumClientStream* client_stream =
      QuicHttpStreamPeer::GetQuicChromiumClientStream(stream_.get());
  EXPECT_FALSE(client_stream->can_migrate());
}

TEST_P(QuicHttpStreamTest, GetRequest) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  size_t spdy_request_header_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, kFin, DEFAULT_PRIORITY,
                                         &spdy_request_header_frame_length));
  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  EXPECT_EQ(OK,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  // Ack the request.
  ProcessPacket(ConstructAckPacket(1, 0, 0));

  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));

  SetResponse("404 Not Found", string());
  size_t spdy_response_header_frame_length;
  ProcessPacket(ConstructResponseHeadersPacket(
      2, kFin, &spdy_response_header_frame_length));

  // Now that the headers have been processed, the callback will return.
  EXPECT_EQ(OK, callback_.WaitForResult());
  ASSERT_TRUE(response_.headers.get());
  EXPECT_EQ(404, response_.headers->response_code());
  EXPECT_TRUE(response_.headers->HasHeaderValue("Content-Type", "text/plain"));
  EXPECT_FALSE(response_.response_time.is_null());
  EXPECT_FALSE(response_.request_time.is_null());

  // There is no body, so this should return immediately.
  EXPECT_EQ(0,
            stream_->ReadResponseBody(read_buffer_.get(), read_buffer_->size(),
                                      callback_.callback()));
  EXPECT_TRUE(stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_header_frame_length),
            stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_header_frame_length),
            stream_->GetTotalReceivedBytes());
}

// QuicHttpStream does not currently support trailers. It should ignore
// trailers upon receiving them.
TEST_P(QuicHttpStreamTest, GetRequestWithTrailers) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  size_t spdy_request_header_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, kFin, DEFAULT_PRIORITY,
                                         &spdy_request_header_frame_length));
  AddWrite(ConstructAckPacket(2, 3, 1));  // Ack the data packet.

  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));

  EXPECT_EQ(OK,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  // Ack the request.
  ProcessPacket(ConstructAckPacket(1, 0, 0));

  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));

  SetResponse("200 OK", string());

  // Send the response headers.
  size_t spdy_response_header_frame_length;
  QuicStreamOffset offset = 0;
  ProcessPacket(ConstructResponseHeadersPacketWithOffset(
      2, !kFin, &spdy_response_header_frame_length, &offset));
  // Now that the headers have been processed, the callback will return.
  EXPECT_EQ(OK, callback_.WaitForResult());
  ASSERT_TRUE(response_.headers.get());
  EXPECT_EQ(200, response_.headers->response_code());
  EXPECT_TRUE(response_.headers->HasHeaderValue("Content-Type", "text/plain"));
  EXPECT_FALSE(response_.response_time.is_null());
  EXPECT_FALSE(response_.request_time.is_null());

  // Send the response body.
  const char kResponseBody[] = "Hello world!";
  ProcessPacket(
      ConstructDataPacket(3, false, !kFin, /*offset=*/0, kResponseBody));
  SpdyHeaderBlock trailers;
  size_t spdy_trailers_frame_length;
  trailers["foo"] = "bar";
  trailers[kFinalOffsetHeaderKey] = base::IntToString(strlen(kResponseBody));
  ProcessPacket(ConstructResponseTrailersPacket(
      4, kFin, trailers, &spdy_trailers_frame_length, &offset));

  // Make sure trailers are processed.
  base::MessageLoop::current()->RunUntilIdle();

  EXPECT_EQ(static_cast<int>(strlen(kResponseBody)),
            stream_->ReadResponseBody(read_buffer_.get(), read_buffer_->size(),
                                      callback_.callback()));
  EXPECT_TRUE(stream_->IsResponseBodyComplete());

  EXPECT_EQ(OK,
            stream_->ReadResponseBody(read_buffer_.get(), read_buffer_->size(),
                                      callback_.callback()));

  EXPECT_TRUE(stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_header_frame_length),
            stream_->GetTotalSentBytes());
  EXPECT_EQ(
      static_cast<int64_t>(spdy_response_header_frame_length +
                           strlen(kResponseBody) + +spdy_trailers_frame_length),
      stream_->GetTotalReceivedBytes());
  // Check that NetLog was filled as expected.
  TestNetLogEntry::List entries;
  net_log_.GetEntries(&entries);
  size_t pos = ExpectLogContainsSomewhere(
      entries, /*min_offset=*/0,
      NetLog::TYPE_QUIC_CHROMIUM_CLIENT_STREAM_SEND_REQUEST_HEADERS,
      NetLog::PHASE_NONE);
  pos = ExpectLogContainsSomewhere(
      entries, /*min_offset=*/pos,
      NetLog::TYPE_QUIC_CHROMIUM_CLIENT_STREAM_SEND_REQUEST_HEADERS,
      NetLog::PHASE_NONE);
  ExpectLogContainsSomewhere(
      entries, /*min_offset=*/pos,
      NetLog::TYPE_QUIC_CHROMIUM_CLIENT_STREAM_SEND_REQUEST_HEADERS,
      NetLog::PHASE_NONE);
}

// Regression test for http://crbug.com/288128
TEST_P(QuicHttpStreamTest, GetRequestLargeResponse) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, kFin, DEFAULT_PRIORITY,
                                         &spdy_request_headers_frame_length));
  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  EXPECT_EQ(OK,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  // Ack the request.
  ProcessPacket(ConstructAckPacket(1, 0, 0));

  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));

  SpdyHeaderBlock headers;
  headers[":status"] = "200 OK";
  headers[":version"] = "HTTP/1.1";
  headers["content-type"] = "text/plain";
  headers["big6"] = string(1000, 'x');  // Lots of x's.

  response_headers_ = headers;
  size_t spdy_response_headers_frame_length;
  ProcessPacket(ConstructResponseHeadersPacket(
      2, kFin, &spdy_response_headers_frame_length));

  // Now that the headers have been processed, the callback will return.
  EXPECT_EQ(OK, callback_.WaitForResult());
  ASSERT_TRUE(response_.headers.get());
  EXPECT_EQ(200, response_.headers->response_code());
  EXPECT_TRUE(response_.headers->HasHeaderValue("Content-Type", "text/plain"));

  // There is no body, so this should return immediately.
  EXPECT_EQ(0,
            stream_->ReadResponseBody(read_buffer_.get(), read_buffer_->size(),
                                      callback_.callback()));
  EXPECT_TRUE(stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_headers_frame_length),
            stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_headers_frame_length),
            stream_->GetTotalReceivedBytes());
}

// Regression test for http://crbug.com/409101
TEST_P(QuicHttpStreamTest, SessionClosedBeforeSendRequest) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));

  session_->connection()->CloseConnection(
      QUIC_NO_ERROR, "test", ConnectionCloseBehavior::SILENT_CLOSE);

  EXPECT_EQ(ERR_CONNECTION_CLOSED,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  EXPECT_EQ(0, stream_->GetTotalSentBytes());
  EXPECT_EQ(0, stream_->GetTotalReceivedBytes());
}

// Regression test for http://crbug.com/584441
TEST_P(QuicHttpStreamTest, GetSSLInfoAfterSessionClosed) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));

  SSLInfo ssl_info;
  EXPECT_FALSE(ssl_info.is_valid());
  stream_->GetSSLInfo(&ssl_info);
  EXPECT_TRUE(ssl_info.is_valid());

  session_->connection()->CloseConnection(
      QUIC_NO_ERROR, "test", ConnectionCloseBehavior::SILENT_CLOSE);

  SSLInfo ssl_info2;
  stream_->GetSSLInfo(&ssl_info2);
  EXPECT_TRUE(ssl_info2.is_valid());
}

TEST_P(QuicHttpStreamTest, LogGranularQuicConnectionError) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, kFin, DEFAULT_PRIORITY,
                                         &spdy_request_headers_frame_length));
  AddWrite(ConstructAckAndRstStreamPacket(2));
  use_closing_stream_ = true;
  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  EXPECT_EQ(OK,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  // Ack the request.
  ProcessPacket(ConstructAckPacket(1, 0, 0));
  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));

  EXPECT_TRUE(QuicHttpStreamPeer::WasHandshakeConfirmed(stream_.get()));
  stream_->OnClose(QUIC_PEER_GOING_AWAY);

  NetErrorDetails details;
  EXPECT_EQ(QUIC_NO_ERROR, details.quic_connection_error);
  stream_->PopulateNetErrorDetails(&details);
  EXPECT_EQ(QUIC_PEER_GOING_AWAY, details.quic_connection_error);
}

TEST_P(QuicHttpStreamTest, DoNotLogGranularQuicErrorIfHandshakeNotConfirmed) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, kFin, DEFAULT_PRIORITY,
                                         &spdy_request_headers_frame_length));
  AddWrite(ConstructAckAndRstStreamPacket(2));
  use_closing_stream_ = true;
  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  EXPECT_EQ(OK,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  // Ack the request.
  ProcessPacket(ConstructAckPacket(1, 0, 0));
  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));

  // The test setup defaults handshake to be confirmed. Manually set
  // it to be not confirmed.
  // Granular errors shouldn't be reported if handshake not confirmed.
  QuicHttpStreamPeer::SetHandshakeConfirmed(stream_.get(), false);

  EXPECT_FALSE(QuicHttpStreamPeer::WasHandshakeConfirmed(stream_.get()));
  stream_->OnClose(QUIC_PEER_GOING_AWAY);

  NetErrorDetails details;
  EXPECT_EQ(QUIC_NO_ERROR, details.quic_connection_error);
  stream_->PopulateNetErrorDetails(&details);
  EXPECT_EQ(QUIC_NO_ERROR, details.quic_connection_error);
}

// Regression test for http://crbug.com/409871
TEST_P(QuicHttpStreamTest, SessionClosedBeforeReadResponseHeaders) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, kFin, DEFAULT_PRIORITY,
                                         &spdy_request_headers_frame_length));
  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));

  EXPECT_EQ(OK,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  session_->connection()->CloseConnection(
      QUIC_NO_ERROR, "test", ConnectionCloseBehavior::SILENT_CLOSE);

  EXPECT_NE(OK, stream_->ReadResponseHeaders(callback_.callback()));

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_headers_frame_length),
            stream_->GetTotalSentBytes());
  EXPECT_EQ(0, stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, SendPostRequest) {
  SetRequest("POST", "/", DEFAULT_PRIORITY);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, !kFin, DEFAULT_PRIORITY,
                                         &spdy_request_headers_frame_length));
  AddWrite(ConstructDataPacket(2, kIncludeVersion, kFin, 0, kUploadData));
  AddWrite(ConstructAckPacket(3, 3, 1));

  Initialize();

  std::vector<std::unique_ptr<UploadElementReader>> element_readers;
  element_readers.push_back(base::WrapUnique(
      new UploadBytesElementReader(kUploadData, strlen(kUploadData))));
  ElementsUploadDataStream upload_data_stream(std::move(element_readers), 0);
  request_.method = "POST";
  request_.url = GURL("http://www.example.org/");
  request_.upload_data_stream = &upload_data_stream;
  ASSERT_EQ(OK, request_.upload_data_stream->Init(CompletionCallback()));

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  EXPECT_EQ(OK,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  // Ack both packets in the request.
  ProcessPacket(ConstructAckPacket(1, 0, 0));

  // Send the response headers (but not the body).
  SetResponse("200 OK", string());
  size_t spdy_response_headers_frame_length;
  ProcessPacket(ConstructResponseHeadersPacket(
      2, !kFin, &spdy_response_headers_frame_length));

  // The headers have arrived, but they are delivered asynchronously.
  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));
  EXPECT_EQ(OK, callback_.WaitForResult());
  ASSERT_TRUE(response_.headers.get());
  EXPECT_EQ(200, response_.headers->response_code());
  EXPECT_TRUE(response_.headers->HasHeaderValue("Content-Type", "text/plain"));

  // Send the response body.
  const char kResponseBody[] = "Hello world!";
  ProcessPacket(ConstructDataPacket(3, false, kFin, 0, kResponseBody));
  // Since the body has already arrived, this should return immediately.
  EXPECT_EQ(static_cast<int>(strlen(kResponseBody)),
            stream_->ReadResponseBody(read_buffer_.get(), read_buffer_->size(),
                                      callback_.callback()));

  EXPECT_TRUE(stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_headers_frame_length +
                                 strlen(kUploadData)),
            stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_headers_frame_length +
                                 strlen(kResponseBody)),
            stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, SendChunkedPostRequest) {
  SetRequest("POST", "/", DEFAULT_PRIORITY);
  size_t chunk_size = strlen(kUploadData);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, !kFin, DEFAULT_PRIORITY,
                                         &spdy_request_headers_frame_length));
  AddWrite(ConstructDataPacket(2, kIncludeVersion, !kFin, 0, kUploadData));
  AddWrite(
      ConstructDataPacket(3, kIncludeVersion, kFin, chunk_size, kUploadData));
  AddWrite(ConstructAckPacket(4, 3, 1));
  Initialize();

  ChunkedUploadDataStream upload_data_stream(0);
  upload_data_stream.AppendData(kUploadData, chunk_size, false);

  request_.method = "POST";
  request_.url = GURL("http://www.example.org/");
  request_.upload_data_stream = &upload_data_stream;
  ASSERT_EQ(OK, request_.upload_data_stream->Init(
                    TestCompletionCallback().callback()));

  ASSERT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  ASSERT_EQ(ERR_IO_PENDING,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  upload_data_stream.AppendData(kUploadData, chunk_size, true);
  EXPECT_EQ(OK, callback_.WaitForResult());

  // Ack both packets in the request.
  ProcessPacket(ConstructAckPacket(1, 0, 0));

  // Send the response headers (but not the body).
  SetResponse("200 OK", string());
  size_t spdy_response_headers_frame_length;
  ProcessPacket(ConstructResponseHeadersPacket(
      2, !kFin, &spdy_response_headers_frame_length));

  // The headers have arrived, but they are delivered asynchronously
  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));
  EXPECT_EQ(OK, callback_.WaitForResult());
  ASSERT_TRUE(response_.headers.get());
  EXPECT_EQ(200, response_.headers->response_code());
  EXPECT_TRUE(response_.headers->HasHeaderValue("Content-Type", "text/plain"));

  // Send the response body.
  const char kResponseBody[] = "Hello world!";
  ProcessPacket(ConstructDataPacket(3, false, kFin, response_data_.length(),
                                    kResponseBody));

  // Since the body has already arrived, this should return immediately.
  ASSERT_EQ(static_cast<int>(strlen(kResponseBody)),
            stream_->ReadResponseBody(read_buffer_.get(), read_buffer_->size(),
                                      callback_.callback()));

  EXPECT_TRUE(stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_headers_frame_length +
                                 strlen(kUploadData) * 2),
            stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_headers_frame_length +
                                 strlen(kResponseBody)),
            stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, SendChunkedPostRequestWithFinalEmptyDataPacket) {
  SetRequest("POST", "/", DEFAULT_PRIORITY);
  size_t chunk_size = strlen(kUploadData);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, !kFin, DEFAULT_PRIORITY,
                                         &spdy_request_headers_frame_length));
  AddWrite(ConstructDataPacket(2, kIncludeVersion, !kFin, 0, kUploadData));
  AddWrite(ConstructDataPacket(3, kIncludeVersion, kFin, chunk_size, ""));
  AddWrite(ConstructAckPacket(4, 3, 1));
  Initialize();

  ChunkedUploadDataStream upload_data_stream(0);
  upload_data_stream.AppendData(kUploadData, chunk_size, false);

  request_.method = "POST";
  request_.url = GURL("http://www.example.org/");
  request_.upload_data_stream = &upload_data_stream;
  ASSERT_EQ(OK, request_.upload_data_stream->Init(
                    TestCompletionCallback().callback()));

  ASSERT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  ASSERT_EQ(ERR_IO_PENDING,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  upload_data_stream.AppendData(nullptr, 0, true);
  EXPECT_EQ(OK, callback_.WaitForResult());

  ProcessPacket(ConstructAckPacket(1, 0, 0));

  // Send the response headers (but not the body).
  SetResponse("200 OK", string());
  size_t spdy_response_headers_frame_length;
  ProcessPacket(ConstructResponseHeadersPacket(
      2, !kFin, &spdy_response_headers_frame_length));

  // The headers have arrived, but they are delivered asynchronously
  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));
  EXPECT_EQ(OK, callback_.WaitForResult());
  ASSERT_TRUE(response_.headers.get());
  EXPECT_EQ(200, response_.headers->response_code());
  EXPECT_TRUE(response_.headers->HasHeaderValue("Content-Type", "text/plain"));

  // Send the response body.
  const char kResponseBody[] = "Hello world!";
  ProcessPacket(ConstructDataPacket(3, false, kFin, response_data_.length(),
                                    kResponseBody));

  // The body has arrived, but it is delivered asynchronously
  ASSERT_EQ(static_cast<int>(strlen(kResponseBody)),
            stream_->ReadResponseBody(read_buffer_.get(), read_buffer_->size(),
                                      callback_.callback()));
  EXPECT_TRUE(stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_headers_frame_length +
                                 strlen(kUploadData)),
            stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_headers_frame_length +
                                 strlen(kResponseBody)),
            stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, SendChunkedPostRequestWithOneEmptyDataPacket) {
  SetRequest("POST", "/", DEFAULT_PRIORITY);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, !kFin, DEFAULT_PRIORITY,
                                         &spdy_request_headers_frame_length));
  AddWrite(ConstructDataPacket(2, kIncludeVersion, kFin, 0, ""));
  AddWrite(ConstructAckPacket(3, 3, 1));
  Initialize();

  ChunkedUploadDataStream upload_data_stream(0);

  request_.method = "POST";
  request_.url = GURL("http://www.example.org/");
  request_.upload_data_stream = &upload_data_stream;
  ASSERT_EQ(OK, request_.upload_data_stream->Init(
                    TestCompletionCallback().callback()));

  ASSERT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  ASSERT_EQ(ERR_IO_PENDING,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  upload_data_stream.AppendData(nullptr, 0, true);
  EXPECT_EQ(OK, callback_.WaitForResult());

  ProcessPacket(ConstructAckPacket(1, 0, 0));

  // Send the response headers (but not the body).
  SetResponse("200 OK", string());
  size_t spdy_response_headers_frame_length;
  ProcessPacket(ConstructResponseHeadersPacket(
      2, !kFin, &spdy_response_headers_frame_length));

  // The headers have arrived, but they are delivered asynchronously
  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));
  EXPECT_EQ(OK, callback_.WaitForResult());
  ASSERT_TRUE(response_.headers.get());
  EXPECT_EQ(200, response_.headers->response_code());
  EXPECT_TRUE(response_.headers->HasHeaderValue("Content-Type", "text/plain"));

  // Send the response body.
  const char kResponseBody[] = "Hello world!";
  ProcessPacket(ConstructDataPacket(3, false, kFin, response_data_.length(),
                                    kResponseBody));

  // The body has arrived, but it is delivered asynchronously
  ASSERT_EQ(static_cast<int>(strlen(kResponseBody)),
            stream_->ReadResponseBody(read_buffer_.get(), read_buffer_->size(),
                                      callback_.callback()));

  EXPECT_TRUE(stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_headers_frame_length),
            stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_headers_frame_length +
                                 strlen(kResponseBody)),
            stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, DestroyedEarly) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, kFin, DEFAULT_PRIORITY,
                                         &spdy_request_headers_frame_length));
  AddWrite(ConstructAckAndRstStreamPacket(2));
  use_closing_stream_ = true;
  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  EXPECT_EQ(OK,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  // Ack the request.
  ProcessPacket(ConstructAckPacket(1, 0, 0));
  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));

  // Send the response with a body.
  SetResponse("404 OK", "hello world!");
  // In the course of processing this packet, the QuicHttpStream close itself.
  ProcessPacket(ConstructResponseHeadersPacket(2, kFin, nullptr));

  base::MessageLoop::current()->RunUntilIdle();

  EXPECT_TRUE(AtEof());

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_headers_frame_length),
            stream_->GetTotalSentBytes());
  // Zero since the stream is closed before processing the headers.
  EXPECT_EQ(0, stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, Priority) {
  SetRequest("GET", "/", MEDIUM);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, kFin, MEDIUM,
                                         &spdy_request_headers_frame_length));
  AddWrite(ConstructAckAndRstStreamPacket(2));
  use_closing_stream_ = true;
  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK, stream_->InitializeStream(&request_, MEDIUM, net_log_.bound(),
                                          callback_.callback()));

  // Check that priority is highest.
  QuicChromiumClientStream* reliable_stream =
      QuicHttpStreamPeer::GetQuicChromiumClientStream(stream_.get());
  DCHECK(reliable_stream);
  DCHECK_EQ(kV3HighestPriority, reliable_stream->priority());

  EXPECT_EQ(OK,
            stream_->SendRequest(headers_, &response_, callback_.callback()));

  // Check that priority has now dropped back to MEDIUM.
  DCHECK_EQ(MEDIUM,
            ConvertQuicPriorityToRequestPriority(reliable_stream->priority()));

  // Ack the request.
  ProcessPacket(ConstructAckPacket(1, 0, 0));
  EXPECT_EQ(ERR_IO_PENDING, stream_->ReadResponseHeaders(callback_.callback()));

  // Send the response with a body.
  SetResponse("404 OK", "hello world!");
  // In the course of processing this packet, the QuicHttpStream close itself.
  ProcessPacket(ConstructResponseHeadersPacket(2, kFin, nullptr));

  base::MessageLoop::current()->RunUntilIdle();

  EXPECT_TRUE(AtEof());

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_headers_frame_length),
            stream_->GetTotalSentBytes());
  // Zero since the stream is closed before processing the headers.
  EXPECT_EQ(0, stream_->GetTotalReceivedBytes());
}

// Regression test for http://crbug.com/294870
TEST_P(QuicHttpStreamTest, CheckPriorityWithNoDelegate) {
  SetRequest("GET", "/", MEDIUM);
  use_closing_stream_ = true;

  AddWrite(ConstructRstStreamPacket(1));

  Initialize();

  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK, stream_->InitializeStream(&request_, MEDIUM, net_log_.bound(),
                                          callback_.callback()));

  // Check that priority is highest.
  QuicChromiumClientStream* reliable_stream =
      QuicHttpStreamPeer::GetQuicChromiumClientStream(stream_.get());
  DCHECK(reliable_stream);
  QuicChromiumClientStream::Delegate* delegate = reliable_stream->GetDelegate();
  DCHECK(delegate);
  DCHECK_EQ(kV3HighestPriority, reliable_stream->priority());

  // Set Delegate to nullptr and make sure Priority returns highest
  // priority.
  reliable_stream->SetDelegate(nullptr);
  DCHECK_EQ(kV3HighestPriority, reliable_stream->priority());
  reliable_stream->SetDelegate(delegate);

  EXPECT_EQ(0, stream_->GetTotalSentBytes());
  EXPECT_EQ(0, stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, SessionClosedBeforeSendHeadersComplete) {
  SetRequest("POST", "/", DEFAULT_PRIORITY);
  AddWrite(SYNCHRONOUS, ERR_FAILED);
  Initialize();

  ChunkedUploadDataStream upload_data_stream(0);

  request_.method = "POST";
  request_.url = GURL("http://www.example.org/");
  request_.upload_data_stream = &upload_data_stream;
  ASSERT_EQ(OK, request_.upload_data_stream->Init(
                    TestCompletionCallback().callback()));

  ASSERT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  ASSERT_EQ(ERR_QUIC_PROTOCOL_ERROR,
            stream_->SendRequest(headers_, &response_, callback_.callback()));
}

TEST_P(QuicHttpStreamTest, SessionClosedBeforeSendBodyComplete) {
  SetRequest("POST", "/", DEFAULT_PRIORITY);
  size_t spdy_request_headers_frame_length;
  AddWrite(ConstructRequestHeadersPacket(1, !kFin, DEFAULT_PRIORITY,
                                         &spdy_request_headers_frame_length));
  AddWrite(SYNCHRONOUS, ERR_FAILED);
  Initialize();

  ChunkedUploadDataStream upload_data_stream(0);
  size_t chunk_size = strlen(kUploadData);
  upload_data_stream.AppendData(kUploadData, chunk_size, false);

  request_.method = "POST";
  request_.url = GURL("http://www.example.org/");
  request_.upload_data_stream = &upload_data_stream;
  ASSERT_EQ(OK, request_.upload_data_stream->Init(
                    TestCompletionCallback().callback()));

  ASSERT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));
  ASSERT_EQ(ERR_QUIC_PROTOCOL_ERROR,
            stream_->SendRequest(headers_, &response_, callback_.callback()));
}

TEST_P(QuicHttpStreamTest, ServerPushGetRequest) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  Initialize();

  // Initialize the first stream, for receiving the promise on.
  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));

  // TODO(ckrasic) - could do this via constructing a PUSH_PROMISE
  // packet, but does it matter?
  ReceivePromise(promise_id_);
  EXPECT_NE(session_->GetPromisedByUrl(promise_url_), nullptr);

  request_.url = GURL(promise_url_);

  // Make the second stream that will exercise the first step of the
  // server push rendezvous mechanism.
  EXPECT_EQ(OK, promised_stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                                   net_log_.bound(),
                                                   callback_.callback()));

  // Receive the promised response headers.
  response_headers_ = promised_response_;
  size_t spdy_response_headers_frame_length;
  ProcessPacket(InnerConstructResponseHeadersPacket(
      1, promise_id_, false, &spdy_response_headers_frame_length));

  // Receive the promised response body.
  const char kResponseBody[] = "Hello world!";
  ProcessPacket(
      InnerConstructDataPacket(2, promise_id_, false, kFin, 0, kResponseBody));

  // Now sending a matching request will have successful rendezvous
  // with the promised stream.
  EXPECT_EQ(OK, promised_stream_->SendRequest(headers_, &response_,
                                              callback_.callback()));

  EXPECT_EQ(
      QuicHttpStreamPeer::GetQuicChromiumClientStream(promised_stream_.get())
          ->id(),
      promise_id_);

  // The headers will be immediately available.
  EXPECT_EQ(OK, promised_stream_->ReadResponseHeaders(callback_.callback()));

  // As will be the body.
  EXPECT_EQ(
      static_cast<int>(strlen(kResponseBody)),
      promised_stream_->ReadResponseBody(
          read_buffer_.get(), read_buffer_->size(), callback_.callback()));
  EXPECT_TRUE(promised_stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  EXPECT_EQ(0, stream_->GetTotalSentBytes());
  EXPECT_EQ(0, stream_->GetTotalReceivedBytes());
  EXPECT_EQ(0, promised_stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_headers_frame_length +
                                 strlen(kResponseBody)),
            promised_stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, ServerPushGetRequestSlowResponse) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  Initialize();

  // Initialize the first stream, for receiving the promise on.
  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));

  // TODO(ckrasic) - could do this via constructing a PUSH_PROMISE
  // packet, but does it matter?
  ReceivePromise(promise_id_);
  EXPECT_NE(session_->GetPromisedByUrl(promise_url_), nullptr);

  request_.url = GURL(promise_url_);

  // Make the second stream that will exercise the first step of the
  // server push rendezvous mechanism.
  EXPECT_EQ(OK, promised_stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                                   net_log_.bound(),
                                                   callback_.callback()));

  // Now sending a matching request will rendezvous with the promised
  // stream, but pending secondary validation.
  EXPECT_EQ(ERR_IO_PENDING, promised_stream_->SendRequest(
                                headers_, &response_, callback_.callback()));

  // Receive the promised response headers.
  response_headers_ = promised_response_;
  size_t spdy_response_headers_frame_length;
  ProcessPacket(InnerConstructResponseHeadersPacket(
      1, promise_id_, false, &spdy_response_headers_frame_length));

  // Receive the promised response body.
  const char kResponseBody[] = "Hello world!";
  ProcessPacket(
      InnerConstructDataPacket(2, promise_id_, false, kFin, 0, kResponseBody));

  base::MessageLoop::current()->RunUntilIdle();

  // Rendezvous should have succeeded now, so the promised stream
  // should point at our push stream, and we should be able read
  // headers and data from it.
  EXPECT_EQ(OK, callback_.WaitForResult());

  EXPECT_EQ(
      QuicHttpStreamPeer::GetQuicChromiumClientStream(promised_stream_.get())
          ->id(),
      promise_id_);

  EXPECT_EQ(OK, promised_stream_->ReadResponseHeaders(callback_.callback()));

  EXPECT_EQ(
      static_cast<int>(strlen(kResponseBody)),
      promised_stream_->ReadResponseBody(
          read_buffer_.get(), read_buffer_->size(), callback_.callback()));

  // Callback should return
  EXPECT_TRUE(promised_stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  EXPECT_EQ(0, stream_->GetTotalSentBytes());
  EXPECT_EQ(0, stream_->GetTotalReceivedBytes());
  EXPECT_EQ(0, promised_stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_headers_frame_length +
                                 strlen(kResponseBody)),
            promised_stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, ServerPushCrossOriginOK) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  Initialize();

  // Initialize the first stream, for receiving the promise on.
  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));

  // TODO(ckrasic) - could do this via constructing a PUSH_PROMISE
  // packet, but does it matter?

  push_promise_[":authority"] = "mail.example.org";
  promise_url_ = SpdyUtils::GetUrlFromHeaderBlock(push_promise_);
  serialized_push_promise_ =
      SpdyUtils::SerializeUncompressedHeaders(push_promise_);

  ReceivePromise(promise_id_);
  EXPECT_NE(session_->GetPromisedByUrl(promise_url_), nullptr);

  request_.url = GURL(promise_url_);

  // Make the second stream that will exercise the first step of the
  // server push rendezvous mechanism.
  EXPECT_EQ(OK, promised_stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                                   net_log_.bound(),
                                                   callback_.callback()));

  // Receive the promised response headers.
  response_headers_ = promised_response_;
  size_t spdy_response_headers_frame_length;
  ProcessPacket(InnerConstructResponseHeadersPacket(
      1, promise_id_, false, &spdy_response_headers_frame_length));

  // Receive the promised response body.
  const char kResponseBody[] = "Hello world!";
  ProcessPacket(
      InnerConstructDataPacket(2, promise_id_, false, kFin, 0, kResponseBody));

  // Now sending a matching request will have successful rendezvous
  // with the promised stream.
  EXPECT_EQ(OK, promised_stream_->SendRequest(headers_, &response_,
                                              callback_.callback()));

  EXPECT_EQ(
      QuicHttpStreamPeer::GetQuicChromiumClientStream(promised_stream_.get())
          ->id(),
      promise_id_);

  // The headers will be immediately available.
  EXPECT_EQ(OK, promised_stream_->ReadResponseHeaders(callback_.callback()));

  // As will be the body.
  EXPECT_EQ(
      static_cast<int>(strlen(kResponseBody)),
      promised_stream_->ReadResponseBody(
          read_buffer_.get(), read_buffer_->size(), callback_.callback()));
  EXPECT_TRUE(promised_stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  EXPECT_EQ(0, stream_->GetTotalSentBytes());
  EXPECT_EQ(0, stream_->GetTotalReceivedBytes());
  EXPECT_EQ(0, promised_stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_headers_frame_length +
                                 strlen(kResponseBody)),
            promised_stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, ServerPushCrossOriginFail) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  Initialize();

  // Initialize the first stream, for receiving the promise on.
  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));

  // TODO(ckrasic) - could do this via constructing a PUSH_PROMISE
  // packet, but does it matter?
  push_promise_[":authority"] = "www.notexample.org";
  promise_url_ = SpdyUtils::GetUrlFromHeaderBlock(push_promise_);
  serialized_push_promise_ =
      SpdyUtils::SerializeUncompressedHeaders(push_promise_);

  ReceivePromise(promise_id_);
  // The promise will have been rejected because the cert doesn't
  // match.
  EXPECT_EQ(session_->GetPromisedByUrl(promise_url_), nullptr);
}

TEST_P(QuicHttpStreamTest, ServerPushVaryCheckOK) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  Initialize();

  // Initialize the first stream, for receiving the promise on.
  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));

  push_promise_["accept-encoding"] = "gzip";
  serialized_push_promise_ =
      SpdyUtils::SerializeUncompressedHeaders(push_promise_);

  // TODO(ckrasic) - could do this via constructing a PUSH_PROMISE
  // packet, but does it matter?
  ReceivePromise(promise_id_);
  EXPECT_NE(session_->GetPromisedByUrl(promise_url_), nullptr);

  request_.url = GURL(promise_url_);

  // Make the second stream that will exercise the first step of the
  // server push rendezvous mechanism.
  EXPECT_EQ(OK, promised_stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                                   net_log_.bound(),
                                                   callback_.callback()));

  headers_.SetHeader("accept-encoding", "gzip");

  // Now sending a matching request will rendezvous with the promised
  // stream, but pending secondary validation.
  EXPECT_EQ(ERR_IO_PENDING, promised_stream_->SendRequest(
                                headers_, &response_, callback_.callback()));

  // Receive the promised response headers.
  promised_response_["vary"] = "accept-encoding";
  response_headers_ = promised_response_;
  size_t spdy_response_headers_frame_length;
  ProcessPacket(InnerConstructResponseHeadersPacket(
      1, promise_id_, false, &spdy_response_headers_frame_length));

  // Receive the promised response body.
  const char kResponseBody[] = "Hello world!";
  ProcessPacket(
      InnerConstructDataPacket(2, promise_id_, false, kFin, 0, kResponseBody));

  base::MessageLoop::current()->RunUntilIdle();

  // Rendezvous should have succeeded now, so the promised stream
  // should point at our push stream, and we should be able read
  // headers and data from it.
  EXPECT_EQ(OK, callback_.WaitForResult());

  EXPECT_EQ(
      QuicHttpStreamPeer::GetQuicChromiumClientStream(promised_stream_.get())
          ->id(),
      promise_id_);

  EXPECT_EQ(OK, promised_stream_->ReadResponseHeaders(callback_.callback()));

  EXPECT_EQ(
      static_cast<int>(strlen(kResponseBody)),
      promised_stream_->ReadResponseBody(
          read_buffer_.get(), read_buffer_->size(), callback_.callback()));

  // Callback should return
  EXPECT_TRUE(promised_stream_->IsResponseBodyComplete());
  EXPECT_TRUE(AtEof());

  EXPECT_EQ(0, stream_->GetTotalSentBytes());
  EXPECT_EQ(0, stream_->GetTotalReceivedBytes());
  EXPECT_EQ(0, promised_stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_headers_frame_length +
                                 strlen(kResponseBody)),
            promised_stream_->GetTotalReceivedBytes());
}

TEST_P(QuicHttpStreamTest, ServerPushVaryCheckFail) {
  SetRequest("GET", "/", DEFAULT_PRIORITY);
  request_headers_[":scheme"] = "https";
  request_headers_[":path"] = "/bar";
  request_headers_["accept-encoding"] = "sdch";

  size_t spdy_request_header_frame_length;
  AddWrite(ConstructRstStreamVaryMismatchPacket(1));
  AddWrite(InnerConstructRequestHeadersPacket(
      2, stream_id_ + 2, !kIncludeVersion, kFin, DEFAULT_PRIORITY,
      &spdy_request_header_frame_length));
  AddWrite(ConstructAckPacket(3, 3, 1));
  AddWrite(ConstructRstStreamCancelledPacket(4));
  Initialize();

  // Initialize the first stream, for receiving the promise on.
  request_.method = "GET";
  request_.url = GURL("http://www.example.org/");

  EXPECT_EQ(OK,
            stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                      net_log_.bound(), callback_.callback()));

  push_promise_["accept-encoding"] = "gzip";
  serialized_push_promise_ =
      SpdyUtils::SerializeUncompressedHeaders(push_promise_);

  // TODO(ckrasic) - could do this via constructing a PUSH_PROMISE
  // packet, but does it matter?
  ReceivePromise(promise_id_);
  EXPECT_NE(session_->GetPromisedByUrl(promise_url_), nullptr);

  request_.url = GURL(promise_url_);

  // Make the second stream that will exercise the first step of the
  // server push rendezvous mechanism.
  EXPECT_EQ(OK, promised_stream_->InitializeStream(&request_, DEFAULT_PRIORITY,
                                                   net_log_.bound(),
                                                   callback_.callback()));

  headers_.SetHeader("accept-encoding", "sdch");

  // Now sending a matching request will rendezvous with the promised
  // stream, but pending secondary validation.
  EXPECT_EQ(ERR_IO_PENDING, promised_stream_->SendRequest(
                                headers_, &response_, callback_.callback()));

  // Receive the promised response headers.
  promised_response_["vary"] = "accept-encoding";
  response_headers_ = promised_response_;
  size_t spdy_response_headers_frame_length;
  ProcessPacket(InnerConstructResponseHeadersPacket(
      1, promise_id_, false, &spdy_response_headers_frame_length));

  base::MessageLoop::current()->RunUntilIdle();

  // Rendezvous should have failed due to vary mismatch, so the
  // promised stream should have been aborted, and instead we have a
  // new, regular client initiated stream.
  EXPECT_EQ(OK, callback_.WaitForResult());

  // Not a server-initiated stream.
  EXPECT_NE(
      QuicHttpStreamPeer::GetQuicChromiumClientStream(promised_stream_.get())
          ->id(),
      promise_id_);

  // Instead, a new client-initiated stream.
  EXPECT_EQ(
      QuicHttpStreamPeer::GetQuicChromiumClientStream(promised_stream_.get())
          ->id(),
      stream_id_ + 2);

  // After rendezvous failure, the push stream has been cancelled.
  EXPECT_EQ(session_->GetPromisedByUrl(promise_url_), nullptr);

  // The rest of the test verifies that the retried as
  // client-initiated version of |promised_stream_| works as intended.

  // Ack the request.
  ProcessPacket(ConstructAckPacket(2, 0, 0));

  SetResponse("404 Not Found", string());
  size_t spdy_response_header_frame_length;
  ProcessPacket(InnerConstructResponseHeadersPacket(
      3, stream_id_ + 2, kFin, &spdy_response_header_frame_length));

  base::MessageLoop::current()->RunUntilIdle();

  EXPECT_EQ(OK, promised_stream_->ReadResponseHeaders(callback_.callback()));
  ASSERT_TRUE(response_.headers.get());
  EXPECT_EQ(404, response_.headers->response_code());
  EXPECT_TRUE(response_.headers->HasHeaderValue("Content-Type", "text/plain"));
  EXPECT_FALSE(response_.response_time.is_null());
  EXPECT_FALSE(response_.request_time.is_null());

  // There is no body, so this should return immediately.
  EXPECT_EQ(
      0, promised_stream_->ReadResponseBody(
             read_buffer_.get(), read_buffer_->size(), callback_.callback()));
  EXPECT_TRUE(promised_stream_->IsResponseBodyComplete());

  stream_->Close(true);

  EXPECT_TRUE(AtEof());

  // QuicHttpStream::GetTotalSent/ReceivedBytes currently only includes the
  // headers and payload.
  EXPECT_EQ(static_cast<int64_t>(spdy_request_header_frame_length),
            promised_stream_->GetTotalSentBytes());
  EXPECT_EQ(static_cast<int64_t>(spdy_response_header_frame_length),
            promised_stream_->GetTotalReceivedBytes());
}

}  // namespace test
}  // namespace net
