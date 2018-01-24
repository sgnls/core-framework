/*
 * This file is protected by Copyright. Please refer to the COPYRIGHT file
 * distributed with this source distribution.
 *
 * This file is part of REDHAWK bulkioInterfaces.
 *
 * REDHAWK bulkioInterfaces is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * REDHAWK bulkioInterfaces is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

#include "OutPortTest.h"

// Suppress warnings for access to deprecated methods
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

// Global connection/disconnection callbacks
static void port_connected(const char* connectionId)
{
}

static void port_disconnected(const char* connectionId)
{
}

template <class Port>
void OutPortTest<Port>::testLegacyAPI()
{
    port->setNewConnectListener(&port_connected);
    port->setNewDisconnectListener(&port_disconnected);

    typename Port::ConnectionsList cl = port->_getConnections();
    std::string sid="none";
    CPPUNIT_ASSERT(port->getCurrentSRI().count(sid) == 0);

    port->enableStats(false);

    rh_logger::LoggerPtr logger = rh_logger::Logger::getLogger("BulkioOutPort");
    port->setLogger(logger);
}

template <class Port>
void OutPortTest<Port>::testConnections()
{
    // Should start with one connection, to the in port stub
    ExtendedCF::UsesConnectionSequence_var connections = port->connections();
    CPPUNIT_ASSERT(connections->length() == 1);
    CPPUNIT_ASSERT(port->state() == BULKIO::ACTIVE);

    // Should throw an invalid port on a nil
    CORBA::Object_var objref;
    CPPUNIT_ASSERT_THROW(port->connectPort(objref, "connection_nil"), CF::Port::InvalidPort);

    // Normal connection
    StubType* stub2 = this->_createStub();
    objref = stub2->_this();
    port->connectPort(objref, "connection_2");
    connections = port->connections();
    CPPUNIT_ASSERT(connections->length() == 2);

    // Cannot reuse connection ID
    CPPUNIT_ASSERT_THROW(port->connectPort(objref, "connection_2"), CF::Port::OccupiedPort);

    // Disconnect second connection
    port->disconnectPort("connection_2");
    connections = port->connections();
    CPPUNIT_ASSERT(connections->length() == 1);

    // Bad connection ID on disconnect
    CPPUNIT_ASSERT_THROW(port->disconnectPort("connection_bad"), CF::Port::InvalidPort);

    // Disconnect the default stub; port should go to idle
    port->disconnectPort("test_connection");
    connections = port->connections();
    CPPUNIT_ASSERT(connections->length() == 0);
    CPPUNIT_ASSERT(port->state() == BULKIO::IDLE);
}

template <class Port>
void OutPortTest<Port>::testStatistics()
{
    const char* stream_id = "port_stats";

    BULKIO::UsesPortStatisticsSequence_var uses_stats = port->statistics();
    CPPUNIT_ASSERT_EQUAL((CORBA::ULong) 1, uses_stats->length());
    CPPUNIT_ASSERT_EQUAL(std::string("test_connection"), std::string(uses_stats[0].connectionId));

    BULKIO::StreamSRI sri = bulkio::sri::create();
    port->pushSRI(sri);

    this->_pushTestPacket(1024, BULKIO::PrecisionUTCTime(), false, stream_id);

    uses_stats = port->statistics();
    CPPUNIT_ASSERT_EQUAL((CORBA::ULong) 1, uses_stats->length());
    const BULKIO::PortStatistics& stats = uses_stats[0].statistics;

    CPPUNIT_ASSERT(stats.elementsPerSecond > 0.0);
    size_t bits_per_element = round(stats.bitsPerSecond / stats.elementsPerSecond);
    //CPPUNIT_ASSERT_EQUAL(8 * sizeof(typename Port::NativeType), bits_per_element);
}

template <class Port>
void OutPortTest<Port>::testMultiOut()
{
    StubType* stub2 = this->_createStub();
    CORBA::Object_var objref = stub2->_this();
    port->connectPort(objref, "connection_2");

    // Set up a connection table that only routes the filtered stream to the
    // second stub, and another stream to both connections
    const std::string filter_stream_id = "filter_stream";
    _addStreamFilter(filter_stream_id, "connection_2");
    const std::string all_stream_id = "all_stream";
    _addStreamFilter(all_stream_id, "test_connection");
    _addStreamFilter(all_stream_id, "connection_2");

    // Push an SRI for the filtered stream; it should only be received by the
    // second stub
    BULKIO::StreamSRI sri = bulkio::sri::create(filter_stream_id, 2.5e6);
    port->pushSRI(sri);
    CPPUNIT_ASSERT(stub->H.empty());
    CPPUNIT_ASSERT(stub2->H.size() == 1);
    CPPUNIT_ASSERT_EQUAL(filter_stream_id, std::string(stub2->H.back().streamID));

    // Push a packet for the filtered stream; again, only received by #2
    this->_pushTestPacket(91, bulkio::time::utils::now(), false, filter_stream_id);
    CPPUNIT_ASSERT(stub->packets.empty());
    CPPUNIT_ASSERT(stub2->packets.size() == 1);
    CPPUNIT_ASSERT_EQUAL((size_t) 91, stub2->packets.back().size());

    // Unknown (to the connection filter) stream should get dropped
    const std::string unknown_stream_id = "unknown_stream";
    sri = bulkio::sri::create(unknown_stream_id);
    port->pushSRI(sri);
    CPPUNIT_ASSERT(stub->H.empty());
    CPPUNIT_ASSERT(stub2->H.size() == 1);
    this->_pushTestPacket(50, bulkio::time::utils::now(), false, unknown_stream_id);
    CPPUNIT_ASSERT(stub->packets.empty());
    CPPUNIT_ASSERT(stub2->packets.size() == 1);

    // Check SRI routed to both connections...
    sri = bulkio::sri::create(all_stream_id, 1e6);
    port->pushSRI(sri);
    CPPUNIT_ASSERT(stub->H.size() == 1);
    CPPUNIT_ASSERT(stub2->H.size() == 2);
    CPPUNIT_ASSERT_EQUAL(all_stream_id, std::string(stub->H.back().streamID));
    CPPUNIT_ASSERT_EQUAL(all_stream_id, std::string(stub2->H.back().streamID));

    // ...and data
    this->_pushTestPacket(256, bulkio::time::utils::now(), false, all_stream_id);
    CPPUNIT_ASSERT(stub->packets.size() == 1);
    CPPUNIT_ASSERT_EQUAL((size_t) 256, stub->packets.back().size());
    CPPUNIT_ASSERT(stub2->packets.size() == 2);
    CPPUNIT_ASSERT_EQUAL((size_t) 256, stub2->packets.back().size());

    // Reset the connection filter and push data for the filtered stream again,
    // which should trigger an SRI push to the first stub
    connectionTable.clear();
    port->updateConnectionFilter(connectionTable);
    this->_pushTestPacket(9, bulkio::time::utils::now(), false, filter_stream_id);
    CPPUNIT_ASSERT(stub->H.size() == 2);
    CPPUNIT_ASSERT_EQUAL(filter_stream_id, std::string(stub->H.back().streamID));
    CPPUNIT_ASSERT(stub->packets.size() == 2);
    CPPUNIT_ASSERT_EQUAL((size_t) 9, stub->packets.back().size());
    CPPUNIT_ASSERT(stub2->packets.size() == 3);
    CPPUNIT_ASSERT_EQUAL((size_t) 9, stub2->packets.back().size());
}

template <class Port>
void OutPortTest<Port>::_addStreamFilter(const std::string& streamId, const std::string& connectionId)
{
    bulkio::connection_descriptor_struct desc;
    desc.stream_id = streamId;
    desc.connection_id = connectionId;
    desc.port_name = port->getName();
    connectionTable.push_back(desc);
    port->updateConnectionFilter(connectionTable);
}

template <class Port>
void ChunkingOutPortTest<Port>::testPushChunking()
{
    // Set up a basic stream
    const char* stream_id = "push_chunking";
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    sri.xdelta = 0.125;
    port->pushSRI(sri);

    // Test that the push is properly chunked
    BULKIO::PrecisionUTCTime time = bulkio::time::utils::create(0, 0);
    _testPushOversizedPacket(time, false, stream_id);

    // Check that the synthesized time stamp(s) advanced by the expected time
    for (size_t index = 1; index < stub->packets.size(); ++index) {
        double expected = stub->packets[index-1].size() * sri.xdelta;
        double elapsed = stub->packets[index].T - stub->packets[index-1].T;
        CPPUNIT_ASSERT_EQUAL_MESSAGE("Incorrect time stamp delta", expected, elapsed);
    }
}

template <class Port>
void ChunkingOutPortTest<Port>::testPushChunkingEOS()
{
    // Set up a basic stream
    const char* stream_id = "push_chunking_eos";
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    port->pushSRI(sri);

    // Send a packet with end-of-stream set
    _testPushOversizedPacket(BULKIO::PrecisionUTCTime(), true, stream_id);

    // Check that only the final packet has end-of-stream set
    CPPUNIT_ASSERT_MESSAGE("Last packet does not have EOS set", stub->packets.back().EOS);
    for (size_t index = 0; index < (stub->packets.size() - 1); ++index) {
        CPPUNIT_ASSERT_MESSAGE("Intermediate packet has EOS set", !stub->packets[index].EOS);
    }
}

template <class Port>
void ChunkingOutPortTest<Port>::testPushChunkingSubsize()
{
    // Set up a 2-dimensional stream
    const char* stream_id = "push_chunking_subsize";
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    sri.subsize = 1023;
    port->pushSRI(sri);

    this->_testPushOversizedPacket(BULKIO::PrecisionUTCTime(), false, stream_id);

    // Check that each packet is a multiple of the subsize (except the last,
    // because the oversized packet was not explicitly quantized to be an exact
    // multiple)
    for (size_t index = 0; index < (stub->packets.size() - 1); ++index) {
        CPPUNIT_ASSERT_MESSAGE("Packet size is not a multiple of subsize", (stub->packets[index].size() % 1023) == 0);
    }
}

template <class Port>
void ChunkingOutPortTest<Port>::_testPushOversizedPacket(const BULKIO::PrecisionUTCTime& time,
                                                         bool eos, const std::string& streamID)
{
    // Pick a sufficiently large number of samples that the packet has to span
    // multiple packets
    const size_t max_bits = 8 * bulkio::Const::MaxTransferBytes();
    const size_t bits_per_element = bulkio::NativeTraits<CorbaType>::bits;
    const size_t count = 2 * max_bits / bits_per_element;
    this->_pushTestPacket(count, time, eos, streamID);

    // More than one packet must have been received, and no packet can exceed
    // the max transfer size
    CPPUNIT_ASSERT(stub->packets.size() > 1);
    for (size_t index = 0; index < stub->packets.size(); ++index) {
        size_t packet_bits = stub->packets[index].size() * bits_per_element;
        CPPUNIT_ASSERT_MESSAGE("Packet too large", packet_bits < max_bits);
    }
}

template <class Port>
void NumericOutPortTest<Port>::testPushPointer()
{
    const char* stream_id = "push_pointer";
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    port->pushSRI(sri);

    NativeType buffer[128];
    port->pushPacket(buffer, 128, BULKIO::PrecisionUTCTime(), false, stream_id);
    CPPUNIT_ASSERT(stub->packets.size() == 1);
    CPPUNIT_ASSERT_EQUAL((size_t) 128, stub->packets.back().size());
}

template <class Port>
void NumericOutPortTest<Port>::testPushChunkingComplex()
{
    // Set up a complex stream
    const char* stream_id = "push_chunking_complex";
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    sri.mode = 1;
    sri.xdelta = 0.0625;
    port->pushSRI(sri);

    // Test that the push is properly chunked
    BULKIO::PrecisionUTCTime time = bulkio::time::utils::create(0, 0);
    this->_testPushOversizedPacket(time, false, stream_id);

    // Check that each packet contains an even number of samples (i.e., no
    // complex value was split)
    for (size_t index = 0; index < stub->packets.size(); ++index) {
        CPPUNIT_ASSERT_MESSAGE("Packet contains a partial complex value", (stub->packets[index].size() % 2) == 0);
    }

    // Check that the synthesized time stamp(s) advanced by the expected time
    for (size_t index = 1; index < stub->packets.size(); ++index) {
        double expected = stub->packets[index-1].size() * 0.5 * sri.xdelta;
        double elapsed = stub->packets[index].T - stub->packets[index-1].T;
        CPPUNIT_ASSERT_EQUAL_MESSAGE("Incorrect time stamp delta", expected, elapsed);
    }
}

template <class Port>
void NumericOutPortTest<Port>::testPushChunkingSubsizeComplex()
{
    // Set up a 2-dimensional complex stream
    const char* stream_id = "push_chunking_subsize_complex";
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    sri.subsize = 2048;
    sri.mode = 1;
    port->pushSRI(sri);

    this->_testPushOversizedPacket(BULKIO::PrecisionUTCTime(), false, stream_id);

    // Check that each packet is a multiple of the subsize (except the last,
    // because the oversized packet was not explicitly quantized to be an exact
    // multiple)
    for (size_t index = 0; index < (stub->packets.size() - 1); ++index) {
        CPPUNIT_ASSERT_MESSAGE("Packet size is not a multiple of subsize", (stub->packets[index].size() % 4096) == 0);
    }
}

class OutCharPortTest : public NumericOutPortTest<bulkio::OutCharPort>
{
    typedef NumericOutPortTest<bulkio::OutCharPort> TestBase;

    CPPUNIT_TEST_SUB_SUITE(OutCharPortTest, TestBase);
    CPPUNIT_TEST(testPushChar);
    CPPUNIT_TEST_SUITE_END();

public:
    void testPushChar()
    {
        // Test for char* overloads of pushPacket
        const char* stream_id = "push_char";
        BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
        port->pushSRI(sri);

        std::vector<char> vec;
        port->pushPacket(vec, BULKIO::PrecisionUTCTime(), false, stream_id);
        CPPUNIT_ASSERT(stub->packets.size() == 1);
        CPPUNIT_ASSERT_EQUAL(vec.size(), stub->packets.back().size());

        char buffer[100];
        port->pushPacket(buffer, sizeof(buffer), BULKIO::PrecisionUTCTime(), false, stream_id);
        CPPUNIT_ASSERT(stub->packets.size() == 2);
        CPPUNIT_ASSERT_EQUAL(sizeof(buffer), stub->packets.back().size());
    }

protected:
    using TestBase::port;
    using TestBase::stub;
};



#define CREATE_TEST(x,BASE)                                             \
    class Out##x##PortTest : public BASE<bulkio::Out##x##Port>          \
    {                                                                   \
        CPPUNIT_TEST_SUB_SUITE(Out##x##PortTest, BASE<bulkio::Out##x##Port>); \
        CPPUNIT_TEST_SUITE_END();                                       \
    };                                                                  \
    CPPUNIT_TEST_SUITE_REGISTRATION(Out##x##PortTest);

#define CREATE_BASIC_TEST(x) CREATE_TEST(x,OutPortTest)
#define CREATE_NUMERIC_TEST(x) CREATE_TEST(x,NumericOutPortTest)

CREATE_NUMERIC_TEST(Octet);
CREATE_NUMERIC_TEST(Short);
CREATE_NUMERIC_TEST(UShort);
CREATE_NUMERIC_TEST(Long);
CREATE_NUMERIC_TEST(ULong);
CREATE_NUMERIC_TEST(LongLong);
CREATE_NUMERIC_TEST(ULongLong);
CREATE_NUMERIC_TEST(Float);
CREATE_NUMERIC_TEST(Double);
CREATE_TEST(Bit, ChunkingOutPortTest);
CREATE_BASIC_TEST(XML);
CREATE_BASIC_TEST(File);
